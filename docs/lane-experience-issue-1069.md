# Lane experience — issue #1069 (perceus variant reuse donates a shared cell)

## Scope as planned vs as shipped

Planned: fix the reuse-in-place double-free (#1069), then re-apply the #1048
record scope-exit drop on top, closing both. The issue offered two fix
directions: (A) reinstate the diagonal gate, or (B) incref the reused child
when shared.

Shipped: a third, narrower cut of (B) — gate reuse RECOGNITION on a
counted-owner scrutinee. (A) and (B)-as-written were both rejected on
inspection; see below. The #1048 re-apply was attempted, verified green on its
own fixture, and then WITHDRAWN: the drop surfaces a further, distinct
under-count in the native reuse LOWERING (shared-donor consumed children —
details below), so #1048 stays blocked on that named residual, no longer on
this issue.

## The actual mechanism, one level deeper than the issue

The issue's diagnosis (the un-counted sibling field read,
`pcs_consumes_kind(EField(EVar)) = false`) is correct but the load-bearing
consequence is at the SCRUTINEE, not the arm: `pcs_recognise_reuse_arms`
accepted any `match` scrutinee as a donation source. For `match e.kind { … }`
the matched cell is owned by `e`; that claim is invisible to the runtime
`rc==1` uniqueness guard whenever the projection read is a borrow (the native
field load), so `kai_variant_reuse_at` donated a cell with a live second owner
and moved its children without incref. The same hazard covers `xs[i]`
scrutinees and the cons/record reuse shapes, which is why the fix sits at the
single recognition site rather than in `pcs_consumes_kind` (whose consumers —
the consume-map, arm moves — are caller-side move decisions, a different
axis).

Scrutinees that ARE counted stay eligible: a var (every non-last read is
dup'd, so a var holding the only ref at its last use is genuinely unique) and
a call result (callees return owned refs; a shared return shows rc>1 and the
runtime guard falls back). `__perceus_borrow`-wrapped matches never overlap
recognition — borrow inference requires inspect-only arms, reuse requires a
rebuild from binders.

## Why not (A), why not (B)-as-written

(A) — reinstating `pcs_reuse_is_diagonal` — does not fix the bug: a DIAGONAL
rebuild over a projection scrutinee donates all the same (the diagonal
predicate constrains the arm's slots, not the scrutinee's ownership), and the
`KNum(n) -> KNum(n+1)` unboxed-slot shape passes the diagonal test. It would
also have killed the rb-tree rotation reuse that 113e2991 shipped, paying perf
for no soundness. The orphaned diagonal helpers are deleted, not revived.

(B)-as-written — "incref the shared child" — needs a definition of "shared
with a live sibling" at emit time; the only sound approximation available to
the recogniser IS the scrutinee's ownership shape, which collapses (B) into
the shipped gate. Making `pcs_consumes_kind` count field reads was rejected as
a cross-cutting change to every consume-map consumer for a bug whose decision
point is one function.

## The role of 113e2991 — partially exculpated

The issue fingered 113e2991 ("drop the diagonal gate") as the regression. In
fact the projection-scrutinee donation PREDATES the diagonal gate era: a
diagonal arm over `match e.kind` donated under the old gate too. What
113e2991 did was widen the exposure enormously — non-diagonal rebuilds are
exactly the compiler's own `EMatch(rqc(s), rqc_arms(arms))` walk shape, so
after it, essentially every AST walk in the self-compile became a donation
site. Post-fix, the compiler's own self-compiled C contains ONE
`kai_variant_reuse_at` site (an owned-var match inside
`emit_reuse_variant_body`, fittingly) and ~209 `kai_reuse_or_alloc_variant`
sites, all owned-var arms.

## Perf: the 113e2991 trade is preserved

rb-tree (1M inserts, -O2, KAI_TRACE_RC build): reuse_in_place=6.96M,
alloc_total=6.3M, wall 1.21s — at or above the level 113e2991 recorded
(5.65M / ~1.28s). The rb-tree matches on owned vars, so the gate costs it
nothing. The compiler's own walks lose their (unsound) donations; that cost
is the correctness.

## Gates

- tier0, C selfhost byte-id: green.
- Reuse batteries #118 (+ASAN), nested-reuse/#872 (+ASAN), #882, #995 both
  fixtures, #747: green; #995 dead-donor asserts reuse still fires.
- ASAN-on-selfhost (the gate the bounded repros cannot replace), method as
  in the #1054 retro (runtime cc-compiled under
  `-fsanitize=address -DKAI_NO_CELL_POOL`, bitcode hidden, full self-compile):
  - NATIVE: CLEAN with the fix (the `kaix_variant_reuse_at ← rqc_kind` UAF is
    gone). With the #1048 drop re-applied on top it goes red again — on a
    THIRD bug, not this one (see the shared-donor consumed-child finding).
  - C oracle: still red — and PROVEN unrelated, see below.
