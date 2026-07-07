# Lane experience — native variant field read (#1083)

## Scope as planned vs as shipped

**Planned (brief):** the native rb-tree was 2.73x the C backend's instruction
count. The brief asserted — with a "PROVED BY BISECTION" framing — that the
cause was the navigation reading the child POINTER slot (kind-0) via
`kaix_variant_arg`, which branches on the tag's slot mask on every read. The
brief ordered the pointer fix FIRST ("the Int is marginal") and listed
RC-cancellation as already ruled out ("RC identical").

**Shipped:** ONE commit — BLOQUE 1 (`KProjBorrow`, kind-0 pointer). A second
change (BLOQUE 2, kind-1 Int raw read) was implemented, measured to be the real
lever, and then REVERTED after the native self-host gate caught a segfault it
introduces. It is a documented follow-up.

- BLOQUE 1 — `KProjBorrow`: a `KProj` sibling for a kind-0 pointer slot,
  lowered to `kaix_variant_arg_borrow` (`slots[i].ptr`, no mask branch). Sound;
  emitted C byte-identical (KIR-only); native self-host gate green. Full-insert
  bench 2.73x → **2.52x** C (~8%).

- BLOQUE 2 — `SInt64` binder for kind-1 Int slots (REVERTED): read the key
  `.i64` raw at the binder. On a pure-lookup isolation bench it collapsed the
  native/C gap from 2.01x to **1.08x** — the true lever. But it SEGFAULTS the
  native self-host gate's self-compile step (see below). Reverted; follow-up.

## The brief's diagnosis was inverted — measure before trusting a "bisected" claim

The rb-tree descent binds three things per level: the two pointer children
(kind-0) and the Int key (kind-1). The brief was certain the pointer read was
the cost. A pure-lookup isolation bench (fixed 1M tree, 10M descents, zero
construction/RC/balance) disproved it:

- Base and after BLOQUE 1 (pointers already borrowed): **2.01x**.
- The emitted C for `rb_get` showed both backends already borrow the pointer
  children identically (`kai_incref(slots[i].ptr)` — RC never differed there).
  The gap was the KEY: native read it BOXED (`dup`/`drop` + `int.unbox`×3 per
  level) while C read `slots[i].i64` raw.
- After BLOQUE 2 (key read raw): **1.08x**.

Lesson: a "proved by bisection" brief claim is an INPUT, not a verdict. The
cheap disproof was one isolation bench plus reading the emitted C for the hot
function side by side against the C oracle.

## Why BLOQUE 2 was reverted — the #709 phantom-box class, caught by the right gate

The native self-host gate (`tools/test-native-selfhost-gate.sh`) builds the
compiler WITH the native backend and then RUNS it to compile a program. With
BLOQUE 2, that self-compile step **SIGSEGVs on Linux -O2** — the classic #709
phantom box: a variant Int field is read raw (`.i64`) at some site where a
consumer treats it as boxed (a pointer), and the deref of a scalar-as-pointer
crashes. The compiler uses Int-in-variant fields intensively (AST/KIR nodes),
so the hole reproduces there even though the small rb-tree fixtures do not hit
it.

Two things made this hard to catch and hard to close:

1. **macOS -O0 hides it.** The local selfhost byte-id gate (mac) passed
   green — mac's -O0 codegen tolerates the mistyped read. The Linux -O2 native
   self-host gate is exactly the gate that exposes it, and it is not runnable
   locally on this mac (the native compiler link even fails here). So the fix
   could not be validated locally. This is the recurring "native self-host gate
   self-compile catches segfaults mac hides" trap.

