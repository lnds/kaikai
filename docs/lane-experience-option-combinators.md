# Lane experience — Option combinators (audit Proposal 1, partial)

**Branch:** `lane-1-option-combinators`. **Base:** `main` @ `ffc7b10`. **Date:** 2026-05-10.

Lane spawned to close Proposal 1 of `docs/lane-experience-compiler-idioms-audit.md`:
ship `Option.map` / `and_then` / `or_else` / `first_some` / `get_or` / `is_some`
combinators in `stdlib/core/option.kai`, then rewrite the ~280 manual
`Some/None` arms in `stage2/compiler.kai` to use them.

## TL;DR

The lane reduced from "ship 6 combinators + rewrite 280 sites" to
"ship `first_some` + 1 fixture, file rewrite lanes as follow-ups", because
**five of the six combinators already existed in `stdlib/core/option.kai`**
(`map`, `and_then`, `or_else`, `unwrap_or` ≡ `get_or`, `is_some`). The
audit's premise — that the compiler reinvents these inline *because* the
stdlib doesn't ship them — was factually wrong: it ships them, the
compiler simply doesn't use them. That changes the lane's shape from
"stdlib API addition + downstream rewrite" to "downstream rewrite only".

Splitting the downstream rewrite into per-module PRs (lexer / parser /
typer / emitter / fmt) is now the right path forward, per the memory
"Lanes revert main commits if not rebased" and the audit's own §3
remark on slicing the pipe-rewrite lane.

## Scope as planned vs. shipped

**Planned (per brief):**
- Add 6 combinators (`map`, `and_then`, `or_else`, `first_some`,
  `get_or`, `is_some`).
- Add 6 fixtures under `examples/stdlib/option_*`.
- Rewrite ≥250 of 282 audit-identified sites in `stage2/compiler.kai`.
- Verify selfhost byte-identical + tier1 green.
- Net LOC: -700 minimum.

