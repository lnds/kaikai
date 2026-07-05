# Lane experience — issue #1083 (variant spine-free teardown)

## Scope as planned vs as shipped

Planned: the bounded RC-traffic lever from the #1083 diagnosis — an
iterative unique-variant tree free in the runtime, mirroring the list
spine-free (#817), in both runtime copies. Explicitly out of scope per
the asu review on the issue: TRMC unrolling (not the lever, not sound
without drop-specialization, reopens the #1054/#1069/#1074 UAF class),
flat-per-ctor layout (downstream), and full drop-specialization in
codegen (its own lane).

Shipped exactly that scope, with one design iteration the brief did not
anticipate (see "Native instruction regression" below): the walk is
entered by handoff from `kai_free_value`'s KAI_VARIANT case only when a
unique variant child actually exists, instead of routing every variant
free through it.

## Measurements (rb-tree 1M, Darwin arm64, median of 5)

Baseline first: the issue's 25.0M decref / 3.97x ratio predates the
#1080/#1081 reuse lanes. Re-measured on current main before touching
anything:

| | baseline | after | delta |
|---|---|---|---|
| decref_total (C) | 14,317,730 | 13,317,731 | **-999,999** |
| decref/alloc ratio | 2.272x | 2.113x | **-7.0%** |
| instructions retired (C) | 2.512G | 2.487G | -1.0% |
| instructions retired (native) | 15.033G | 15.020G | -0.1% |
| wall (C / native) | 0.37s / 0.92s | 0.37s / 0.92s | within noise |

alloc_total, free_total, leaked and program output are byte-identical
before/after on both backends. The -999,999 is exactly the final tree:
1M nodes whose teardown decrefs the fused walk deletes.

The wall clock does not move: teardown is ~0.05s of a 0.37s run and the
saved instructions are cheap ones (call overhead, not cache misses).
The honest claim is "RC ops down 7%, instructions down 1%, wall flat".

## Design decisions

**Worklist, not single-child iteration.** A tree is not a list: a node
has two recursive slots, so the cons-spine trick (follow the one unique
tail in O(1)) does not transfer. The walk uses an explicit
`pending[128]` stack. On a chain the stack oscillates at depth 1; on a
balanced tree it is bounded by tree height (~29 for 1M nodes); on
overflow it falls back to `kai_decref`'s recursive cascade, which is
the status quo. The brief pre-authorized exactly this trade.

**Fused drop does not count as a decref.** #817's cons spine charges
`kai_rc_decref_total++` per consumed cell to stay byte-identical with
the recursive version. This lane deliberately diverges: a unique child
consumed by the walk pays *no* decrement — eliminating that RC op is
the point of the lane, and the counter measures RC ops performed. The
free-side ledger (free_total, per-tag frees, alloc-site credit, poison)
is untouched — every cell still passes KAI_RECYCLE_CELL exactly once,
so the leak/double-free accounting the rc-detector checks is intact.
The cons spine keeps its #817 counter semantics; changing it would
shift list-fixture RC reports for no benefit.

## Native instruction regression (the surprise)

The first implementation replaced the whole KAI_VARIANT case with a
call into the walk. C backend: -26M instructions. Native: **+172M**
(stable across runs), wall unchanged. Isolation steps that located it:

1. Regenerating the runtime bitcode from the baseline header reproduced
   baseline counts — so the regression was the change, not the .bc.
2. `nm` size diff: `insert_loop` and `main` byte-identical; only
   `kai_free_value` grew. The cost was per-free, in the ~5.3M run-time
   frees whose children are shared and gain nothing from a walk.
3. Unfolding kai_decref's rc>1 arm inline (one rc load, no re-checks)
   did not help; `static inline` on the walk did not help. The
   1KB worklist frame plus its stack guard (`___stack_chk_fail`
   appears in the disassembly) taxed every variant free.

Fix: the KAI_VARIANT case keeps a cheap flat loop (shared children
decremented inline, exactly the operations the old code did) and hands
off to the out-of-line walk only on the first unique variant child —
so the worklist frame is only paid by teardown-shaped frees. Result:
native -13M vs baseline, C -25M. Lesson for future runtime lanes: on
the native pipeline, measure per-call frame effects of a hot-path
helper (instructions retired via `/usr/bin/time -l`), not just the
algorithmic delta; the two backends' inliners price the same C very
differently.

## What this lane did NOT do (follow-ups)

The ratio is 2.11x after this lane — still above asu's 1.5x over-RC
line. The residual lives in the insert loop's scattered inc/decref
pairs (incref 13.3M ≈ decref 13.3M: they are dup/drop pairs Perceus
could elide but does not), not in teardown. That is perceus.kai /
borrow-analysis work and this week's UAF history says it needs its own
lane with its own soundness cycle. Downstream and still open on #1083:
drop-specialization in codegen, then flat-per-ctor layout, then (only
after drop-spec) TRMC unrolling.

## Fixtures and gates

- `examples/perceus/variant_spine_free_1083.kai` + golden: perfect tree
  (worklist ~ height), 20K chain (deeper than the cap; stack stays
  shallow), and a shared subtree read *after* its parent's teardown (a
  walk that wrongly consumed a shared child corrupts the output).
- `test-perceus-1083-variant-spine-free` (stage2/Makefile, wired into
  tier1-native): both backends against the golden + decref_total
  ceiling 220,000 — measured 151,267 with the walk, 302,336 with the
  cascade, so the gate separates the two by 2x.
- Corpus entry in `tools/rc-detector-corpus.txt` (teardown shape was
  uncovered): ASAN + no-cell-pool + strict ledger on both backends.

Verification run locally: tier0 (includes byte-id C selfhost), the
perceus reuse gates (817/872/882/995/1025/1053/1069), serial backend
parity (BACKEND_PARITY_JOBS=1), rc-selfhost-detector on both backends,
and ASAN+UBSan at -O2 on the fixture and the 1M bench.

## Cost vs estimate

One design iteration more than the naive "mirror #817" plan: the
handoff redesign after the native instruction measurement. The
measurement discipline (baseline first, counterfactual for the gate
ceiling, isolate-the-.bc step) consumed most of the lane and was all
of its value: without the instructions-retired check the lane would
have shipped a native regression hidden inside an unchanged wall.
