# Lane experience — issue #417 (FFI v2: struct-by-value, opaque handles, fixed-width)

## Scope as planned vs as shipped

Planned (issue + brief): three pieces — fixed-width FFI-boundary types,
struct-by-value (incl. return-by-value), opaque handles — on BOTH
backends (native default + C-direct oracle), with the two hard problems
Linus flagged (KaiValue unwrap at the boundary, Perceus ownership for
opaque handles) actually solved.

Shipped:
- **Fixed-width** (`U8 U16 U32 U64 I8 I16 I32 I64 F32`), **struct-by-value
  + return-by-value**, **opaque handles** — all fully on the **C-direct
  backend**, compiling AND running against real C shims, ASAN-clean.
- **Native backend**: **opaque handles fully supported** (the libpq /
  DB-driver case). **Struct-by-value + standalone fixed-width route
  through `--backend=c`** — the native libLLVM backend lacks the C-API
  surface (struct types, extractvalue, integer-width truncation) to
  marshal them, so a native build reports the gap and the `--backend=c`
  fix. This is the one deliberate scope cut; see *Follow-ups*.

The "OLD issue → defer to v1.1" verdict in the issue comments was
treated as problem context, not a live decision: every factual claim
was re-verified against current main (post #869/#870), and the feature
shipped in full on the oracle backend.

## Surface spelling chosen + alternatives

- `extern "C" type Name = { f: U8, ... }` (struct) and
  `extern "C" opaque Name` (handle), reusing the existing `extern "C"`
  head. `opaque` is a **contextual keyword** (recognised only after
  `extern "C"`), NOT a lexer token — adding `TkOpaque` would have
  risked the stage1 selfhost byte-id fixed point for zero benefit.
- Fixed-width are bare type names in extern param/return slots.
- Alternative considered + rejected: a new `TypeBody::TBExternStruct`
  arm. `TypeBody` has 132 match sites across 13 files; a new arm there
  is large blast radius. Chosen instead: a new single `Decl` arm
  `DExternType(name, ExternTypeBody, l, c)` (struct + opaque in one
  arm, ~14 exhaustive-Decl-match edits, the standard additive cost a
  prior arm like `DConst` paid), with `TypeBody` untouched.

## Representation decisions (each settled by a spike, not a guess)

- **Fixed-width = kaikai-side `Int`/`Real`.** The typer resolves
  `U8`/`I32`/… → `TyInt`, `F32` → `TyReal` (all three `resolve_ty*`
  sites), so `make_gray(64)` and `Color { r: 255 }` type-check. The FFI
  classifier reads the SURFACE name for the C cast. Verified: without
  this the typer rejects `make_gray(64)` ("expected (I32), found (Int)").
- **Struct = ordinary structural record.** Spike showed the typer
  accepts `Color { r: 255 }` + `c.r` on an UNDECLARED record name with
  no `DType` — so an extern struct needs NO typer registration; it
  rides the existing structural-record machinery. Not lowered.
- **Opaque = `KAI_FOREIGN` box + 1-field-record lowering.** A new
  runtime tag `KAI_FOREIGN` (added to BOTH runtime.h copies + the
  native `runtime_llvm.c` as `kaix_foreign`/`kaix_foreign_ptr`) parks a
  `void *`; its free is a no-op on the payload (only the box is
  reclaimed). The opaque type is lowered (at desugar) to a
  ONE-`__ffi_handle`-field nominal record so Perceus RC-tracks it. The
  empty-record (0-field) lowering was tried first and does NOT work —
  Perceus treats a 0-field type as non-heap and emits no drop. The
  1-field record is a pure typer/RC fiction (never matched / never
  built); the box ignores the field at runtime.

## KaiValue unwrapping — how it was emitted on each backend

- **C-direct** (`compiler/emit_ffi_c.kai`, a new A-grade file): per
  param, read each record field with `kai_op_field_borrow` and unbox at
  its C width into a local `struct __kai_ffi_<Name>` literal; call by
  value; re-box a returned struct field-by-field into a fresh
  `kai_record`. Opaque unboxes via `kai_foreign_ptr`, re-boxes via
  `kai_foreign`. The struct typedef (`struct __kai_ffi_<Name> { ... };`
  — plain tag, no typedef alias) is injected into both program
  emitters' type header.
- **Native** (`emit_native_ffi.kai`): the v1 shim was already symmetric
  via a KIR `KFnFfi(c_symbol, [KFfiKind], KFfiKind)`. Extended
  `KFfiKind` with `KFfiForeign` (rides the existing native pointer
  path); struct + fixed-width classify as `KFfiUnknown` so the existing
  `nemit_unsupported` aborts loud.

The single shared classifier `ffi_kind_of_ty` (emit_shared) gained a
registry-aware sibling `ffi_kind_of_ty_reg` in a new
`compiler/ffi_v2.kai` (the extern-type registry + the v2 `FfiRetKind`
arms). Both backends consume it; the registry is threaded via an
`EmitCtx.ffi_reg` field (C) and a `lower_fn` param (native).

## The Perceus ownership rule (the load-bearing fix)

Opaque-handle multi-use was a **latent UAF / double-free**, not a
leak — the framing that justified solving it completely now rather than
deferring. An `extern "C" fn`'s body is the magic FFI `ETodo` tag, so
Perceus's borrow-inference marked all params BORROWED and its
consume-inference marked NONE consuming, while the emitted shim DECREFS
each boxed input (callee-owns). Single-use masked it (the shim decref
cancelled the mint by luck); a handle passed to two calls had the first
shim free it and the second read freed memory. Fix (gated on
`body_is_ffi_extern`): `pcs_borrow_entry_of` registers NO borrowed
positions for an FFI shim, and `pcs_consume_entry_of` marks every boxed
param consuming (skip Unit). Perceus then dups a shared handle before
each non-final call and moves on the last — balanced, ASAN-clean. This
also fixes the same latent bug for a v1 `String` used in two extern
calls. (asu review confirmed the diagnosis + the exact fix sites.)

Ownership rule shipped: **no Drop integration.** kaikai RC frees the
handle box at its last use; the parked `void *` is never touched; the
driver calls the C destructor. Returned strings keep copy semantics
(`kai_str`), so handle-borrowed `char *` never dangles. The richer
affine/linear handle was NOT built (not needed for correctness given
copy + no-Drop; noted as a follow-up).

## Struct ABI (register vs memory)

Deferred to the C compiler. kaikai emits `struct __kai_ffi_<Name>` with
the exact C field widths and passes it by value; the C compiler does
the SysV / AAPCS register-vs-memory classification. Verified with
`Color` (4×u8, register-class) and `Vector2` (2×float) round-trips +
struct-in / struct-out (`vec_add`).

## Selfhost-additivity surprises

- No `TypeBody` change → the 132 `TypeBody` match sites stayed
  byte-identical.
- The new `Decl`/`Ty`/`KFfiKind`/`FfiRetKind` arms are dead on the
  existing corpus (no program produces a `DExternType` etc.), so
  selfhost stays a fixed point — the standard additive discipline.
- Surprise: `kai info ffi` needed NO compiler change — the topic system
  is file-driven (`docs/info/<topic>.md`), so the new page just works.
- Surprise: the native object lands next to the source on
  `--emit=native` (not under a tmp dir as the C path does); the fixture
  harness compensates.

## Fixtures added + coverage gaps

- Positive: `examples/ffi/v2_struct_by_value.kai` (pass + return-by-value
  + struct-in/out + fixed-width fields/param) and
  `examples/ffi/v2_opaque_handle.kai` (3-consuming-call handle — pins
  the ownership rule), with `examples/ffi/v2_shim.c`. Both run + ASAN on
  the C backend; the opaque fixture also runs on native.
- Negative: `examples/negative/ffi/v2_struct_field_non_c.kai` (a struct
  field with no exact C width) and `v2_callback_param.kai` (a
  function-typed extern param).
- Makefile: `test-ffi-v2-struct`, `test-ffi-v2-opaque` (both C + ASAN),
  `test-ffi-v2-negatives`, wired into `.PHONY` / `TEST_LIGHT_TARGETS` /
  `test-fast`.
- Gap: no native-backend AUTOMATED fixture for opaque (it works,
  verified manually; the native ratchet's harness links no extra `.c`,
  so a shim-linked native fixture needs a small harness extension —
  left as a follow-up). No standalone fixed-width-only fixture (it is
  exercised inside the struct fixture's fields + the `I32` param).

## Real cost vs estimate

No time estimates (per repo discipline). The dominant cost was NOT the
three features (each was mechanical once the representation was fixed)
but the **Perceus ownership interaction** — three wrong turns
(empty-record lowering, borrow-shim, decref-shim-without-the-map-fix)
before the consume/borrow-map fix landed. asu's two consults (surface +
ownership) each saved a thrash cycle.

## Follow-ups for next lanes

- **Native struct-by-value + fixed-width.** Add the libLLVM C-API prims
  (struct type, GEP/extractvalue/insertvalue, trunc/zext/sext/fptrunc/
  fpext, intN types) + runtime forwarders, then emit the struct
  marshalling in `emit_native_ffi.kai`. Bounded (~10 prims × 3 sites)
  but with real ABI-classification risk; the C oracle is the parity
  reference.
- **Automated native opaque fixture** — extend the native test harness
  to link an extra `.c` shim.
- **Affine/linear opaque handles** — auto-drop via the C destructor
  (richer than no-Drop). Optional; no-Drop is correct + shippable.
- **`Borrowed[char*, from: handle]`** zero-copy returned strings — needs
  lifetime tracking kaikai deliberately avoids; copy is correct for now.
- **bindgen** (`kai bindgen foo.h`) — out of scope, tracked separately.