- Fixture `reuse_projection_scrutinee_1069`: both backends golden (the
  original tree survives two grow passes un-mutated) + a recognition ratchet
  on the emitted C (`grow_kind` must contain fresh allocators only). Wired
  into `test-perceus-1069-projection-reuse` and the rc-detector corpus.

## Surprises

- The bounded repro the issue called elusive became possible once the fix was
  understood as scrutinee-shaped: keeping the ORIGINAL tree live across the
  walk turns the donation into a visible functional corruption (golden diff),
  no ASAN needed on the backend whose projection reads borrow.
- **The C oracle's ASAN-selfhost red is NOT this bug.** The issue lumped the
  C oracle's heap-buffer-overflow (`typecheck_module`, READ of 8 at 16 bytes
  BEFORE a live 64-byte `kai_variant_u_fast` region born in `expand_ta_decl`)
  in with the native reuse UAF as "the same shared front-end corruption".
  Bisect: sed the self-compiled `kaic2b.c` so ALL 212 donation sites
  (`kai_variant_reuse_at` + `kai_reuse_or_alloc_{variant,record,cons}`) pass
  `NULL` as the donor — i.e. reuse-in-place globally OFF — and the overflow
  reproduces IDENTICALLY (same line, same relative address). It is a
  distinct, pre-existing bug (present on main, masked by the cell pool;
  byte-id is blind to it), read address in the left redzone of a live
  variant block — a stale or under-offset pointer, possibly TRMC/cctx or an
  emit-side slot walk. Split to its own issue rather than stretched into
  this lane.

## Cost vs estimate

The fix itself was small (one predicate + one gate + deletions). The cost
centres were reproducing the ASAN-on-selfhost method (no packaged harness —
reassembled from the #1054 retro + `native-selfhost-link.sh`; the native
ASAN link additionally needs the KAI_LLVM shim TU) and the bisect that
decoupled the C oracle signature from the reuse mechanism.

## The residual that still blocks #1048 — shared-donor CONSUMED children

Re-applying the #1048 drop (fixture green: free_total=6008 ≥ alloc-10) turns
the native ASAN-selfhost red again with a familiar-looking but distinct
stack: UAF read in `kaix_internal_dup ← region__rw_expr`, freed by a planted
drop in `expand_ta_expr`, cell allocated by the FRESH-ALLOC fallback inside
`kaix_variant_reuse_at ← rqc_kind` (frame: `kai_variant_u ←
kaix_variant_reuse_at` — the donation did NOT fire).

Mechanism: when a recognised reuse arm runs against a donor that is SHARED at
runtime, the fresh-alloc fallback still lets the arm extract the donor's
children without incref and CONSUME them (`EMatch(rqc(s), rqc_arms(arms))` —
`s` is an argument, not an embedded use). The #872 compensation
(`reuse_gc` → `kai_incref_if_shared` in the `KConReuse` shared branch) covers
only children the rebuild RE-EMBEDS, and it runs at rebuild time — after the
argument evaluation has already consumed (and freed) the child. The shared
donor's surviving copy then points at freed cells; without the #1048 drop
that copy leaks silently (which is why this lane's no-drop ASAN run is
clean), with the drop the cascade reads the freed child.

Fix direction for the follow-up lane: arm-entry compensation — on a
`__perceus_reuse_*` arm, `incref_if_shared(donor, child)` each extracted
pointer binder the body consumes OUTSIDE embed position, before the body
runs; or unify by moving ALL shared compensation (kept + consumed) to arm
entry and retiring the per-embed incref. Native lowering only
(`kir_lower_walk.kai` `lower_arm_rc` / reuse-gc plumbing); the C oracle's
`kai_reuse_or_alloc_*` path increfs binds and is not affected.

## Follow-ups

- Shared-donor consumed-child under-count in the native reuse lowering
  (above) — the actual remaining blocker of #1048; needs its own issue+lane.
- The C oracle ASAN-selfhost heap-buffer-overflow (above) needs its own
  issue + lane; this lane's evidence (stack, allocation site, donation-off
  bisect) is in the PR.
- An ASAN-on-selfhost harness deserves a `tools/` script + optional CI slot;
  three lanes (#1054, #1069, the next one) have now hand-rolled it.
- `pcs_consumes_kind(EField(EVar)) = false` still under-counts projections
  for the consume-map; benign for reuse after this gate, but worth an audit
  if a future caller-side move decision trusts it for projections.
