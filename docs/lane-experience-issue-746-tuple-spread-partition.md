# Lane experience — issue #746: tuple-in-list-spread under --include-prelude-tests

## Scope as planned vs. as shipped

**Planned:** fix the typer bug found while landing #744 — a function
returning `[(A, B)]` with an `[]` base case and a sugared tuple in the
recursive cons fails inference (`expected [Pair]`, `found
[Pair[Int, Char]]`), but only under `--include-prelude-tests`.

**Shipped:** root-caused to the Hanga Roa modular-typecheck partition
(not the tuple desugar, as the original #744 retro guessed). One-site
fix in `stage2/compiler/infer.kai`: `DTest`/`DBench`/`DCheck` now
attribute to their physical module (the running `anchor`), not
unconditionally to `target_mod`; and the partition walker advances the
anchor to `target_mod` once a genuine target decl is seen. A regression
fixture + `make test-issue-746` target wired into `test` / `test-fast`.

## Root cause (the interesting part — not where the #744 retro pointed)

The #744 retro hypothesised "tuple literal `(a, b)` does not propagate
type arguments to the `Pair` constructor." That was wrong — the symptom,
not the cause. The real chain:

1. `partition_decls_by_home` (the #574 modular-typecheck partition)
   buckets the flat prelude+target decl stream by home module, in
   first-appearance order, and the fold types each bucket against the
   merged delta of all earlier buckets. The target must come LAST so it
   inherits every prelude's records.
2. `decide_partition_home` sent every `DTest`/`DBench`/`DCheck` to
   `target_mod`, on the comment's assumption that "tests never live
   inside a prelude file."
3. That assumption is FALSE under `--include-prelude-tests` (the flag's
   whole point is to keep prelude test blocks). So the first prelude
   module with tests — `core/char.kai`, loaded early — emitted `DTest`s
   that *created the target bucket near the front of the bucket list*,
   before `core/tuple.kai` / `option` / `list` contributed their
   records.
4. The fold then typed the target bucket at that early position, with
   an inherited delta that did not yet contain `Pair` (or `Option`,
   `Triple`, …). `synth_record_lit` looked up `Pair`, missed, and fell
   to its `None` branch: `TyCon(None, "Pair", [])` — `Pair` with no
   type args. That is the `found [Pair]` in the error.

The bug is invisible without the flag because then prelude tests are
stripped, no early `DTest` creates the target bucket, and the target
lands last in first-appearance order as intended.

### How it was found vs. guessed

The #744 retro reached for the desugar because the minimal repro used a
sugared tuple. But narrowing showed an *explicit* `Pair { fst, snd }`
record failed identically, and a single-element list (no spread) failed
too — ruling out both the tuple desugar and list-spread inference.
Instrumenting `synth_record_lit`'s `None` branch (`nrecs=3 hasPair=N`)
then the fold (`module=h inherited_recs=3 hasPair=N` at position 3,
vs `inherited_recs=9 hasPair=Y` at the end without the flag) located the
ordering fault precisely. Lesson: a symptom at the tuple/record layer
can be an ordering bug three layers down; instrument, don't guess.

## Fix

`stage2/compiler/infer.kai`, two coordinated edits:
- `decide_partition_home`: `DTest/DBench/DCheck -> anchor` (was
  `-> target_mod`). A test block belongs to the physical .kai file it
  sits in.
- `partition_by_homes_walk`: `next_anchor = if home == target_mod then
  target_mod else anchor1`. Once a genuine target-authored decl appears
  (a `mo = None` DFn routed to `target_mod`), the anchor flips so the
  target file's *own* test blocks still resolve to `target_mod` rather
  than leaking back to the last prelude module.

### A wrong first attempt (recorded for the next lane)

The first fix moved the `target_mod` bucket to the end of the bucket
list unconditionally. It fixed the repro but REGRESSED compiling a core
module *as the target* (`core/tuple.kai --test`): the target never
inherits its own delta, so moving tuple-as-target to the end left its
own `Pair`/`Quad` self-references unresolved (it went 0 → 21 errors).
Verified the regression by building `main`'s kaic2 in a throwaway
worktree and diffing — `tuple.kai --test` was 0 errors on main, 21 with
the move. Reverted and fixed at the root (the `DTest` attribution)
instead. The root fix is strictly better: it also drops `core/list.kai
--test` from 26 preexisting errors to 0 (same ordering bug, different
victim).

## Fixtures added

- `examples/stdlib/issue_746_tuple_spread.kai` (+ `.out.expected`) — a
  `[(Int, Char)]` loop with `[]` base + sugared-tuple cons, a `test`
  block asserting the typed result, and a `main` for the standalone
  golden.
- `make test-issue-746` (wired into `test` + `test-fast`) — compiles the
  fixture in the *failing* mode (`--include-prelude-tests --test`) and
  asserts kaic2 emits clean C with no `type mismatch in list spread`.
  The runtime end-to-end check is the standalone `test-stdlib` run (the
  flag mode hits an unrelated prelude-test runtime panic — see gaps).

## Verification

- repro #744/#746 compiles and runs (`1/1 tests passed`) under the flag.
- All six core modules compile as `--include-prelude-tests` target with
  0 errors (`list.kai` was 26 on main — fixed as a side effect).
- selfhost byte-identical; `test-stdlib` 148 OK / 0 FAIL.

## Decisions

- **Kept `CharIndex` (the #744 nominal-record workaround) as-is.** Now
  that the tuple form types correctly, `char_indices` *could* revert to
  `[(Int, Char)]` — but `CharIndex { off, cp }` is a better public API
  than `.fst`/`.snd`, and it already shipped in #744. Reverting would be
  a second breaking change to a just-shipped surface for no gain.

## Coverage gaps / follow-ups

- Under `--include-prelude-tests`, running the *full* prelude test suite
  panics (`non-exhaustive match`) somewhere in option/result's blocks —
  preexisting (present on `main`, unrelated to this fix), which is why
  `test-issue-746` gates on compilation, not the full prelude-test run.
  Worth a separate issue if anyone relies on `--include-prelude-tests`
  for runtime, but it is not a documented supported path today.
