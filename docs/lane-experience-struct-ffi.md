# Lane retro — struct-by-value FFI on both backends (#923, #924)

Branch: `struct-ffi`. Base: `323dc2e9` (main).

Two related struct-by-value FFI gaps:

- **#923 (native):** a native build of an `extern "C" type` struct program
  SIGSEGV'd the compiler instead of degrading. `kai info ffi` promised
  native "reports the gap and the `--backend=c` fix"; it crashed.
- **#924 (C):** the `extern "C" type` struct lowered to a file-local
  `struct __kai_ffi_T` in `out.c`, invisible to a hand-written shim — so
  (the issue claimed) struct-by-value was uncallable.

## Scope as planned vs as shipped

Planned (brief): fix struct-by-value FFI on both backends — either real
native marshalling or an honest native reject (#923), and a shim that can
name the struct type (#924). Decisions A-or-B left to the lane, justified
here.

Shipped:

- **#923 = reject honestly (B).** The crash had two layers; both are fixed,
  and the native struct path now rejects with an actionable diagnostic
  rather than silently corrupting data.
- **#924 = shared header (A).** The `extern "C" type` structs ship in a
  generated `kai_ffi.h` that both `out.c` and a hand-written shim
  `#include`, naming one C-compiler-checked type.

## #923 — the native SIGSEGV had two distinct layers

The native backend already carried WIP struct marshalling (`KFfiStruct`,
`ffi_unbox_struct`, `byval` attributes). It crashed; the commit that added
it said so ("struct WIP … still crashes the native emitter"). Diagnosis
peeled two layers:

**Layer 1 — Perceus dup stomps a bare LLVM `Type*`.** `ffi_unbox_struct`
bound `let sty = ffi_struct_llvm_type(...)` and used it twice (alloca +
GEP-store). `ffi_struct_llvm_type` is a USER fn returning `Handle`, so
Perceus treats a multi-use binding of its result as owned and emits
`kai_internal_dup(sty)` + a trailing drop. `kai_incref` on a raw
`LLVMTypeRef` does `v->rc++`, which lands on the `Type` object's context
field — the AsmPrinter then SIGSEGVs in `PointerType::get` reading a
context whose low bit got set. The borrow-vs-dup asymmetry is the lever:
a **raw-prim** Handle (`llvm_array_type`, registered `TyHandle`) is
borrowed and survives a multi-use `let` (this is why the structurally
identical `nemit_ptr_array` never crashed), while a user-fn Handle is
dup'd. This is the same "handle-binder exclusion" trap the rebox path
already documents.

**Layer 2 — `byval` is the wrong ABI.** Fixing layer 1 stopped the crash
but `take_one(One{v:42})` returned 0. clang lowers a small struct to a
COERCED register signature (`take_one(i64)`, `[2 x float]`, `sret`) per
SysV / AAPCS / Darwin; `byval` only means "pass on the stack by value" and
LLVM does NOT reclassify it into registers. That classification is
frontend work (clang's `ABIInfo`) and the native backend IS the frontend,
so it would have to replicate it per target — a correctness minefield
where a wrong class silently corrupts boundary data. (struct-RETURN
"worked by luck" for `{i8,i8,i8,i8}` because 4 bytes in x0 coincide with
i32 in x0; it would break for `Vector2` as an HFA in v0/v1.)

**Decision: (B) reject.** A half-working `byval` path that silently
corrupts data fails Tier 1 "safe at compile time" — strictly worse than a
crash because the program keeps running with garbage. There is no LLVM
shortcut for the classification. Native struct ABI is a separate lane with
a fundamentally different correctness gate (per-ABI-class byte goldens, not
an `.err.expected`). So: keep the layer-1 fix as the bug-class fix it is,
delete the unsound `byval`/struct-marshalling, and gate struct kinds at the
shim with an actionable diagnostic naming the function + `--backend=c`.
`emit_native_ffi.kai` shrinks (the WIP marshalling goes); scalars,
fixed-width and opaque still marshal natively.

## #924 — the C blocker was already gone; the contract was the gap

Reproducing #924 on current main: the natural shim spelling (a shim with
its own `typedef struct {...} Color;`) ALREADY compiled, linked, and ran
correctly on `--backend=c`. A recent fix (#933, emit the extern
forward-decl for non-libc symbols) incidentally unblocked it — the linker
matches only the symbol, and the two TUs' structs are layout-identical, so
the by-value ABI matched at the machine level.

But that is exactly the silent-corruption bug-class #923 rejected on
native, just on the C side: the layout agreement is implicit, the shim
author declares the field widths by hand, and nothing detects a mismatch
(C has no cross-TU ODR for types). Documenting "declare a struct with these
widths and trust the layout" would leave a landmine.

**Decision: (A) shared header.** Emit the `extern "C" type` structs as
`typedef struct {...} <Name>;` (bare declared name) into a generated
`kai_ffi.h`. `out.c` `#include`s it (and references the bare name
everywhere — `extern_struct_c_tag` now returns the name, not the
`struct __kai_ffi_<Name>` tag), and a hand-written shim `#include`s the
SAME header. One authoritative definition: a shim that redeclares the type
with a wrong layout is a C compile error, not boundary corruption
(verified). The collision the `__kai_ffi_` prefix guarded against does not
materialise — kaikai's own records compile to `struct kai_<mod>__<Name>`,
never a bare `Color`.

Mechanism: a struct program emits a `//KAIFILE`-marker stream (the same
scheme the modular path uses) carrying `kai_ffi.h` + `out.c`; the wrapper
splits it into a subdir and `-I`s it so both `out.c`'s `#include` and the
shim resolve the header. A program with NO extern struct emits bare `out.c`
with no marker — byte-identical to before, so selfhost is unaffected (the
compiler bundle declares no `extern "C" type`).

Trap hit: the first wrapper split wrote the split `out.c` back into `$tmp`
while awk was still reading `$tmp/out.c` as input — self-overwrite
truncation (`unknown type name 'E'` mid-line). Fixed by splitting into a
subdir.

## How no other FFI shape regressed

Scalars, name-override, fixed-width, and opaque verified on BOTH backends.
On C: the full struct fixture (struct-in, return-by-value, struct-in/out)
runs `127 64 4 6` with the header shim; ASAN clean. The `extern_struct_c_tag`
change flows through the C shim emitter uniformly (param/return/local/
extern-decl all use it), so the bare name is consistent within a TU.

Pre-existing bug surfaced, NOT fixed here (out of lane): nested-struct
fields (`Rect { tl: Point, br: Point }`) were never marshalled — both the
field C type and the unbox read fall through to `/* unsupported */`. The
orphan `examples/ffi/v2_struct_adv.kai` (not wired into any test target)
exercises it and prints `0` for the nested case on pristine main too. Filed
separately; the regression fixture here stays on flat structs.

## How the #937 collision was avoided

The parallel #937 lane rewrites the `lower_fns` accumulator in
`kir_lower_fns.kai`. This lane touched only the FFI files (`emit_c.kai`,
`emit_ffi_c.kai`, `emit_native_ffi.kai`, `ffi_v2.kai`, `bin/kai`,
`stage2/Makefile`) and `kir.kai` not at all — zero overlap with
`kir_lower_fns.kai`. The native struct work lives entirely in
`emit_native_ffi.kai`, never in the KIR lowering.

## Fixtures

- `examples/ffi/v2_struct_shim.c` — the documented `#include "kai_ffi.h"`
  struct shim (#924), wired into `test-ffi-v2-struct` (C + ASAN).
- `examples/ffi/v2_struct_native_reject.err.expected` — the native reject
  golden (#923), wired into `test-ffi-v2-native-reject` (gated on
  `KAI_LLVM`; SKIP on a C-only build).
- `test-ffi-v2-struct` rewired to split the marker stream and link the
  header shim.

## Follow-ups

- Native struct-by-value ABI classification (the real #923 (A)) — its own
  lane: SysV / AAPCS / Darwin per-class byte goldens.
- Nested `extern "C" type` struct fields on the C backend (the
  `v2_struct_adv` gap).
- Modular C path (`KAI_MODULAR=1`) shim-header sharing — opt-in, not
  regressed, but does not yet expose `kai_ffi.h` to a shim.