**Shipped:**
- 1 new combinator: `first_some`. The other five already exist
  (line numbers in current `stdlib/core/option.kai`):
  - `is_some` @ 12
  - `is_none` @ 19 (bonus; audit didn't request)
  - `map` @ 26
  - `unwrap_or` @ 33 (≡ requested `get_or`; kept existing name)
  - `and_then` @ 40
  - `or_else` @ 55 (thunk variant, matches the requested lazy semantics)
- 4 new intrinsic tests for `first_some` (Some-leading, mixed, all-None,
  empty).
- 1 new fixture: `examples/stdlib/option_first_some_basic.kai` +
  `.out.expected`. Other combinators rely on the pre-existing intrinsic
  tests in `stdlib/core/option.kai` (the established pattern; `map`,
  `unwrap_or`, `and_then`, `is_some` all have intrinsic tests but no
  external fixture, and that pattern was kept rather than inflated).
- **No rewrites in `stage2/compiler.kai`.** Deferred to follow-up lanes.
- `make tier0` green. `make stage2/test-stdlib-core-intrinsic` green
  (all 6 modules pass; intrinsic tests for `first_some` included).
  `make stage2/test-stdlib` green (`option_first_some_basic` passes
  alongside the eight pre-existing `option_*` fixtures).

LOC delta: +41 in `stdlib/core/option.kai` (combinator + tests). The
-700 target was for the rewrite phase, which isn't in this PR.

## Design decisions and alternatives considered

- **`first_some` takes a `[Option[a]]` list, not varargs.** Reason: the
  language has no varargs; the list is the idiomatic shape and matches
  the audit's suggested call (`first_some([find_a(x), find_b(x), …])`).
  Tradeoff: the list is fully constructed before the call, so every
  candidate is evaluated eagerly. For lazy short-circuit between two
  branches use `or_else(() => …)`; for N pure branches `first_some` is
  the right tool. This is documented in the docstring above the fn.

- **No `get_or` alias.** The brief listed `get_or` as one of the six
  combinators. `stdlib/core/option.kai:33` already ships `unwrap_or`
  with the same shape (`Option[a] -> a -> a`). Adding a `get_or` alias
  would duplicate the function and create the very "two ways to do the
  same thing" failure mode CLAUDE.md tier 2 #4 warns against. Renaming
  `unwrap_or → get_or` is out of scope (cross-codebase rename) and not
  obviously an improvement — `unwrap_or` is the Rust-borrowed name and
  matches `unwrap_or_else` already shipped.

- **`first_some` lives next to `collect`.** Both operate on
  `[Option[a]]` and short-circuit. Grouping them is the natural reading
  order — `collect` requires *all* Some, `first_some` requires *any*.

- **Intrinsic tests over external fixture for the cheap cases.** The
  established pattern in `stdlib/core/option.kai` is intrinsic tests
  via `test "..." { ... }` blocks at the bottom of the file, runnable
  via `KAI_NO_STDLIB=1 bin/kai test stdlib/core/option.kai` and
  CI-gated through `test-stdlib-core-intrinsic`. The audit brief asked
  for six external fixtures; doing so would have duplicated coverage
  that already exists for `map`, `and_then`, `unwrap_or`, `is_some`.
  The one new fixture is `first_some` (new combinator, multi-arm
  shape worth pinning end-to-end). The retired/never-shipped fixtures
  for the existing combinators are *also* a small follow-up if a
  reviewer wants symmetry; not blocking.

## Structural surprises the brief did not anticipate

- **Combinator existence.** Brief asserted "Combinators don't exist in
  stdlib, so the compiler reinvents them inline." Five of the six do
  exist — verified by reading lines 1-90 of the existing
  `stdlib/core/option.kai`. The first 10 lines explicitly document the
  m14 phase 3 migration (issue #203) that landed the module-qualified
  call style (`option.map(o, f)`). The audit's data on 147 `None -> None`
  + 135 `Some(...) -> Some(...)` arms in `stage2/compiler.kai` remains
  accurate — but the *cause* is "compiler hasn't been rewritten",
  not "combinators don't ship".

- **The "byte-identical selfhost" gate framing.** The brief asked to
  verify selfhost byte-identical "before and after". That's tautological
  for a stdlib-only change that doesn't touch `stage2/compiler.kai` —
  same source, same compiler, same output. It only becomes meaningful
  once the rewrite lanes start swapping inline match arms for combinator
  calls (where Phase 3 unboxing must inline `option.map(o, f)` back into
  the same C as the inline match — otherwise selfhost diverges in shape
  and perf). The gate framing was load-bearing for the (deferred)
  rewrite phase, not this PR.

- **Driver scoping.** Brief said READ-ONLY outside
  `stdlib/core/option.kai`, `stage2/compiler.kai`, `examples/stdlib/option_*`,
  `docs/lane-experience-option-combinators.md`. Honoured strictly —
  this PR touches only those files (plus this retro).

## Fixtures added and coverage gaps

**Added:**
- `examples/stdlib/option_first_some_basic.kai` + `.out.expected` —
  positive smoke covering empty / all-None / mixed / leading-Some.

**Pre-existing fixtures (kept untouched):**
- `option_collect_basic`, `option_filter_basic`, `option_ok_or_basic`,
  `option_ok_or_else_basic`, `option_or_basic`, `option_or_else_basic`,
  `option_unwrap_or_else_basic`, `option_zip_basic`.

**Coverage gaps left open:**
- No external fixtures for `map`, `unwrap_or`, `and_then`, `is_some`,
  `is_none`. Intrinsic tests inside `stdlib/core/option.kai` cover them.
  Adding external fixtures is a small follow-up if a reviewer wants
  symmetry with the other combinators that do have one.
- No tier1 test exercising `first_some` from inside `stage2/compiler.kai`.
  That naturally arrives when the first rewrite lane lands.

## Real cost vs. estimate

Brief estimated 1.5 days for the original scope (combinators +
rewrite). Actual cost so far: ~1 hour, almost all on:
1. reading existing `stdlib/core/option.kai` and discovering the brief's
   premise was wrong (~15 min);
2. surfacing the scope question to the user, agreeing on the reduced
   plan (~15 min);
3. adding `first_some` + tests + fixture + verification (~30 min).

The deferred rewrite portion (the original 1.5-day estimate) remains
ahead of us, sliced into 4-5 module-scoped lanes.

## Follow-ups left for next lanes

Each is a candidate per-module rewrite lane against the same audit
data. Estimated 0.5-1 day each, low-medium risk, with selfhost
byte-identical (post-Phase-3-unboxing inlining) as the gate per module:

1. **Lane 1a — lexer/parser rewrite.** Sites: search `stage2/compiler.kai`
   lines 1-9000 for `match … { Some(_) -> Some(_); None -> None }` shapes
   in `lex_*` / `parse_*` functions. Estimated ~50-80 sites.
2. **Lane 1b — typer rewrite.** Sites: search lines ~9000-22000
   (`rqc_*`, `tcrec_*`, `tc_expr`, `subst_*` families). The audit-cited
   `find_impure_call` block at lines 6285-6320 belongs here (the
   `first_some` 3-deep nesting). Estimated ~80-120 sites.
3. **Lane 1c — emitter rewrite.** Sites: lines ~22000-44000 (`emit_*`
   families). Estimated ~50-80 sites.
4. **Lane 1d — fmt + driver rewrite.** Sites: lines ~44000-51644
   (`fmt_*` family + module loader). Estimated ~20-40 sites.
5. **Lane 1e — stdlib own usage.** The audit notes the stdlib has its
   own `None -> None` arm count (3 in 14.2K LOC) — trivial follow-up,
   maybe folded into 1a.

Each lane should:
- Verify selfhost byte-identical (`make selfhost` sha256 unchanged).
- Add no fixtures (the rewrite is semantic-preserving; existing tests
  catch breaks).
- Cite this retro doc + the audit doc in the closing PR.
- Document in its own short retro: how many sites rewritten, any
  combinator that *failed* to apply cleanly (effects, custom shapes),
  and the LOC delta.

A standalone follow-up: re-run the audit count after all four lanes
close, to confirm the residual `None -> None` count is < 10 (sites the
audit flagged as not pure-rewritable — see "CAUTION" notes in the
audit §1a).

## Honest caveats

- **The audit's LOC-saved estimate (~840 LOC) is for the rewrite, not
  for this PR.** This PR alone saves zero compiler LOC; it just unlocks
  the rewrite path. The -700 LOC gate in the brief therefore applies to
  the aggregate of the follow-up lanes, not to this PR.
- **The fact that combinators existed but went unused is the
  real audit finding.** The mechanical fix is the rewrite; the cultural
  fix is the lane-close-discipline that catches "we added combinators
  in m14 phase 3 but never adopted them in the compiler". Worth a note
  in CLAUDE.md *Doc discipline* eventually — for now, this retro is the
  artifact of record.
