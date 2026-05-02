# Lane experience report — check-v1

Best-effort retrospective by the implementing agent.

## Objective metrics (from /tmp/lane-check-v1-builds.tsv)

- Start: 2026-05-01T20:40:49-04:00
- End:   2026-05-01T21:10:03-04:00
- Wall-clock: ~29m 14s
- Build/test invocations:
  - `make selfhost`: 2 invocations, 2 passes, 0 fails. The first
    confirmed the M1+M2 walker plumbing kept the bootstrap
    byte-identical; the second confirmed the same after the
    M3+M4 emit + runtime additions.
  - `make test-check`: 1 invocation, 1 pass, 0 fails.
  - `make tier1`: 1 invocation, 1 pass, 0 fails (the rename of
    eleven existing `fn check` helpers to `fn verify` was done
    *after* the first tier1 run revealed the keyword collision,
    but that first run's failure was diagnosed inside
    `test-stdlib`, not via a top-level `make tier1` invocation —
    so the TSV records it as the single passing run after the
    rename).
  - Manual smoke compile of a 3-block fixture: 2 attempts, 1
    pass. The first attempt referenced a nonexistent
    `list_eq_int` helper (assumed structural list equality
    needed an explicit `Int` overload); the second used `==`
    directly and compiled clean. Decision: structural equality
    works for `[Int]` end-to-end via the existing kaikai `==`
    operator, so v1 fixtures don't need a custom comparator.

## Compiler errors I encountered

Inside the kaikai layer:

1. **`undefined name list_eq_int`** during the manual smoke
   test of the property block

       check "list reverse twice" with xs: [Int] {
         list_eq_int(list_reverse(list_reverse(xs)), xs)
       }

   I had assumed list equality required an `Int`-specialised
   helper. `==` works directly, so the fix was a one-character
   substitution. Diagnosis time: ~5 s (the error pinpointed the
   call site exactly).

Outside the kaikai layer:

2. **`expected function name`** at every existing
   `fn check(label, ok)` declaration once the new `check`
   keyword went live. Eleven fixtures hit this. Fix was
   mechanical (sed `check(` → `verify(` per file) but the *cost
   of taking a keyword namespace* is real and worth flagging:
   bench v1 paid zero rename cost because no fixture used
   `bench` as an identifier. Future keyword-promoting lanes
   should grep for the candidate identifier before committing
   to the keyword.

That's it. No type errors, no effect-row errors, no
non-exhaustive match warnings, no Perceus drop-pass complaints.

## Friction points

- **`MCheck` was already taken.** The user-facing brief said
  `--check` flag, but the existing typecheck-only mode
  (`MCheck`, triggered by `--check`) predates property-checks
  and is referenced from `stage2/Makefile`. Repurposing
  `--check` would break the existing workflow. Compromise:
  internal flag is `--prop-check`, internal mode is
  `MPropCheck`, and the user-facing `kai check` subcommand
  translates. The CHANGELOG and the `usage_check` help text
  document the rationale. Cost: ~3 minutes of decision +
  wording.

- **Walker call-site count is the dominant cost.** Same
  observation as bench-v1: the DCheck mirror needed an arm in
  ~25 distinct functions across `stage2/compiler.kai`. With
  `with`-clause params I had to be more careful than bench was
  — for some passes (validation, perceus, rqc, rename-proto)
  the params shape the analysis (treated as fn-param-like
  bindings); for others they ride along unchanged. A grep map
  of every DBench arm (38 lines from `grep -n DBench
  stage2/compiler.kai`) was the load-bearing reference; without
  it, missing the param-aware sites would have been silent.

- **Counterexample formatting.** `kai_to_string` borrows its
  argument and decrefs the resulting String once we're done,
  but I initially wrote
  `KaiValue *s = kai_to_string(kai_incref(v))` which double-
  inc'd. Caught immediately by reading the runtime header; no
  build error reached the surface. Cost: ~30 s.

- **Generator range vs. property vacuity.** First fixture
  draft used `n > 0 implies factorial(n) > 0` with `n: Int`.
  Looks innocent but `kai_arbitrary_int` returns values in
  [-50, 50], and `factorial(40)` already overflows int64 — the
  property fails by counterexample, which is *correct
  behaviour* but hides the smoke-test signal that the runner
  works. Replaced with properties that hold for *every* value
  in the generator range (commutativity, associativity, list
  reverse round-trip). The fixture comment now calls this out
  so a future agent doesn't reintroduce the trap.

## Spec ambiguities or interpretive choices

1. **`KAI_CHECK_ITERS` value.** The user brief said "N=100";
   bench v1 used 1000. 100 is enough to flush most boundary-
   value bugs at the v1 generator scale and keeps the smoke
   fixture wall-clock under 100 ms. v1.x can read this from an
   env var.

2. **PRNG reseed policy.** Fixed seed on every run so a
   counterexample reproduces identically. The bench-v1 retro
   noted timing variance as a feature; here, *reproducibility*
   is the feature. v1.x adds `KAI_CHECK_SEED` env override.

3. **`setjmp` wrap?** Like bench v1, the check harness does
   *not* setjmp around the body. An assertion failure inside a
   property body should panic the process — there's nothing
   useful to report if the predicate's evaluator aborted. The
   counterexample-on-false path uses a normal `if (!_ok) break;`
   inside the iter loop, not a longjmp.

4. **Counterexample content vs shrinking.** v1 reports the
   *first* counterexample observed (no shrinking). Shrinking is
   v1.x; the code path is `kai_check_record_param` → buffer →
   `kai_check_fail`. Adding shrinking later means walking the
   buffer + invoking the body fn with a candidate-shrunk param
   set, not changing the report path.

