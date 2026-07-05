# Lane experience — issue #1053 residual (nested rotation reuse)

## Scope as planned vs as shipped

The brief ordered the second half of #1053: port the C oracle's Koka
2-of-3 rotation reuse (outer `_scr` overwrite + `_ru_inner` donation) to
the native lowering, mirroring `lower_reuse_dual`, with a
`kaix_reuse_steal_slot` runtime one-liner and per-branch multiset
accounting. Target: rb-tree 1M `alloc_total` from 1.95x toward ~1.0x.

Shipped: the dual-branch nested reuse as designed — **plus a second shape
the design map missed** — and **zero new runtime helpers** (the planned
`kaix_reuse_steal_slot(outer, nslot, n)` is exactly
`KProj(donor, nslot)` + the existing `kaix_drop_reuse_token(inner, n)`;
composing the two primitives beats adding a third).

Measured on rb-tree 1M inserts (native backend, `KAI_TRACE_RC`):

| stage | alloc_total | vs C (6,301,558) |
|---|---|---|
| baseline (post-#1080) | 12,258,918 | 1.95x |
| + rotation dual-branch (nested pattern) | 10,947,242 | 1.74x |
| + flat-pattern nested-rebuild overwrite | **6,301,552** | **1.0000x** |

`reuse_in_place` = 6,301,528 — byte-equal to the C backend's count. Wall
0.92s (from 0.95s; the C column of the same harness runs 0.33s — the
remaining wall gap is not allocation-shaped). The sequential-insert
fixture reproduces the same exact parity (815,039 vs 815,045).

## The diagnosis the design map missed

The retro's residual analysis attributed the whole 1.95x to the rotation
arms. Implementing exactly that bought only 12.26M → 10.95M: the rb-tree's
~656k rotations each save 2 allocs, ≈1.3M total — consistent, and nowhere
near the 5.96M gap. The dominant residual was the **recolor-wrap shape**:
a FLAT pattern whose rebuild nests a fresh sub-ctor
(`RBNode(_, lx, kx, vx, rx) -> RBNode(Black, RBNode(Red, lx, ...), k, v, r)`,
balance's non-rotating arm — far more frequent than a rotation). The C
oracle overwrites the outer shell there too (its nested path with
`has_inner = false`); native fresh-alloc'd both cells. The fix is the same
dual fork with `nslot = -1`: no inner steal, no donation, outer overwrite +
per-slot RC settlement only. Lesson: a residual attribution derived from
reading the lowering is a hypothesis; only the per-stage `KAI_TRACE_RC`
delta confirms which shape actually carries the megabytes.

## Design

- `pat_reuse_nested` (new `kir_lower_reuse.kai`) classifies a reuse arm's
  pattern into a per-slot RC plan (`ReuseNested` / `RNSlot` in `LowerSt`,
  set alongside `reuse_gc`, same arm-scoped discipline): `RNBind` for a
  live heap-box binder, `RNDrop` for a binderless heap-box slot, nothing
  for tagged-immediate slots. Exactly one nested sub-pattern → rotation
  plan; none → flat plan; anything unsupported (two nested slots, `PAs`,
  `PList`, deeper nesting) → `RNInone`, keeping today's fresh path.
- `lower_reuse_nested_dual` (kir_lower_walk) mirrors `lower_reuse_dual`'s
  fork: `kaix_check_unique` condbr, args lowered per branch. UNIQUE:
  project the inner cell, steal its shell (`KTokenSteal` →
  `kaix_drop_reuse_token`, null when the inner is shared), settle RC
  (`rn_direct_acct` / `rn_grand_acct`), lower the rebuild donating the
  token to the first same-arity sub-ctor, overwrite the outer via
  `KConReuse`. SHARED: the pre-existing fresh path (unconditional leaf
  dups + `KCon`; the match-exit cascade reclaims donor + inner).
- Accounting invariant: in the unique branch every direct slot's old
  reference is owned by the rebuild (embedded binder: `uses-1` extra
  dups; unused: drop; binderless: drop by projection); every grand slot's
  ownership is conditional on the steal, settled by one
  `kaix_incref_if_token_null` per slot so both token outcomes own exactly
  one reference. The donated inner is rebuilt via `kai_variant_at` (no
  slot decref); an undonated token is freed shell-only (`KFreeToken`,
  previously `nemit_unsupported`, now wired).

## The stage1 miscompile bit again — twice

The #1080 retro's warning held. Two distinct shapes died, both surfacing
as `panic: non-exhaustive match` when a *later consumer* (the KIR dump /
the native translator) matched on a corrupted heap value:

1. `KDo(KCall("kaix_incref_if_token_null", [tok, rn_reg(st, nm)]), pos)` —
   a call nested inside a list literal inside a ctor inside a statement
   ctor. Flattening to the `bind_op(st, KCall(sym, [a, b]), pos)` shape
   (the `kaix_check_unique` precedent) fixed that site.
2. A **new 4-slot KOp ctor** (`KConAt(KVal, String, Int, [KSlotInit])`):
   its slot-0 KVal was corrupted between construction and the dump match,
   with slots 1-3 intact — even with the atom let-bound first, even
   constructed inline. Never root-caused (out of lane); the shipped
   workaround deletes the ctor entirely and rides the donation on the
   **existing proven ctor**: `KConReuse` gained a third `KReuseKind`
   (`RkTokenVariant`), whose native emission calls `kaix_variant_at_argv`
   with the token as `args[0]` — the exact buffer-carried call shape #1080
   validated. `KTokenSteal(KVal, Int)`, same construction sites, same
   consumers, never misbehaved.

Debug method that converged: bisecting eprint tracing at phase / stmt /
op-slot granularity, one rebuild per probe. The folklore rule stands and
gets a sharper corollary: *when a new KIR node is needed, prefer widening
an existing ctor's kind enum over minting a new ctor shape* — the enum
arm is a data-only change the stage1 RC pass cannot perturb. The token
also travels between helpers as a register NAME (fresh `KVar` per use),
never as one KVal atom reused across calls — same family of hazard.

## Gates run

- rb-tree 1M alloc-ratio: **1.95x → 1.0000x** (the closure criterion).
- New fixture `nested_rotation_reuse_1053.kai` (100k sequential inserts —
  a rotation per level) + golden + alloc ceiling, wired as
  `test-perceus-1053-nested-rotation-reuse` into tier1-native shard 2.
- Existing reuse gates green: #872, #882, #995 (both), #1069, #1080 spine,
  `nested_pattern_reuse_balance` corpus.
- tier0 + selfhost byte-identity; serial backend parity
  (`BACKEND_PARITY_JOBS=1`); ASAN-on-selfhost clean on BOTH backends
  (the soundness gate for this subsystem — it produced the #1054/#1069/
  #1074 UAF family); `-O2` + UBSan on the bench.

## Cost vs estimate

The mechanical port of the mapped design was straightforward (~1 day of
lane time); >half the wall went to the two stage1 miscompiles (diagnosed
by eprint bisection, one ~3-minute rebuild per probe) and to discovering
the flat-pattern shape the map missed. The per-stage measurement
discipline caught both: the first bench run after the "complete" rotation
port showed 1.74x, refuting completeness before any celebration.

## Follow-ups

- stage1 kaic1 corrupts slot 0 of a newly added 4-slot KOp ctor under the
  dump/translate match (repro preserved in this lane's history: the
  `KConAt` A/B against `RkTokenVariant`) — same family as the #1080
  binder-perturbation bug, needs its own issue.
- The wall gap vs the C backend (~0.92s vs ~0.33s at alloc parity) is now
  non-allocation-shaped (dispatch / inlining / RC traffic) — separate
  profiling lane if wanted.
- `RNInone` fallbacks (two nested slots, `PAs`, deeper nesting) keep the
  fresh path; none appear in the perceus corpus today.
