# Perceus ownership contract — refactor design

`stage2/compiler/perceus.kai` is 6237 lines and scores `km` F--. The
instinct is to split it. Measurement says splitting alone fixes nothing:
cognitive complexity averages 4.1 per function and duplication scores
A++, so the functions are fine — there are simply 352 of them, and the
rule they implement is copied elsewhere.

The defect class this refactor closes is *the same ownership fact derived
in N places that drift apart*. A prior audit attributed ~12 of ~22
historical RC issues to it.

## Evidence

The exit-drop rule exists in seven places:

| where | note |
|---|---|
| `perceus.kai` `pcs_collect_exit_drops` | canonical |
| `perceus.kai` `pcs_collect_block_let_exit_drops` | second copy, same file |
| `perceus_tail_drop.kai`, `perceus_plant_drop.kai` | coordinated by convention |
| `emit_c.kai` ×4 | textual mirrors |
| `emit_native_fn.kai` `nemit_drop_assigns_masked` | mirrors the emit_c mirror |

The second copy lives inside `perceus.kai` itself. Physical proximity did
not prevent it, so file organisation is not the mechanism that would.

The contract is the decorated AST: consumers recover decisions by pattern
matching on `__perceus_*` sentinels rather than reading a value. So
`arm_binder_is_moved` asks whether a binder appears bare and not
dup-wrapped, and its own comment concedes the mechanism — *"we read the
decision perceus already baked into the tree"*. Its second conjunct,
`arm_binder_only_dup_wrapped`, returns `false` unconditionally: half the
predicate is decorative.

Baseline, both backends:

| metric | `emit_c` | native |
|---|---:|---:|
| signatures taking `[Use]` / `[BorrowEntry]` / `[ConsumeEntry]` | 14 | 1 |
| `__perceus_*` sentinel sites | 64 | 33 |

Seven modules outside Perceus take `[Use]`: `emit_c`, `emit_shared`,
`fnreg`, `kir_lower_walk`, `infer`, `unbox`, `rawsafe`.

The backends are independent — native mirrors emit_c's *rules*, never its
code — so the three-hop chain can collapse to `perceus → {emit_c, native}`.

## Precedent

Two mechanisms in this repo already solved this shape.

`driver.kai` computes `raw_arm_binders` once and passes it explicitly to
Perceus and to lowering, *"so no phase re-derives rawness from the mutated
body"*. That is this design applied to rawness; ownership extends it.

`expr_positions.kai` made sub-expression positions a table every walker
reads, with a tier0 ratchet blocking new hand-rolled enumerators. Walkers
went 17 → 11 and the class stopped recurring.

## The performance constraint

Perceus is a load-bearing claim of the language, and the margin is thin:
rb-tree at N=1M runs C 0.25s, Koka 0.25s, kaikai-c 0.40s, kaikai-native
0.48s; instructions retired are ~2.49G (C) against ~6.27G (native).

**That margin is currently unguarded.** `rc-budget` runs the RC trace over
the whole compiler and greps only `leaked=`; `reuse_in_place` appears in
the same log and is discarded. The `>= 1500` floors in `stage2/Makefile`
are per-fixture, not global. Worse, `emit_c` injects
`kai_rc_reuse_total++` as C text while native calls the runtime helper —
the two counts do not travel the same path even when they agree.

A refactor that degrades global reuse passes CI green today. Stage 0
exists to close that hole before anything else moves.

## Stages

Each stage is one lane, in order. Stages 1 and 2 do not split across
lanes: each needs one head holding the whole decision tree, and splitting
them reproduces the very defect class inside the refactor.

**Stage 0 — arm the perf gate.** Blocking. Extend `rc-budget` to extract
`reuse_in_place` with a floor, both backends, and route the C backend's
counter through the runtime helper so both sides count the same way.

**Stage 1 — `OwnershipEntry`, inside the monolith.** Collapse `bmap`,
`cmap`, `opaque_names` and `opaque_ret_fns` into one keyed table. No files
move. Highest risk and highest payoff, inseparably: if two tables diverge
today and agree by luck, collapsing them surfaces it. Mitigated by a
temporary assertion that borrowed and consumed positions are disjoint per
key, checked before the unified table is built and removed when the stage
closes.

**Stage 2 — `ExitDropPlan`, and close the backend leak.** One producer,
N readers. `emit_c` and native each migrate to the *contract*, never to
each other. `arm_binder_only_dup_wrapped` is resolved here: completed
against the contract, or deleted.

**Stage 3 — split into modules.** Only now, and mechanically: the contract
is stable, so moving code is relocation verified by byte-identical
selfhost, and parallelises across lanes. The two exit-drop copies merge
into one function parameterised by scope — placing them side by side in a
new file would relocate the duplication, not remove it.

**Stage 4 — re-measure and close.**

## Acceptance

Mechanical, and checked at every stage boundary:

- **Contract (the discriminating gate).** Signatures outside Perceus
  taking `[Use]` / `[BorrowEntry]` / `[ConsumeEntry]`: 14 + 1 → 0. A
  signature type cannot be faked down; lowering it requires moving the
  decision upstream.
- **Surface.** `__perceus_*` sentinel sites, ratcheted down from 64 / 33.
- **Performance.** `reuse_in_place` floors and rb-tree instructions
  retired, both backends. Never a single run — the daily runner's variance
  has produced false regressions before.
- **Identity.** Byte-identical selfhost, and `tools/test-native-selfhost-gate.sh`,
  which caught two PRs that fixtures alone passed.

Verify on both backends. tier0 runs the C backend and reports green on
native-only breakage.
