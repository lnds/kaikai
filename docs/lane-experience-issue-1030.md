# Lane experience — issue #1030: real struct-by-value FFI on the native backend

## Scope as planned vs as shipped

**Planned.** Replace the honest struct-by-value reject on the native
(libLLVM) backend with real C-ABI classification done in the emitter, so a
struct-FFI program builds with `--emit=native` and links against a
clang-compiled callee that receives the values correctly. The intermediate
milestone the issue authorised was arm64-Darwin-only.

**Shipped.** BOTH primary targets: arm64-Darwin (AAPCS64) and x86-64 SysV.
The classification for SysV turned out to be *less* work than feared once
the AAPCS64 path existed — the two share the layout/flatten machinery and
only differ in the register-class decision (HFA-vs-eightbytes). Both
struct fixtures (`v2_struct_by_value`, `v2_struct_adv`) pass on native with
output identical to a clang-compiled callee; the reject is kept only for a
target the classifier does not model (`PlatNone`).

## Design decisions

1. **Classification is a separate, pure module** (`emit_native_abi.kai`).
   It takes `[KFfiField]` and returns `AbiPass` descriptors (coerce slots /
   indirect / sret) with NO LLVM handles — so it sits entirely outside the
   Perceus handle-exclusion regime and scores A+ (cogcom max 4). The
   emitter (`emit_native_struct.kai`, `emit_native_ffi.kai`) turns those
   descriptors into IR. This split is why the ABI logic is testable by
   reading and why the #923 hazard could not leak into it.

2. **The oracle is clang, not the C backend.** The C backend hands struct
   layout to the C compiler, so it never *names* the coercion — there was
   no coercion decision to mirror. Instead the classification was pinned
   against `clang -emit-llvm` for every fixture shape on both triples
   (captured up front), and the emitter reproduces that exact lowering
   (Color→i64/i32, Vector2→[2 x float], Big>16→sret/byval, Rect nested HFA→
   [4 x float]). The C-backend fixture output remains the behavioural
   oracle for the run; clang IR is the ABI oracle for the classification.

3. **Two new prims only.** `native_target_abi` (reads
   `LLVMGetDefaultTargetTriple` → platform code) and `add_sret_decl/call`
   (the `sret` attribute, mirror of the existing byval). Everything else —
   struct types, struct-GEP, byval, trunc/sext/fpcast, array types — was
   already in the prim table from the reverted WIP (`ed32bb35`).

## Structural surprises the brief did not anticipate

- **`nemit_null` is not a null handle.** `nemit_null` returns an LLVM
  `ConstantPointerNull` VALUE — a non-NULL C handle — so it could not
  signal "no sret" to `llvm_handle_is_null`. First IR emit prepended a
  spurious `ptr null` arg to EVERY extern (even scalar-returning ones).
  Fix: `ffi_null_handle` probes a never-defined global, which returns a
  genuine NULL C handle. This distinction (LLVM null value vs NULL handle)
  is a trap worth remembering for any future sentinel-by-handle code.

- **Field loads must be at the exact C width.** Re-boxing a `Color` return
  read each field with a `load i64` at the field's struct-GEP address —
  reading 8 bytes from a 1-byte `u8` field, so `.r` came back as
  `0xFF404040` (the whole packed i32). Fix: load `iN` at the declared
  width, then sext/zext. The store path already truncated correctly; only
  the reload was wrong.

- **Nested structs are stored INLINE, not by pointer.** The first cut
  treated a `KFfiStruct` field like a scalar and stored a *pointer* into
  the parent's slot; `Rect { Point tl, br }` came back all-zero. Fix:
  `nstruct_store_field` GEP-addresses the sub-struct within the parent and
  recurses the fill in place. Symmetric on re-box (the sub-struct's GEP IS
  its storage).

- **`else match` is not valid kaikai.** `else` wants `if` or `{`. A bare
  `else match ...` failed the bundle parse; wrap the match in `{ }`. (Cost
  one build cycle; `kai info syntax` confirms.)

- **The bundle hides privacy; selfhost catches it.** `nabi_coerce_type`
  was private but called across modules — the bundle-concat kaic2 compiled
  fine, `make selfhost` failed with the privacy error. Standard
  `stage2_bundle_vs_imports_gap` trap; marking it `pub` closed it.

## The #923 hazard

The reverted WIP crashed the *compiler* (a dropped LLVM `Type*` stomped
its context field). Every classifier/marshalling helper that returns a
Type*/Value* Handle is listed in stage1's `native_handle_fns` (kaic1's
handle-binder exclusion), and every inline `f(g_returning_handle(...))`
was rewritten to capture the handle in a `let` first. Result: kaic2
compiles both struct fixtures AND self-hosts byte-identically with no
crash — the hazard did not return.

ASan verification note: a native binary linked against `runtime_llvm.c`
hangs under `-fsanitize=address -O2` REGARDLESS of FFI (a plain hello-world
hangs identically), so ASan-on-the-user-binary is not a usable gate here —
a pre-existing runtime/ASan interaction, not a struct-FFI defect. The
#923 hazard is a *compiler* crash, and the compiler self-hosts clean.

## Fixtures + coverage

- `v2_struct_by_value.kai` / `v2_struct_adv.kai` — now run on native via
  the new `test-ffi-v2-native-struct` target, linked against
  `v2_struct_native_shim.c` / `v2_struct_adv_shim.c` (clang standalone).
- `v2_struct_native_reject.err.expected` — deleted; the reject no longer
  fires for the supported shapes on the covered platforms.
- Gap: no fixture exercises the `PlatNone` reject path (cannot be produced
  on a supported dev triple). The negative `v2_struct_field_non_c` still
  covers the typer-level rejection of a non-C-width field.

## Cost vs estimate

The plumbing was mostly present; the real work was the classification
layer plus three subtle marshalling bugs (sret sentinel, field width,
nested inline), each caught by running against the clang callee rather
than the C backend. Running end-to-end against clang — not native-vs-C —
was load-bearing: the C-backend fixture would have passed with the
field-width bug because both sides shared the same wrong reload.

## Follow-ups for next lanes

- Windows x64 struct ABI (a different classification) stays on the reject.
- The `runtime_llvm.c` + ASan hang is worth its own issue if native ASan
  coverage is ever wanted; it blocks memory-checking any native binary.