2. **The border-reboxing surface is wide.** BLOQUE 2's soundness rests on every
   consumer of a raw kind-1 register re-boxing at its border. The read side
   (`nemit_load_reg_boxed`) and the RC plan (the `nemit_rc` no-op on raw
   registers) were covered, but the self-compile segfault proves at least one
   border is not — most likely the generic/monomorphised case (a field declared
   as a type var, `Int` only after monomorphisation, where construction and the
   binder's `variant_slot_kind(te, [])` classification can disagree). Auditing
   ALL such borders across the whole compiler without a Linux -O2 harness to
   validate against is not something this lane could close with confidence.

Decision: do NOT sell parity that does not pass the gate. Revert BLOQUE 2, ship
BLOQUE 1 (sound, gate-green, its partial win), and file kind-1 as a follow-up
with the honest number (the 1.08x is achievable but blocked on the border
audit + a Linux -O2 validation loop).

A second CI red (test-kir golden: `r: i64 = proj s.0` vs the golden's
`r: box = proj s.0`) shares the same origin — the `SInt64` binder changes the
dump. Reverting BLOQUE 2 removes it too; no golden regen needed.

## Design decisions (BLOQUE 1, shipped)

**KProjBorrow as a new KOp, not a new KSlot (asu-reviewed).** A kind-0 pointer
slot read as a direct borrow behaves identically to `SBoxed` in every place
`KSlot` decides a register's type (~14 sites: drop-walker,
`nslot_type_of_tag`, `native_ctx_reg_slot_at`). A new `SPtr` slot would force a
sweep of all of them, and a single miss in the drop-walker is a UAF. A new KOp
`KProjBorrow(KVal, Int)` keeps the register `SBoxed` (a `ptr`, identical
store), so no `KSlot` consumer changes; the sweep is three arms
(`emit_native_ops`, `emit_native_term`, `kir_dump`) whose fallback for a KOp
walker is "treat like `KProj`". The C backend emits from the AST, never the
KIR, so `KProjBorrow` leaves the emitted C byte-identical.

**read-matches-write via a single `slot_kind_of` with empty `enums`.** The
construction path classifies slots with `variant_slot_kind(te, [])` — enums
always `[]`. The binder uses the identical call. An enum-typed slot classifies
kind-0 (boxed) under `enums = []`, exactly how the ctor built it, so a `.ptr`
borrow is correct.

## The residual insert gap is construction codegen, NOT field reads or RC

Measured: the full-insert bench (BLOQUE 1 only) is ~2.52x C. It is NOT RC and
NOT allocs — both are identical across backends (alloc_total, reuse_in_place,
incref/decref all match under KAI_TRACE_RC). It is the `Node` CONSTRUCTION
codegen in the spine rebuild: native emits the generic `kai_variant_u` +
separate `kai_alloc` per node where C emits the fixed-arity fast path
`kai_variant_u_fast` + `kai_slab_alloc`. That is the #1053/SROA
fixed-arity-ctor surface, distinct from the field read this lane touched.

## Fixtures

`examples/perceus/native_field_read.kai` — a build-probe-discard driver (2000
rounds) binding a variant's pointer children on every descent level. Gated in
tier1-native (`test-native-1083-field-read`) on native==golden, native==C, and
native `alloc_total` ≤ C's + 10 under `KAI_TRACE_RC` (the direct borrow keeps
the mask read's RC balance).

## Coverage gaps / follow-ups

- **Kind-1 Int raw read (BLOQUE 2)** — the 1.08x lever, blocked on: (a) a full
  audit of raw-kind-1-register consumers for missing border re-box (the
  generic/monomorphised field is the prime suspect), (b) a Linux -O2 native
  self-host loop to validate, since mac hides the segfault. Its RC guard
  (`nemit_rc` no-op on raw registers) and the `SInt64` binder are the right
  shape; the gap is the border audit.
- The decision-tree test path (`kir_lower_variant`) still reads pointer/Int
  slots via mask-consulting `KProj`. Not covered; follow-up if hot.
- The residual full-insert gap (construction codegen, #1053/SROA surface).

## Cost vs estimate

Larger than a one-slot change: the brief's diagnosis was inverted (real work =
disproving the pointer hypothesis), and the winning change (BLOQUE 2) then hit
the phantom-box border and had to be reverted after the self-host gate — the
gate mac cannot run — flagged it. Two asu reviews (mechanism choice; the pivot
to BLOQUE 2 and the RC-plan border) were high-leverage. The honest outcome is a
sound partial (pointer borrow, ~8%) plus a well-characterised follow-up, not
the headline 1.08x that does not pass the gate.
