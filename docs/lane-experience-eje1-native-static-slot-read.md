# Lane experience — Eje 1: native inline variant-slot read

## Scope as planned vs as shipped

**Planned (brief):** de-opaque the native backend's variant-slot READS so they
read inline (`GEP+load`, kind decided at compile time) like the C backend,
instead of the dynamic `kaix_variant_arg*` shim. Two mechanisms:
1. de-opaque the read itself (pointer / Int / Real slots → `GEP+load`);
2. Route (b) — make match-arm Int/Real binders RAW (`SInt64`/`SReal`) so the
   raw read fires. The brief's honest estimate: ~0.03–0.05s wall on rb-tree,
   gated on the **native self-host -O2** binary soundness gate.

**Shipped:**
- The `GEP+load` slot-read infrastructure (`emit_native_slot.kai`, A++).
- Pointer-slot borrow inline (`KProjBorrow` → `nemit_slot_read_ptr`) — sound,
  passes the gate.
- Route (b) Int/Real raw binders, GATED by a **strict use-site predicate**
  (`binder_read_raw` / `all_uses_raw` in `kir_lower_bind.kai`): a binder is raw
  ONLY when every use is a direct operand of a raw arithmetic/comparison
  `EBinop`. Any other use (a call arg, a ctor slot, interp, a lambda capture)
  keeps it boxed.
- A **native mode-slave re-box gap was discovered and worked around**, not
  fully closed. See "Structural surprises".

## The central structural surprise: the native boxing border is NOT total

The brief's premise — "the #709 re-box border already exists automatically;
CERO LOC nuevo para el border" — is **false for Route (b)**. The C oracle is
**mode-slave**: it reads each `EVar`'s `.mode` and synthesises boxing at EVERY
boxed use-site. The native backend re-boxes ONLY where a value flows through
`nemit_atom(KVar)` (→ `nemit_load_reg_boxed`). Any consumer that reads a
register by another path (`nemit_load_reg` raw, an inlined `kai_intf` tagged
dispatch, Perceus `dup`/`drop`, a closure capture) receives a bare `i64`.

Concretely: with Route (b) making a match-arm Int binder `SInt64`, the
`--emit=native` self-host self-compile **segfaulted** (`ldr x26, [x26, #8]`
with `x26 = 0x8`). A raw `i64` value (a line/col = 8, bit0=0) was mis-dispatched
as a `KaiValue*` by the Koka tagged-immediate check (`kai_is_int`: bit0==1 →
immediate, bit0==0 → pointer). The raw 8 never passed through `kai_int` (which
would tag it `(8<<1)|1 = 0x11`).

asu's verdict (recorded): Route (b) is a **hard technical dependency** on a
native mode-slave re-box pass, not a pending optimisation. There is no shortcut
that gives totality with a special case — replicating the C oracle's exact
`seed_variant_int_binders` predicate (interp + lambda-capture guards) fixed the
lambda-capture crash but the self-compile still segfaulted on other use-sites.

## What actually made the gate pass: a STRICTER predicate than the oracle's

Rather than build the full mode-slave pass, the binder is raw only in the
provably-safe shape: **every use is a raw-arith `EBinop` operand**. This is
stricter than `seed_variant_int_binders` (which seeds MUnboxed whenever no
interp/lambda escapes, trusting the mode-slave emitter to box the rest). The
native has no such emitter, so the predicate must itself be total-by-exclusion.

Consequence: rb-tree keys `kx` are used BOTH in `k < kx` (raw-safe) AND rebuilt
into `RBNode(.., kx, ..)` (a ctor slot — NOT raw-safe under this predicate), so
`kx` stays boxed. **rb-tree gets no measurable win** (see table). The win lands
only on binders used purely in arithmetic.

## The reuse-in-place / TRMC codegen gap (the real blocker for rb-tree's win)

