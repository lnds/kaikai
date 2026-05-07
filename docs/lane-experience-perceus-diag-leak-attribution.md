# Lane experience — perceus-diag-leak-attribution

**Lane:** `perceus-diag-leak-attribution`
**Date:** 2026-05-06
**Goal:** Per-leak-site attribution mapping every leaked chunk to the
kaikai source function it allocated under, ranked top-20, as input for
Lane FIX. Diagnosis only — no leak fix.

## Objective metrics

- **Start:** 2026-05-06T19:53:45-04:00
- **End:** 2026-05-06T20:19:38-04:00
- **Wall clock:** ~26 min (vs 1-1.5h budget — under by ~50%)
- **Lines added:** ~210 in `stage0/runtime.h`, ~7 in
  `stage1/compiler.kai`, ~150 in `docs/perceus-leak-attribution.md`.
- **Builds during lane (TSV):** tier0 OK, tier1 OK, tier1-asan OK,
  selfhost-llvm OK. Selfhost byte-identical on both backends; vanilla
  builds zero overhead.

## The instrumentation

### Data structure (runtime side)

Bajo `#ifdef KAI_TRACE_RC_LEAKSITE`:

- One added field on `KaiValue`: `const char *scope_fn`. Vanilla and
  even `KAI_TRACE_RC` builds keep the original layout (the field sits
  inside its own `#ifdef`, not inside `KAI_TRACE_RC`).
- `static const char *kai_current_scope_fn = "<root>"` + helper
  `kai_set_scope_fn(name)`. Static, not `_Thread_local`: the kaic2
  self-compile is single-threaded.
- `KaiLeakSite[8192]` open-addressed hash keyed on `(scope_fn, tag)`.
  8192 buckets × ~16 B = 128 KiB fixed cost. Empirically observed
  load: 1,092 entries (13.3% load factor; well under saturation).
- `kai_leaksite_record_alloc` / `kai_leaksite_record_free` in
  `kai_alloc_traced` / `kai_free_value`, gated under the new
  `#ifdef`.
- `atexit(kai_leaksite_report)` qsorts by leak count and prints
  rank, alloc_fn, scope_fn, allocs, frees, leak, leak%, share% +
  per-tag totals.

### Emitter hook (stage 1 side)

Single edit inside `emit_fn_body`:

```kai
let scope_hook = concat_all([
  "#ifdef KAI_TRACE_RC_LEAKSITE\n    kai_set_scope_fn(\"",
  name,
  "\");\n#endif\n    "
])
```

Inserted between the function's `{` and the body's `return`. The
emitted call is C-side gated, so vanilla builds compile straight
through — the lines are present but inert. kaic1 produces 2,526
hooks for stage2/compiler.kai, one per emitted kaikai fn.

### Why no per-chunk side-table

The plan suggested a parallel hash map keyed on chunk pointer. I
rejected that for two reasons:

1. Capacity. 21M chunks × 48B = ~1 GB on top of the existing 3 GB
   self-compile RSS. Manageable but wasteful.
2. The capacity cap proposed (`KAI_TRACE_RC_LEAKSITE_LIMIT=5M`)
   would have skewed Checkpoint 2: per-alloc_fn totals would no
   longer match per-tag totals.

Instead I added a single pointer field on `KaiValue` and aggregated
on the fly. Per-chunk overhead: 8 B × 21M = ~170 MB, paid only when
the `#ifdef` is active. Aggregate is constant-size.

## Checkpoint 1 result

Required: top-10 contains ≥ 3 sites with `count > 500,000`.

Observed: **10 sites > 500,000** in the top-10. Top-1 is 1,280,880
(prelude_table+kai_str). Pass.

## Checkpoint 2 result

Required: per-alloc_fn totals match per-tag totals within 10%.

Observed: **exact match** chunk-for-chunk. Summing leak counts in
the full agg (1,092 sites) by alloc_fn:

| alloc_fn    | sum_all   | per-tag total | delta  |
|:------------|----------:|--------------:|-------:|
| kai_record  | 6,871,548 |     6,871,548 | 0%     |
| kai_variant | 5,847,644 |     5,847,644 | 0%     |
| kai_cons    | 5,361,788 |     5,361,788 | 0%     |
| kai_str     | 1,969,740 |     1,969,740 | 0%     |
| kai_int     |   557,880 |       557,880 | 0%     |
| kai_closure |   484,477 |       484,477 | 0%     |
| kai_array   |     8,274 |         8,274 | 0%     |
| kai_char    |     4,528 |         4,528 | 0%     |
| kai_real    |         2 |             2 | 0%     |

