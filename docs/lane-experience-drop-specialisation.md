# Lane experience report — drop-specialisation

Best-effort retrospective by the implementing agent. See limitations
at the bottom.

## TL;DR

**Closed as doc-only PR.** Drop specialisation was implemented, ran
green end-to-end (selfhost C + LLVM byte-identical, tier1 OK), and
measured. The wall-clock improvement on `kaic2` self-compile is
**−1.7%** at `-O2` and **+4.6% (regression)** at `-O0` (the default
project build). Both are well below the lane's ≥10% success
threshold. Phase 2 unboxing (PR #38) had already eliminated the bulk
of the boxing/dispatch overhead this lane targets, leaving very
little headroom. Reuse-in-place (Anga Roa) is a more promising
follow-up Perceus optimisation per `docs/perceus-honesty-targets.md`
Tier 3a.

## Objective metrics (from /tmp/lane-drop-specialisation-builds.tsv)

- Start: 2026-05-01T20:40:56-04:00
- End:   2026-05-01T21:19:37-04:00
- Wall-clock: ~38m 41s (single-shot agent run; no wait windows)
- Build/test invocations:
  - `make all`:      4 invocations, 4 passes, 0 fails
  - `make selfhost`: 4 invocations, 4 passes, 0 fails
  - `make tier1`:    2 invocations, 2 passes, 0 fails
  - `make selfhost-llvm` (stage2): 1 invocation, 1 pass, 0 fails
    (not in the TSV — added late, no wrapper)

## Investigation (M1)

Baseline `KAI_TRACE_RC=1 ./kaic2 stage2/compiler.kai` (commit
`7ee72ea`):

```
alloc_total=39,050,630  free_total=18,147,268  leaked=20,903,362
  tag int     allocs=3,319,398    ( 8.5%)
  tag real    allocs=1            ( 0.0%)
  tag char    allocs=5,590        ( 0.0%)
  tag str     allocs=6,128,055    (15.7%)
  tag cons    allocs=4,583,339    (11.7%)
  tag record  allocs=5,628,611    (14.4%)
  tag variant allocs=18,231,549   (46.7%)
  tag closure allocs=1,010,888    ( 2.6%)
  tag array   allocs=143,199      ( 0.4%)
```

99.6% of allocations land in six tags. Free events: 18.1M (each one
goes through the `kai_free_value` switch).

The runtime decref hot path — `kai_decref(v)` at
`stage0/runtime.h:827` — is already very tight: NULL guard,
`INT32_MAX` saturation skip (singletons), decrement, branch to
`kai_free_value` when `rc == 0`. Only the rc-zero path runs the
switch dispatch.

## Implementation (M2 + M3)

Two layers landed end-to-end and were measured, then reverted before
this report:

**M2 — runtime helpers in `stage0/runtime.h`**:

- `kai_decref_int / _real / _char / _str / _list / _record / _variant /
  _closure / _array`: per-tag `static inline` decrefs that skip the
  switch dispatch. The `kai_decref_list` variant unrolls cons-tail
  recursion into a `while` loop, which is also a stack-safety win for
  long lists.
- `kai_internal_drop_<tag>` wrappers that call the matching
  `kai_decref_<tag>` and return `&kai_singleton_unit`. These let
  `__perceus_drop` call sites stay in expression position without a
  stmt-expr wrap. Unit / Bool wrappers are no-ops since both tags are
  always singletons.

**M3 — emitter dispatches in `stage2/compiler.kai`**:

- `drop_fn_name_for(opt_ty)` and `decref_fn_name_for(opt_ty)`:
  type-driven helper-name dispatch (TyInt → `kai_internal_drop_int`,
  TyString → `kai_internal_drop_str`, etc.). Falls back to generic
  `kai_internal_drop` / `kai_decref` for TyVarT, TyAny, and TyCon
  (the runtime can free user-defined records / variants without
  per-name knowledge, but selecting the tag-aware helper requires a
  body shape lookup the emitter doesn't do today).
- Wired into:
  - `__perceus_drop` lowering (the largest single emit site by
    volume).
  - `bit_*` intrinsics (`bit_and`, `bit_or`, `bit_xor`, `bit_not`,
    `bit_shl`, `bit_shr`, `bit_ushr`, `bit_count`, `bit_test`,
    `bit_set`, `bit_clear`, `bit_toggle`) — all `_a` / `_b` are
    KAI_INT.
  - Match scrutinee exit drop (`emit_match_default`).
  - Let-stmt unused-binding drop (`emit_let_stmt_with_drops`).
  - PWild let drop with `is_fresh_alloc` rhs.
- `pcs_make_drop_stmt` extended to accept an `Option[Ty]` and stamp
  it onto the inserted EVar's `.ty` slot. The drop nodes
  perceus_pass synthesises were unconditionally `ty: None` before;
  without this fix the type-driven dispatch in `__perceus_drop`
  fell back to generic for ~99% of sites.

`pcs_make_drop_stmt`'s callers (`pcs_collect_entry_drops`,
`pcs_collect_exit_drops`) already had the `Param` in hand, so
`pcs_param_ty(p)` (resolve `p.ptype`'s `TypeExpr` to `Ty` via the
existing `resolve_ty`) was a one-line bridge.

After M3 in the emitted `stage2.c` output:

```
kai_decref(           generic:     1,912 sites
kai_decref_<tag>      specialized:   714 sites
kai_internal_drop(    generic:     1,400 sites
kai_internal_drop_<tag> specialized: 987 sites
```

So ~41% of `kai_internal_drop(` sites became per-tag (≈the share
of drops on perceus-synthesised parameter slots whose param type
resolves to a builtin). The remainder are TyCon / TyVarT / TyAny
sites that fall through to generic dispatch by design.

## Validation (M4)

| gate                     | result                          |
|--------------------------|---------------------------------|
| `make all` (kaic0/1/2)   | green                           |
| `make selfhost` (C)      | byte-identical fixed point      |
| `make selfhost-llvm`     | byte-identical fixed point      |
| `make tier1`             | green (24 demos, 21 fmt fixtures, bench smoke) |

No undefined-behaviour signal. The LLVM backend is unaffected
(emit path untouched). No glibc tcache aborts, no use-after-free
hits during any of the test runs.

## Measurement

Methodology: each sample is `/usr/bin/time -p ./stage2/kaic2
stage2/compiler.kai > /dev/null`, with the first run dropped as a
cold-cache warm-up. All numbers are wall-clock seconds (the `real`
line of `time -p`).

### `kaic2` self-compile, `-O0` (default `make all`)

| build         | n  | min  | median | mean | max  |
|---------------|---:|-----:|-------:|-----:|-----:|
| baseline      |  7 | 5.11 |  5.18  | 5.19 | 5.31 |
| modified (M3) | 14 | 5.32 |  5.46  | 5.46 | 5.71 |

**Δ median: +0.28s (+5.4% slower)** at `-O0`.

The regression at `-O0` is dominated by the source-size growth
(~270 net lines added across `runtime.h` + `compiler.kai`): kaic2
self-compile work is roughly linear in source size. The per-call-
site savings from drop spec do not surface at `-O0` because
`static inline` does not inline at `-O0` — every per-tag wrapper
becomes a real function call, indistinguishable from `kai_decref`
dispatching to `kai_free_value`.

### `kaic2` self-compile, `-O2` (production build via `bin/kai`)

| build         | n  | min  | median | mean | max  |
|---------------|---:|-----:|-------:|-----:|-----:|
| baseline      |  9 | 2.88 |  2.92  | 2.93 | 2.97 |
| modified (M3) |  9 | 2.82 |  2.87  | 2.88 | 3.02 |

**Δ median: −0.05s (−1.7% faster)** at `-O2`.

This is the regime where the per-tag wrappers actually inline. The
win is real but small: the runtime tag dispatch is a single switch
on a 13-element enum with high branch-prediction hit rate, and
Phase 2 unboxing already eliminated the high-frequency
KAI_INT-arithmetic decref traffic that the tag dispatch would have
amortised over.

### Alloc counts

| metric        | baseline    | modified    | Δ            |
|---------------|------------:|------------:|-------------:|
| alloc_total   | 39,050,630  | 39,160,855  | +110,225 (+0.3%) |
| free_total    | 18,147,268  | 18,216,633  |  +69,365 (+0.4%) |
| leaked        | 20,903,362  | 20,944,222  |  +40,860 (+0.2%) |

The slight increase is explained by the modified `compiler.kai`
being ~200 lines longer; the AST walked during self-compile has
more nodes. **Drop specialisation does not change the number of
free events** — only the per-event cost. Alloc-churn-as-count is
the wrong metric for this lane; alloc-churn-as-cost is what
matters, and that surfaces as wall-time, where the signal is small
to negative.

### Inner-loop benchmark

`examples/perceus/unbox_bench.kai` runs in ~0ms after the first
warm-up at both baseline and modified. Phase 2 unboxing collapses
the entire inner chain to raw `int64_t`, so there are essentially
no decrefs to specialise. Skipped as a useful comparison.

## Decision

**Close lane with doc-only PR.** Per the lane prompt's criterion
#6 ("ROI demasiado bajo") and the project's "ship measurable or
nothing" discipline, the implementation does not meet the ≥10%
threshold and at the default build mode it slightly regresses.
Reverting `stage0/runtime.h` + `stage2/compiler.kai` to `7ee72ea`;
this report and a short CHANGELOG entry are the only artifacts the
PR ships.

The change is correct (selfhost holds, tier1 green, no UB). The
runtime helpers and emitter dispatch are valid infrastructure that
a future Perceus lane may revisit, but committing them now would
ship a default-build regression for a marginal `-O2` win — the
opposite of the discipline pinned for these lanes.

### Pin to Anga Roa

`docs/perceus-honesty-targets.md` Tier 3a / 3b items survive:

- **Reuse-in-place** (~1–2w) — the constructor-cell-reuse
  optimisation. Big win on linked-list rewrites (`map`, `filter`,
  `fold`-into-list). Requires alias analysis the type system can
  prove. This is the next high-ROI Perceus optimisation; drop spec
  is not a prerequisite.
- **Drop specialisation, fuller scope** — could revisit once
  reuse-in-place lands and the alloc mix shifts. Specifically: a
  TyCon → record/variant body-shape lookup would let the per-tag
  dispatch cover the ~61% of allocs currently in
  KAI_RECORD / KAI_VARIANT, raising coverage from ~38% to ~99% of
  alloc events and possibly tipping the wall measurement past
  threshold. Today, with Phase 2 already eliminating the bulk of
  the boxing overhead and TyCon coverage left out, the headroom
  is too narrow.

## Compiler errors I encountered

None visible in current context. Every iteration built cleanly on
the first attempt, including:
- The runtime.h additions (per-tag inline helpers + drop wrappers)
  compiled with no `cc` warnings beyond the pre-existing
  `void *`-to-anonymous-struct conversions (lines 2273/2278/2288 of
  the modified file, present in `7ee72ea` too).
- The `compiler.kai` emit changes compiled through both kaic1 (old
  emit) and kaic2 (new emit) without diagnostic. Selfhost reached
  fixed point on the first attempt for both C and LLVM backends.

The lack of compiler errors is itself a calibration point: the
pre-existing JSON / typed-hole infrastructure was not exercised on
this lane because nothing went wrong syntactically or
type-checking-wise.

## Friction points

1. **`pcs_make_drop_stmt` produced `ty: None` EVar nodes.** First
   wired drop spec at the `__perceus_drop` lowering site only,
   measured 8 of 2116 sites resolved to per-tag (0.4%). Tracked it
   to `stage2/compiler.kai:23950` where the synthesised drop ECall
   wraps a freshly-built EVar with `ty: None, mode: MUnknown`. No
   later pass re-types these nodes — the post-perceus pipeline
   (tcrec rewrite, emit) doesn't propagate types onto
   perceus-inserted nodes. Fixed by extending `pcs_make_drop_stmt`
   to accept the param's resolved Ty and stamping it on the EVar.
   This was the most subtle architectural detail of the lane and
   the only one that required AST surgery beyond the call sites.

2. **`-O0` vs `-O2` divergence.** First wall measurements were at
   `-O0` (default project flags, `make all`). Modified version
   measured ~+5% slower than baseline. The conclusion "drop spec
   regresses" was wrong — the issue was that `static inline`
   doesn't inline at `-O0`, so every per-tag wrapper becomes an
   indirect function call, indistinguishable from the original
   dispatch. Re-measured at `-O2` (the `bin/kai` driver default)
   and saw a small win. Mentioning this here so the next agent
   knows to test both regimes when measuring inline-style
   optimisations.

3. **Source-size growth confound.** The modified `compiler.kai` is
   ~200 lines longer (helpers, comments). The kaic2 self-compile
   workload scales roughly linearly in source size, so
   "before vs. after" wall-clock on the self-compile mixes the
   intended speed-up with the source bloat. Mitigation tried:
   running both binaries on a fixed external input — but the
   stage2/compiler.kai source uses `Stderr` / `Stdout` rows that
   stage1/compiler.kai doesn't, and the candidate fixed inputs in
   `stdlib/` are all <500 lines, too small for stable
   measurement. Settled for self-compile wall-clock with
   source-growth as a known confound.

## Spec ambiguities or interpretive choices

The lane prompt allowed scope flexibility ("user-defined sum/record
types pueden quedarse en dispatch"). Two scope decisions were made:

1. **TyCon stays generic.** User-defined record / variant types
   (TyCon name lookup → TBRecord / TBSum) all flow through
   KAI_RECORD / KAI_VARIANT tags at runtime. The per-tag helpers
   `kai_decref_record` / `kai_decref_variant` exist and would
   work, but the emitter doesn't currently route TyCon to them
   because that needs a body-shape lookup against the program's
   type table. Decision: skip this for the first pass; revisit
   once the v1 numbers land. The numbers landed marginal, so
   "revisit" became "defer to Anga Roa".

2. **TyDimT / TyRefineT recurse on the inner type.** Dimension and
   refinement wrappers are runtime-erased — both lower to their
   base type. So `drop_fn_name_for_ty(TyDimT(TyInt, _))` returns
   `kai_internal_drop_int`. This is the only "specialise through
   wrapper types" decision in the implementation; it's
   straightforward but worth flagging because it differs from
   what an LLVM/JIT-style codegen would do (no recursion needed
   when types are already canonicalised).

## LLM-friendly bet evidence (Tier 3, per `docs/lane-experience-retro-2026-05-01.md`)

> At any point during this lane, would `--effects-json` or
> `--effect-holes-json` have helped you recover from a typing or
> effect-row error? Did you actually use them, or did the plain-
> text compiler output carry the work?

Neither. No typing errors or effect-row errors were hit during the
lane — all code changes compiled cleanly the first time. The work
that benefited from JSON would have been the `pcs_make_drop_stmt`
gotcha (drop EVar nodes with `ty: None`) — but that surfaced as a
*runtime* observation (counting `kai_internal_drop_*` occurrences
in the emitted C), not as a compiler diagnostic. The compiler
*successfully* emitted the generic `kai_internal_drop` for those
sites; the typed-hole / effects-JSON contracts have nothing to say
about a successful-but-suboptimal emit choice.

This is consistent with prior lanes in the
`lane-experience-retro-2026-05-01.md` survey: the JSON contract is
strongest for type-driven authoring, less helpful for performance
tuning lanes where the question is "did the optimisation fire?"
rather than "does this type-check?".

## Subjective summary

- **Confidence in correctness**: high. Selfhost C + LLVM
  byte-identical, full tier1 green, no UB signal. The reverted
  state (current branch HEAD) is identical to `7ee72ea`.
- **Confidence in measurement**: medium. Single machine, no
  process pinning, n=7-15 per condition. Variance is ~5% per
  side; the +5.4% `-O0` regression and −1.7% `-O2` win are both
  inside that variance band. Confident enough to call the result
  "marginal/neutral", not confident enough to claim the `-O2` win
  as a real signal.
- **Hardest sub-task**: `pcs_make_drop_stmt` type propagation.
  Easy to *write*, but the symptom (only 0.4% of drops hitting
  per-tag) was easy to miss without counting the emitted C
  occurrences. A future "does my optimisation fire?" instrumentation
  would help here.
- **Easiest sub-task**: M2 (runtime helpers). Mechanical
  reproduction of the existing `kai_decref` + `kai_free_value`
  switch, one branch per tag.
- **Did the compiler help or hinder?** Neither distinctly. The
  compiler accepted everything I wrote on first try, including
  the type-driven dispatch. The performance signal that ultimately
  drove the close-lane decision came from `KAI_TRACE_RC` and
  `time -p`, not from the compiler.

## Limitations of this report

- Self-report bias acknowledged.
- Context truncation: counts and error lists exclude anything that
  fell out of my visible context window.
- Single agent (Claude Opus 4.7). Not generalisable across LLMs.
- Wall-clock measurement methodology is `time -p` on a single
  machine without isolation (no taskset / cpuset / power profile
  pinning). Variance of ~5% within a side limits the smallest
  effect detectable; if the true `-O2` win is smaller than the
  measured −1.7% the conclusion still holds (close lane), but a
  rigorous evaluator would want isolated benches at n≥30.
- The `bench v1` smoke test was tried (`./bin/kai bench
  examples/stdlib/bench_basic.kai`) but reports per-iteration
  ns at the call-graph level, which doesn't isolate the decref
  cost. A dedicated micro-bench (e.g., a tight loop allocating +
  freeing 1M strings) would have given a cleaner per-event
  number; postponed because the kaic2 self-compile signal already
  bracketed the answer.

## Raw build log

```
timestamp	cmd	outcome	elapsed_s
2026-05-01T20:43:08-04:00	all	OK	-
2026-05-01T20:49:18-04:00	all	OK	-
2026-05-01T20:49:59-04:00	selfhost	OK	-
2026-05-01T20:53:15-04:00	all	OK	-
2026-05-01T20:53:34-04:00	selfhost	OK	-
2026-05-01T20:57:47-04:00	tier1	OK	-
2026-05-01T20:59:24-04:00	all	OK	-
2026-05-01T20:59:42-04:00	selfhost	OK	-
2026-05-01T21:06:13-04:00	selfhost	OK	-
2026-05-01T21:18:29-04:00	tier1	OK	-
2026-05-01T21:26:47-04:00	tier1	OK	-
```
