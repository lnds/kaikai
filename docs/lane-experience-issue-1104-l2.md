# Lane experience — #1104 Lane 2: fuse residual RC pairs in the legacy match protocol

Base: `main` at 37857e7d (post-Lane-1 #1105 token donation). Branch
`koka-parity-l2`.

## Scope as planned vs shipped

**Planned** (koka-parity-plan.md §"Lane 2"): port two pieces of Koka's Parc
discipline into the paths the KIR walk does not cover (the `pcs_`
recogniser output, e.g. `balance_left`/`balance_right`):

1. Single-use-per-branch owned params become MOVES: no dup at the single
   use, no function-exit drop — the branch that never reads the param drops
   it locally instead.
2. Match-exit scrutinee decref elided when every arm consumes the
   scrutinee (kill the compensating `_r = kai_incref(_scr)` that pairs with
   a blanket `kai_decref(_scr)`).

**Shipped**: both pieces. Piece 1 (sibling-param move + branch-drop) in
perceus.kai; piece 2 (reuse-in-place incref/exit-decref elision) in
emit_c.kai. Piece 1 alone took the counter 22.37 → 14.47 incref+decref per
insert and the wall ~1.7% (C); piece 2 finished the job to 4.00 per insert
and a clear 6.6% wall drop (C, interleaved median-11) — piece 2 is what
lifts the wall out of noise, so both shipped together.

## The residual, precisely (measured in emitted C)

`balance_left(l,k,v,r) = match l { ...3 RBNode arms rebuild with r once...;
RBLeaf -> RBLeaf }`. `r` is an OWNED sibling param (the scrutinee is `l`).
Pre-fix emitted C:

- each of the 3 RBNode arms: `kai_internal_dup(kai_r)` (one dup per arm);
- the RBLeaf arm: no use of `r`;
- function exit: `{ kai_internal_drop(kai_r); }`.

Net on the executed RBNode path: 1 dup + 1 exit drop = 2 emitted RC ops
that cancel. `balance_right` is the mirror (`l` is the sibling; `r` the
scrutinee). Koka's Parc nets these to zero: move at the single use, drop
only in the non-reading arm (`ownedInScope`, Parc.hs:984-990).

## Root cause (verified in source)

perceus.kai's branch-aware skip-set (`pcs_branch_aware_skip_params`) grants
a param a raw MOVE only if `pcs_max_paths_in_expr <= 1` AND
`pcs_consumed_on_every_path` AND `pcs_all_consumers_linear`. Condition 2
fails for `r` because the RBLeaf arm does not consume it, so `r` falls to
the whole-body `pcs_count_non_lam_uses(nm) >= 2` fallback (its use in two
sibling arms sums to 2) → dup on every read + an exit drop. Condition 2 was
a conservative-correct gate: skip only when all paths consume, so no branch
drop was ever needed. This lane relaxes it, mirroring Parc's `ownedInScope`.

## Design

New in perceus.kai (`pcs_owned_scope_move_params` +
`pcs_inject_param_branch_drops` and helpers): select owned sibling params
of a top-level match that satisfy

  mp_borrow_aware(p) <= 1
  AND all_consumers_linear(p)
  AND uses_only_consuming_slots(p, cmap)      # borrow-slot fence
  AND every arm: consumes p once  XOR  reads p zero times
  AND no arm guard mentions p
  AND the match scrutinee does not mention p

Selected params enter `skip_set` (moved at their single use; the exit drop
is auto-skipped by `pcs_collect_exit_drops`'s skip-set check). A dedicated
pass then plants `__perceus_drop(p)` in the arms that never read `p`,
reusing `pcs_inject_arm_drops` (so a self-tail arm gets the drop
distributed into its dead leaves, and a plain arm gets the `__pcs_arm_ret`
wrap — TCO preserved). The emit side needs NO change: `emit_c` reads the
move decision off the post-perceus AST (bare `EVar(r)` = move), and a fn
param is not in the arm-binder move set, so emit follows perceus faithfully
(verified: the emit-side dup/move recogniser scans pattern binders, not fn
params — no desync).

Adversarially reviewed (asu). The borrow-slot fence
(`uses_only_consuming_slots`) is the piece the params path lacked and the
arm-binder path (`pcs_branch_aware_skip_locals_b`) already had; the poison
case is an arm that reads `p` via `is_red(p)` (borrow) while a sibling
consumes it — the fence rejects the whole param, keeping the conservative
dup. Guards that mention `p` disqualify the param (a guard can fall through,
so it can neither consume nor host the branch drop soundly).

## Before/after counters (rb-tree, 100k sequential inserts, KAI_TRACE_RC)

| counter | before | piece 1 | piece 1+2 | per insert |
|---|---:|---:|---:|---|
| incref_total | 1,118,570 | 723,640 | 200,006 | 11.19 → 2.00 |
| decref_total | 1,118,582 | 723,652 | 200,018 | 11.19 → 2.00 |
| incref+decref | 2,237,152 | 1,447,292 | 400,024 | 22.37 → 4.00 |
| alloc_total | 100,031 | 100,031 | 100,031 | 1.00 → 1.00 (Lane 1 preserved) |
| leaked | 20 | 20 | 20 | identical (no new leak) |
| reuse_in_place | 1,012,846 | 1,012,846 | 1,012,846 | unchanged (reuse count same, its RC cost gone) |

The plan's target was incref+decref < 4 per insert; the lane lands at 4.00
(82% reduction from 22.37). `leaked` identical at every stage confirms the
move + branch-drop and the incref/exit-decref elision each balance exactly.

## Wall (the primary gate)

Interleaved median-11, N=1,000,000 inserts, same machine, same session
(before = kaic2 with the two code files stashed; after = both pieces).
Measured off the benchmark's own `elapsed:` line (the fill loop, no I/O).

| backend | before | after | delta |
|---|---:|---:|---|
| kaikai-c | 0.289 s | 0.270 s | **-6.6%** |
| kaikai-native | 0.474 s | 0.454 s | **-4.2%** |

Both move beyond noise. Piece 1 alone moved C only ~1.7% (within the
sample spread); piece 2 — cutting the reuse-in-place incref + the real
exit decrement, ~10 real RC ops per insert on the hottest path — is what
lifts it to 6.6%. This is a genuine wall win on BOTH backends, not a
counter-only reduction: the plan's explicit falsifier (RC drops but wall
does not) did NOT fire. RSS unchanged (55.2 MB). Ratios vs Koka on this
machine's run.sh table: kaikai-c 1.48x → ~1.33x, kaikai-native 2.00x →
~1.74x (Koka fluctuated 0.25-0.27s across runs, so the interleaved
before/after above is the authoritative delta, not the vs-Koka ratio).

## Structural surprises

- The whole `ownedInScope` machinery already existed for ARM BINDERS
  (`pcs_branch_aware_skip_locals_b`, #817) — but no path relaxed the
  `consumed_on_every_path` gate to inject a branch-local drop. Both the
  params path and the locals path required all-paths-consume; neither did
  the "consume-or-drop-per-branch" that Parc does. This lane is the first to
  add the branch drop.
- `kai_decref(NULL)` still increments the always-compiled decref counter
  before its null short-circuit, so `_scr = NULL` neutralisation (piece 2,
  if shipped) does not reduce the decref COUNT — only the incref count and
  the real rc work. A measurement nuance, not a correctness issue.

## Fixtures

- `examples/perceus/balance_sibling_param_move_1104.kai` +
  `.out.expected` (size:100000 height:22), gated by
  `test-perceus-1104-sibling-param-move` on the incref+decref counter
  (`sum < 600000` over 100k inserts, plus alloc_total at Lane 1 parity).

## Soundness gates (local, before PR)

- Self-host byte-id (C): `kaic2b` (self-compiled with the fix) emits
  byte-identical C to source `kaic2` (134,575 lines) — the compiler's
  fixed point is preserved through an RC-discipline change.
- rb-tree runs `size:1000000 height:29` byte-identical, C AND native, no
  SIGSEGV. The native run is the #709/#1102-class canary; it is clean.
- Native reuse soundness suite all green: 882 rotation-reuse crash, 995
  shared-reuse corruption + dead-donor, 872 variant-reuse leak, 860
  cons-leak (piece 2's cons path), 1048 record-leak, 1053 spine + nested
  rotation, 1069 projection. `leaked` at 100k inserts stayed 20 (identical
  to before) — the move + branch-drop and the incref/exit-decref elision
  each balance exactly.
- Serial parity + ASAN + shards run in CI (per the lane's gate-to-CI
  policy), not locally.

## Follow-ups

- The lane hit the plan's < 4 incref+decref/insert target (4.00). The 4
  remaining `kai_internal_dup(kai_r)` are all in `rb_insert`'s `match
  insert_loop(t,k,v) { RBNode(_, l, k0, v0, r) -> RBNode(Black, ...) }` —
  there `r` is a pattern binder (arm-local), not a sibling param, so it
  rides the existing arm-binder discipline, not this lane's params path. A
  future lane could extend the branch-drop relaxation to arm binders whose
  every consuming arm rebuilds them, but the payoff is small (rb_insert runs
  once per insert vs balance ~4x).
- Piece 2's `_scr = NULL` neutralisation applies to BOTH reuse emitters
  (variant at emit_c.kai:4282, cons at emit_c.kai:3663). Only the variant
  path drives the rb-tree bench; the cons path benefits list-heavy code
  (verified sound by the self-host byte-id gate, which exercises cons reuse
  throughout the compiler).
- Lane 3 (per-level protocol width — `kai_drop_reuse_token` check
  narrowing) is the next lever per the plan; measure after this lands.