5. **`MPropCheck` mode flag orthogonality.** Same pattern as
   bench: a third sibling Bool (`check_mode`) on
   `emit_program` / `emit_program_llvm` / `emit_main_wrapper`,
   not folded into the existing `test_mode` / `bench_mode`
   pair. The three modes are CLI-mutex so the call sites read
   symmetrically.

6. **Generator coverage cap.** v1 supports Int / Bool / Char /
   String + lists of those. Sum / record / non-primitive list
   element / fn / variant types are all v1.x or v2 — the
   compile-time path emits a `kai_prelude_panic` stub with the
   offending type name so the gap surfaces at runtime, not
   silently. (Surfacing it at compile time would be cleaner but
   requires a name-resolution step inside the typer's intrinsic
   table; deferred together with v1.x structural derivation.)

## Tier 3 LLM-friendly bet evidence

**Did `--effects-json` or `--effect-holes-json` help me recover
from a type / effects error during this lane?**

No. The lane produced *zero* type or effect errors at the
kaikai level. The two errors I hit were:

1. `list_eq_int` was undefined — a name-resolution error.
   Plain text said "undefined name `list_eq_int`" at the exact
   call site; a JSON shape would have given me the same
   pointer with more bytes.

2. `expected function name` after the `check` keyword
   promotion broke `fn check(...)` declarations. Plain text
   pinpointed line and column; the *understanding* needed was
   "check is now a keyword", which a structured JSON of the
   typer state would not have surfaced — that's a parser-stage
   diagnostic, not a typer one.

Like bench-v1, this is a lane where **plain-text compiler
output carried the entire diagnostic load**. The work was
mechanical mirror plumbing, not novel design. The cases where
structured JSON would have helped — "every match arm in
`stage2/compiler.kai` that destructures `DCheck`", or "every
fixture under `examples/` that uses `check` as an identifier"
— are *codebase queries*, not typer queries. The current
honesty-targets work scopes typed holes; an adjacent line of
work scoping a `kai grep --constructor DCheck` or `kai refs
<name>` would have shaved ~5 minutes off this lane (the
manual `grep -n DBench` + per-file scan to confirm parity).

If anything, the lane is *evidence against* the strongest form
of the LLM-authorability hypothesis: an agent operating on a
mirror lane is bottlenecked by site enumeration, not by
understanding. Strengthening the structured-introspection
toolchain on that axis (constructor refs, identifier refs,
declaration-by-shape) would compound across every future
mirror lane.

## Subjective summary

- **Confidence in correctness: high.** The check pipeline is a
  structural mirror of the bench pipeline (which is itself a
  mirror of the test pipeline) with two intentional deviations:
  (a) `with`-clause params thread through param-aware passes
  the same way DFn's params do, and (b) the emitter records
  each generator output via `kai_check_record_param` before
  evaluating the body, so the counterexample buffer holds
  every input the predicate saw on the failing iter.
  `make tier1` is green end-to-end; the fixture exercises
  parameterless / single-Int / `[Int]` / multi-param shapes.
  Selfhost is byte-identical post-change.

- **Hardest sub-task: M3+M4 emit + runtime.** Two distinct
  axes (the loop wrapping logic and the
  `arbitrary_for_ty(TypeExpr) → C-call` mapping) plus the
  counterexample buffer wiring. Took ~10 minutes including
  the `kai_to_string` borrow-vs-consume reread. The runtime
  helpers themselves are ~150 lines of straightforward C; the
  emitter logic is ~120 lines of kaikai.

- **Easiest sub-task: M5 CLI + M6 fixture.** Parallel-edit of
  `bin/kai` against the existing `cmd_bench`, plus a
  ~50-line fixture and a ~20-line Make recipe. ~5 minutes.

- **Did the compiler help or hinder?** Helped. Selfhost
  byte-identity is a strong existence proof for AST/walker
  correctness — every pass touched by the new `DCheck` arm
  must agree byte-for-byte with itself for the bootstrap to
  hold. The single non-exhaustive-match warning that would
  have flagged a missed walker arm never fired (every site
  was covered). Hindered: the `MCheck` name collision cost
  ~3 minutes of decision-making, not effort, but it's the
  kind of friction the namespace will keep paying as the
  compiler grows.

- **Diff stats**:
  - 4 commits over the lane (M1+M2, M3+M4, M5+M6, M7).
  - `stage2/compiler.kai`: ~+340 lines.
  - `stage0/runtime.h`: ~+150 lines.
  - 11 fixtures renamed `check` → `verify`.
  - 1 new fixture (`check_basic.kai`).

## Limitations of this report

- Self-report bias acknowledged. Same caveat as bench-v1.
- The TSV log is sparse (5 entries) because the lane front-
  loaded onto a single mid-lane selfhost run and saved the
  build invocations for end-of-milestone gates. Per-milestone
  wall-clock is reconstructed from commit timestamps, not
  measured directly.
- Property coverage is shallow: 4 fixtures, all primitive
  generators. A future lane adding sum/record generators
  should re-evaluate the runtime helper API design — the
  current `kai_arbitrary_<T>()` shape doesn't compose for
  `[Option[Int]]`-style nested types.

## Raw build log

```
timestamp	cmd	outcome	elapsed_s
2026-05-01T20:52:05-04:00	selfhost	OK	-
2026-05-01T20:58:53-04:00	selfhost	OK	-
2026-05-01T20:59:18-04:00	compile_check_smoke	OK	-
2026-05-01T21:02:06-04:00	test-check	OK	-
2026-05-01T21:09:08-04:00	tier1	OK	-
```