Total: 21,105,881 = `STRICT leaked`. The instrumentation is
exhaustive (no cap, no dropped chunks). Pass.

## Top-3 dominant scope_fns (precomputed for Lane FIX)

1. **`prelude_table`** — 2,561,701 leak (12.1%) across kai_str,
   kai_variant, kai_cons. All-or-nothing leak% = 100% — looks like
   build-once / never-decref'd table that should either be
   immortal-marked or torn down explicitly.
2. **`mk_expr` + `map_expr_kind`** — 3,788,697 leak (17.9%) across
   kai_record, kai_variant, kai_cons. Hot AST construction and
   kind-mapping path; emit shape shared across many syntactic forms
   so a single fix propagates.
3. **`list_has` + `tok`** — 2,851,492 leak (13.5%). `tok` shows
   three rows in the top-20, all near 100% leak%; lexer's inner
   loop not draining intermediate values.

Combined: **9,201,890 / 21,105,881 = 43.6%** of total leak. Per the
brief, Lane FIX should aim ≥ 20% per-tag delta per batch — patching
any one of these clusters meets that bar.

## Friction points

- Initial concern: scope_fn attributes to allocator, not consumer.
  This is a real limitation of the methodology; documented in the
  Caveats section of the doc. Lane FIX must read the actual emit
  shape next to the ranking to decide whether the allocator or a
  consumer is at fault.
- Pre-existing LSP diagnostics fired on lines 3086+ of runtime.h
  (anonymous-struct + `void *` initializer). Not introduced by this
  lane; clang-tooling reads the file in C++ mode but `cc` compiles
  it fine as C99. Ignored.
- Time was 50% under budget primarily because the `KAI_TRACE_RC`
  infrastructure (#291 Track #2 + #296 site-attribution) had already
  paved everything: macros, dump scaffolding, gates. The new layer
  is a thin variation on existing patterns, not a from-scratch
  build.

## Subjective summary

The data-driven approach Linus + Eric called for produced clear
findings on the first run. The top-3 scope_fns concentrate 43.6% of
the leak, and the per-tag breakdown (record 6.9M, variant 5.8M,
cons 5.4M dominate) ranks them in the same order I'd guess from the
ranking — they line up. Lane FIX has a small, well-bounded set of
emit shapes to inspect.

Two design choices I'd flag for review:

1. **One pointer field on KaiValue** instead of a side hash map. Saved
   memory and complexity, made Checkpoint 2 trivially exact. The plan
   pushed for the side-table; the simpler structure was strictly
   better here.
2. **`static`, not `_Thread_local`.** The plan said TLS; the
   self-compile is single-threaded so plain static suffices and is
   `c99`-portable. If a future lane introduces parallelism in the
   compiler this must change.

## Limitations

- Attribution is to the kaikai fn that *allocated* the chunk, not to
  the fn whose emit shape is buggy. These often coincide — a fn
  forgetting a temp-scope drop is its own bug — but they can also
  diverge: a fn that legitimately returns a chunk gets blamed for the
  consumer's missing decref. Lane FIX must inspect emit shapes
  before patching.
- `prelude_table` allocations may be intentional immortals (build-
  once strings + variants) that just aren't flagged as singletons.
  Lane FIX must distinguish "missing drop" from "should be
  immortal".
- Hash size is 8,192 buckets, hardcoded. If a future kaikai program
  exceeds this in `(scope_fn, tag)` cardinality the table will
  saturate and silently drop sites; the dump emits "TABLE SATURATED"
  if it does. Not observed here (load = 13%).

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-06T20:11:09-04:00	tier0	OK	-
2026-05-06T20:17:17-04:00	tier1	OK	-
2026-05-06T20:18:22-04:00	tier1-asan	OK	-
2026-05-06T20:19:27-04:00	selfhost-llvm	OK	-
```

(Selfhost-C byte-identical was checked by `make tier0`, which
ends with that gate; the line above logs the LLVM equivalent.)