Permitting a raw binder in a kind-MATCHED ctor slot (Int binder → Int slot)
recovers `kx` raw and gives −5.7% instructions on rb-tree — but **re-introduces
the self-compile segfault**, AND the fixture exposed a correctness divergence
(native `419` vs C `922.25`) on a tree whose Int slot is rebuilt via
reuse-in-place. The gap is NOT the predicate: it is the native's
reuse-in-place / TRMC node emitter mishandling a raw `i64` written into a reused
Int slot. The C oracle round-trips it (`kai_take_int(kai_int(kair_kx))`); the
native writes it raw and either corrupts the value or hands a bare `i64` to a
downstream tagged dispatch. This is a separate codegen lane (#1110) — and is the last link to make Route (b) pay off on the hot path.

## Measurements (rb-tree 1M, macOS arm64; median of 5)

| config | wall | cycles | instructions | gate |
|--------|------|--------|--------------|------|
| baseline (main) | 0.424s | 2.25e9 | 6.139e9 | pass |
| pointer read de-opaque only | ~0.438s | ~2.19e9 | 6.139e9 | **pass** |
| Route (b) strict predicate (SHIPPED) | ~0.427s | ~2.16e9 | 6.139e9 | **pass** |
| Route (b) + ctor-slot raw (kind-matched) | ~0.436s | — | 5.786e9 (−5.7%) | **SEGFAULT** |

Honest read: **the shipped lane is a no-op on rb-tree wall/cycles/instructions**
— exactly the brief's "if cycles don't drop measurably, report it, don't sell
it." The pointer-read shim was already inlined by the P2 runtime bitcode, so
de-opaquing it changes nothing the optimiser wasn't already doing; and rb-tree's
Int keys can't go raw without the reuse-codegen fix. The lane's real value is
**structural** (native reads inline like C, no dependence on the inliner) plus
the strict-predicate infrastructure that lets SOME binders go raw soundly, plus
**discovering and bounding the two real blockers** (non-total border → needs a
mode-slave pass; reuse-in-place raw-Int codegen gap).

## The `nemit_tagof` check (brief's 5-minute sizing question)

`nemit_tagof` goes **by shim** (`kaix_variant_tag_of`, no KaiValue struct type
registered), so the lane is the WIDER case: it introduces a KaiValue-header
struct type (`{i32,i8,i8,i16,[16 x i64]}`) to derive `&v->as` by field-index
(never a hardcoded `8`), then reads inline. asu confirmed the struct GEP is
sound: LLVM's DataLayout is deterministic over the same element types, field 4
lands at offset 8, and an `i64`/`ptr` load inherits ABI align 8 from the scalar
type. The runtime bitcode is always compiled without `-DKAI_TRACE_RC`, so the
header is always the vanilla 8-byte shape.

## Fixtures added

- `examples/perceus/native_static_slot_read.kai` (+ `.out.expected`): a hot
  binary-tree walk reading an Int slot (kind-1), a Real slot (kind-2), and
  borrowing pointer slots (kind-0), all in match-arms. Native output must be
  byte-identical to C and RC balanced (`test-native-static-slot-read`, gated in
  `tier1-native.yml`). Deliberately READ-ONLY — a rebuild into a reused Int slot
  hits the reuse codegen gap and is out of this fixture's scope.

## Coverage gaps / follow-ups

- **Reuse-in-place / TRMC raw-Int codegen** (#1110): the blocker for
  rb-tree's −5.7%. Fixing it lets the predicate permit kind-matched ctor slots.
- **Native mode-slave re-box pass**: the general fix that would let Route (b)
  match the C oracle's coverage instead of the strict-by-exclusion predicate.
  Until it exists, the predicate is the sound floor.

## Real cost vs estimate

Far over the brief's implied size. The brief framed Route (b) as "the same
`slot_kind_of`, invariant read-matches-write" — a mechanical follow-up. The
binary self-host gate proved it is a load-bearing architectural dependency
(non-total border). Most of the lane's time was the segfault bisection that
established: pointer read = sound + no-win; Route (b) = sound only under a
strict predicate = no rb-tree win; the rb-tree win needs a reuse-codegen fix
that is its own lane.
