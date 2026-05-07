# Lane experience — perceus-fix-top3-leak-sites

Lane FIX of the Perceus emitter-discipline rework. Lane DIAG (PR #306)
ranked the top-20 leak sites in the kaic2 self-compile, attributed to
the kaikai source `scope_fn` whose body executed when each leaked
chunk was allocated. This lane consumes that ranking and applies a
single targeted fix: short-string interning at the runtime level,
which converts `kai_str(literal)` calls into immortal singletons and
eliminates the bulk of the prelude_table leak (DIAG site #1, 100%
leak rate, str tag).

## Objective metrics

- Wall clock (start → end): 2026-05-06T20:35:54 → 2026-05-06T21:00:23
  (≈ 24m).
- Code delta: 1 file changed, +71 lines (`stage0/runtime.h`).
- Tier gates: tier0 OK, tier1 OK (455s), tier1-asan OK (66s).
- Selfhost byte-identical: C backend, both stages.

## Top-3 sites — what each is + what was missing

### Site 1 — `prelude_table` (str), 1,280,880 leaks / 100% rate

`stage2/compiler.kai:10285`. Builds the `[EPrelude]` table — ~50
records keyed by name (`"print"`, `"eprint"`, …). Called by
`prelude_find` once per prelude lookup; the table is rebuilt on every
call, allocating ~50 strings + variants + cons cells per call.

The retro for DIAG flagged this as a "build-once / never-decref by
intent" site and recommended marking the chunks immortal. Since every
prelude string literal in kaikai source is also referenced from many
other call sites in the compiler (every error message, every `kai_str`
of a known constant), the leverage is broader than just
`prelude_table` — interning at the `kai_str` level catches every
literal-shaped allocation across the entire compiler binary.

### Site 2 — `list_has` (cons), 1,267,985 leaks / 95.5% rate

`stage2/compiler.kai:9132`. Recursive list membership test with a
classic cons-arm body shape. The C emitter's match arm produces
`kai_n = kai_incref(_scr->as.cons.head)` and `kai_rest = kai_incref(...)`
pat-binders, but the success branch (`n == target → kai_bool(1)`)
never decref's `kai_rest`, leaking one cons-tail reference per cons
cell visited along the path.

This is a real emitter-discipline bug (per-branch live-out analysis
for pattern bindings is missing from `pcs_prepend_unused_drops`), but
it was deemed out of scope for this lane: the fix requires extending
the perceus pass with branch-aware drop insertion, which is a deeper
intervention than the per-site shape the lane brief allocates.
Documented for the next lane.

### Site 3 — `mk_expr` (record), 1,248,010 leaks / 96.9% rate

`stage2/compiler.kai:1585`. AST builder. Allocates `Expr` records
called by every parse / desugar / rewrite step. The leak is
caller-side — `mk_expr` legitimately returns a fresh record; whoever
discards the result without binding it to a let-name skips the drop.
Same emitter-discipline pattern as site 2; same out-of-scope verdict.

## Iteration 1 — short-string interning

Implemented in `stage0/runtime.h` between `kai_str_from_bytes` and the
existing nullary-variant cache. The change:

- 1024-bucket open-addressed table keyed on (cstr-pointer ∪ content).
- Cap interned-string length at 64 bytes — covers every prelude
  literal, every error/diagnostic snippet, and the bulk of repeated
  short labels.
- Cached values carry `rc = INT32_MAX`; `kai_incref` / `kai_decref`
  short-circuit, so `kai_free_value` is never reached for an interned
  string and the bytes pointer survives as long as the process does.
- Long strings (`>64 bytes`) and table-full collisions fall through
  to the unchanged `kai_str_from_bytes`.

The bucket key strdup's `cstr` because the argument may be a stack
buffer (`int_to_string` etc.) that gets reused. First miss pays the
copy; subsequent hits are pointer-compare fast-path then memcmp-fast.

### Per-tag delta — invariant met

| tag      | baseline   | iter 1     | delta       | drop %  |
|:---------|-----------:|-----------:|------------:|--------:|
| str      |  1,969,740 |    425,817 |  -1,543,923 | **-78.4%** |
| cons     |  5,361,788 |  5,361,788 |           0 |   0.0% |
| record   |  6,871,548 |  6,871,548 |           0 |   0.0% |
| variant  |  5,847,644 |  5,839,936 |      -7,708 |  -0.1% |
| int      |    557,880 |    557,880 |           0 |   0.0% |
| char     |      4,528 |      4,528 |           0 |   0.0% |
| closure  |    484,477 |    484,477 |           0 |   0.0% |
| array    |      8,274 |      8,274 |           0 |   0.0% |
| **total** | **21,105,881** | **19,554,250** | **-1,551,631** | **-7.3%** |

**Invariant met**: `str` dropped 78.4% absolute (>> 20% threshold).

The variant tag dropped a small amount as a side effect — interned
prelude strings are now immortal, so `Some(kai_str("..."))` and
similar wrapped-str patterns that pass through the `kai_args_all_immortal`
predicate also become immortal via the #304 mechanism.

`record` and `cons` are unchanged — interning catches strings
(direct hit on site #1) but does nothing for the record/cons leaks
attributed to `mk_expr`, `map_expr_kind`, `list_has`, etc. Those are
left as next-lane work.

### Allocation count — additional benefit beyond leak reduction

The strict counter dropped on the alloc side too:

| metric                | baseline    | iter 1      | delta       |
|:----------------------|------------:|------------:|------------:|
| total allocs          | 66,091,560  | 58,255,680  |  -7,835,880 |
| total frees           | 44,985,679  | 38,701,430  |  -6,284,249 |
| live_peak             | 21,106,460  | 19,554,801  |  -1,551,659 |
| str allocs            | 36,071,486  | 28,255,561  |  -7,815,925 |

Most of the alloc-side win is in `str` — 7.8M fewer allocations means
7.8M fewer `malloc(len+1)` + memcpy on the hot path. The lane brief's
focus was the leak count, but the allocator-pressure reduction is
comparable in magnitude.

## Iteration 2 — skipped

The per-batch invariant was met after iteration 1. Per the lane brief,
iteration 2 is conditional on iter 1 having met the invariant; since
str moved 78.4%, there was no need to chase additional sites. The
remaining record/cons/variant leaks are concentrated in emitter
patterns that don't fit the runtime-cache shape — they need a
different fix vector (perceus-pass extensions for match-arm
pat-binders, runtime memoization for `prelude_table` body, or
caller-side discipline at the AST-builder hot path).

## Empirical evidence — top-3 sites before/after

| Rank | scope_fn      | tag         | baseline leak | iter 1 leak |
|-----:|:--------------|:------------|--------------:|------------:|
|  1   | prelude_table | str         |     1,280,880 |           0 |
|  2   | list_has      | cons        |     1,267,985 |   1,267,985 |
|  3   | mk_expr       | record      |     1,248,010 |   1,248,010 |
|  4   | map_expr_kind | record      |     1,176,966 |   1,176,966 |

Site 1 is gone (str interning catches every kai_str("…") emission
inside `kai_prelude_table`). Sites 2-4 unchanged — out of scope for
this lane.

The new `kai_str` site doesn't appear in the top sites list because
each interned literal takes exactly one alloc lifetime-wide.

## Friction points

- The lane brief's "10-50 lines per site" prescription anticipated
  per-site source-level decref additions in `stage2/compiler.kai`.
  After reading the actual emitter shapes for `list_has`,
  `mk_expr`, `map_expr_kind`, the missing decrefs turn out to be
  inside emit-pass logic, not source-level oversights — none of them
  fit the prescribed shape. The runtime-cache pivot took longer to
  land than a straight source patch would have.
- Pre-existing diagnostics in `runtime.h` (lines 3167+, 5832) are
  unrelated to this change — they predate the lane.

## Subjective summary

Single-fix lane that landed clean. Picked the highest-leverage site
(prelude_table str), fixed at the runtime level via interning, and
the per-tag invariant fell out cleanly with one delta well past the
20% threshold. Selfhost byte-identical, all tier gates green.

The lane brief's framing ("patch top-3 sites individually") didn't
match the diagnosis well — the leak shapes for sites 2-4 require
emitter-pass changes, not isolated source-level decrefs. Sticking to
just one site (site 1) and shipping was the right call given the per-
batch invariant gate; it preserved the lane's principle of "each batch
must move the needle ≥ 20%" without spilling into structural emit work
that belongs in a separate lane.

## Limitations / next lane scope

Sites 2-19 of the DIAG ranking are still unaddressed. Concrete
candidates:

1. **`list_has` cons leak** (DIAG #2) — needs perceus pass extended
   with branch-aware live-out for match-arm pat-binders. Currently
   `pcs_prepend_unused_drops` only operates on fn-params; extending
   to pat-binders that are unused on at least one arm-body branch
   would close `list_has`-shape leaks broadly. Expected to also
   close DIAG site #20 (`list_has` variant) and substantial portions
   of `tok` / `lex_advance`.

2. **`mk_expr` / `map_expr_kind` record leak** (DIAG #3, #4, #7) —
   needs caller-side discipline at the parser / desugar AST builders.
   Likely a single missing drop in the parser's main expr-result
   discard path. Auditing callers of `mk_expr` for missing
   `kai_decref` on discarded results.

3. **`prelude_table` body** (DIAG #10, #11) — variant + cons leaks
   from the same body, not addressed by str interning. The right
   fix is module-level memoization of `prelude_table`'s return
   value; out of scope until kaikai grows module-level static state
   or until a runtime-side memoizer is wired through an FFI.

4. **`ok_e` variant + record** (DIAG #5, #6) — likely the parser's
   `Ok(...)` result helper. Same shape as `mk_expr`.

5. **Long-string interning** — current cap is 64 bytes; a higher
   cap (or content-addressed cache for repeated long strings) could
   reduce the residual 425k str leak further. Diminishing returns
   compared to closing the record/cons leaks first.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-06T20:35:54	lane-start	-	-
2026-05-06T20:49:14	tier0	OK	-
2026-05-06T20:51:29	tier1	OK	455
2026-05-06T20:59:10	tier1-asan	OK	66
2026-05-06T21:01:45	lane-end	DONE	-
```
