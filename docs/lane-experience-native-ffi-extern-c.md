# Lane retro — native `extern "C" fn` shim + symbol separation

Branch: `native-ffi-extern-c`. Base: `256e1ac0` (main).

Closes the FFI cluster of the in-process libLLVM native backend (KIR Lane
1.5): the two `extern "C" fn` fixtures the ratchet listed as gaps —
`examples/effects/ffi_extern_c_basic.kai` and
`examples/effects/ffi_pub_axiom_cross_module/main.kai`. The native baseline
drops 5 → 3.

The C-direct oracle (`emit_c.kai`) and the LLVM-text backend (`emit_llvm.kai`)
both already lowered FFI correctly; the divergence was native-only, in two
disjoint places.

## Scope as planned vs as shipped

Planned (brief): make the in-process native backend emit a working call to an
`extern "C" fn` (direct + cross-module), at parity with C-direct.

Shipped: that, via **one** root cause in the KIR fn lowering and **one** in the
native symbol naming — the brief flagged only the first (it did not assume the
cause; it asked for the symptom to be captured first, which is what surfaced
the second).

### Bug 1 — the FFI body never reached a shim (the symptom in the brief)

An `extern "C" fn` lowers to a `DFn` whose body is the magic
`ETodo("__kai_ffi__"[:override])` tag. emit_c / emit_llvm intercept that DFn in
`emit_fn_body` and synthesise an unbox→extern-call→re-box shim from the params
+ ret type — they never lower the body expr. The native path consumed the body
through `lower_expr`, hit the `ETodo`, and lowered a panic stub: the binary
**compiled** and **crashed at run** with `panic: todo: __kai_ffi__`. (The
proto-dispatch dispatcher in the same `lower_fn` already had exactly this
intercept pattern, for exactly this reason — the FFI case was simply missing.)

Fix (asu Camino B): a body-less `KFnFfi(c_symbol, param_kinds, ret_kind)` KFn
kind. The frontend classifies + resolves the C symbol (the issue #261 override
carved out of the tag via `ffi_extern_symbol_of`); the native backend
synthesises the **whole** shim (`emit_native_ffi.kai`), the same asymmetry the
oracle has — FFI is an ABI frontier handled in the fn driver, not a language
expression. asu's GHC precedent: `foreign import ccall` is a backend-synth
wrapper around a `CCall` primitive with machine types, never a Core term; the
KIR is C-- and the machine-typed call lives below it, not in it. The shim:

1. unbox each boxed param to its raw C scalar (`kaix_int_field` /
   `kaix_real_field` / `kaix_bool_field` / `kaix_str_bytes` — the last two are
   new box→raw borrows in `runtime_llvm.c`);
2. call the bare extern symbol over the scalars (`ncall_finish`, which builds a
   fn-type with arbitrary LLVM types and declares the extern);
3. decref each boxed input **after** the call (RC order load-bearing: a String
   unboxes to a borrowed `char *` aliasing inside the cell — decref'ing before
   the extern finishes reading it is a UAF; the raw return has no RC, so the
   re-box mints the single owner with nothing to decref);
4. re-box the raw return.

`KFfiKind` is the KIR's own marshalling enum (translated from emit_shared's
`FfiRetKind` in the frontend) so `kir.kai` stays Lane-0 / emit_shared-free.

### Bug 2 — the wrapper symbol collided with the bare extern (the surprise)

Once the shim built, both fixtures still crashed — now SIGSEGV, not the panic.
The IR showed the wrapper for `extern "C" fn abs` emitted as
`define ptr @abs(ptr)` and, inside it, `call i64 @abs(i64 ...)` — resolving to
**itself** (the boxed wrapper), so the wrapper recursed infinitely.

Root: the native backend does **not** `kai_`-prefix user symbols (the C /
LLVM-text oracles always emit `kai_<c_sym>`, so their wrapper `kai_abs` and the
extern `abs` never clashed). The native emits module-mangled (`char__is_digit`)
or bare (`main`→`kai_main`) symbols; a root `extern "C" fn abs` came out as a
bare `@abs`, sharing the symbol with the libc extern it calls.

Fix (asu, mint-in-lowering): the FFI wrapper + every call site mint
`__ffi_<c_sym>`; the bare extern keeps its verbatim C symbol. `LowerSt.ffi`
carries the extern-name set so a call site mangles to the same wrapper the def
minted (bare via `c_sym_of_callee_st`, cross-module via the `EModCall` arm).

A **false-green caught mid-flight** (asu warning #1): the first attempt used the
prefix `kai_` (to mirror the oracle). The native `ncall_sym` rewrites any
`kai_`-prefixed callee to a `kaix_<rest>` runtime prim (`kai_op_field` →
`kaix_field`), so `kai_abs` mis-linked to the nonexistent `kaix_abs` — an
undefined-symbol **link** error, not recursion. `__ffi_` is neither a prelude
name nor `kai_`-prefixed, so it stays a literal user-fn symbol. The lesson: the
native naming scheme reserves `kai_` for prim forwarders; a wrapper prefix must
dodge it.

## Structural surprises the brief did not anticipate

- **byte-id is false-green for this whole lane.** `compiler.kai` (what selfhost
  re-compiles) contains no `extern "C" fn`, so the `KFnFfi` path is never
  exercised by selfhost and the auto-recursive IR is "valid" — byte-id passed
  green with the recursion bug live. The real gate is the two fixtures running
  native==C-direct + an ASAN run on the String path. (selfhost still earns its
  keep here: it caught the missing `import compiler.emit_native_ffi` in
  `emit_native_term.kai` that the bundle-concat build hid — a genuine module-
  privacy / import-resolution miss, orthogonal to the FFI logic.)

- **`LowerSt` grew a field.** Threading the FFI-name set meant `ffi: [String]`
  on `LowerSt` and a one-line edit at all 9 reconstruction sites + `ls_new`
  (the `consts` field set the precedent). Mechanical but wide; concentrated in
  `kir_lower.kai`, so the blast radius stayed in one file.

## Fixtures added

- `examples/effects/ffi_native_symbol_separation.kai` (+ `.out.expected`, `25`)
  — a single program exercising all three shapes a regression could hit: a
  direct-name extern (`abs`, wrapper `__ffi_abs` vs extern `abs`), a
  name-override extern (#261, `c_strlen` vs `strlen`), and both a String arg
  (borrowed `char *`) and an Int arg (scalar). Lives in `examples/effects/`
  (the parity harness walks it; `examples/ffi/` it does not), so native==C is
  CI-gated. The two original gap fixtures stay in the corpus too.

## Known orthogonal gap (documented, NOT fixed here)

The native backend can clash a **root** user fn with a libc symbol of the same
name even **without any FFI** — e.g. a user `fn time()` and libc `time`, or
`fn free()` and libc `free`. The oracles `kai_`-prefix every user symbol, so
they are immune; the native does not. This is a pre-existing native-mangling
concern, not introduced by this lane, and no corpus fixture exercises it. It
belongs in a separate `compiler` issue (a repro without FFI makes the
independence clear). Not opened here — `gh issue create` needs explicit
authorisation, and this is out of lane.

## Cost vs estimate

Two asu consults (one on the KFnFfi-vs-KOp representation, one on the symbol-
collision fix — both confirming the lower-blast-radius option with a soundness
caveat that turned out load-bearing). The second bug was not in the brief and
cost the most: it only appeared after the first fix built, and the first symbol
choice (`kai_`) was itself wrong for a native-specific reason. Capturing the
exact symptom before assuming the cause (the brief's instruction) is what made
both surface cleanly.

## Files

- `stage2/compiler/kir.kai` — `KFfiKind` enum + `KFnFfi` KFnKind constructor.
- `stage2/compiler/kir_lower_fns.kai` — `lower_fn` FFI intercept; `KFfiKind`
  classification; `ffi_extern_names`.
- `stage2/compiler/kir_lower.kai` — `LowerSt.ffi` field; `ffi_wrapper_sym`;
  call-site mangling in `c_sym_of_callee_st`.
- `stage2/compiler/kir_lower_walk.kai` — cross-module FFI call-site mangling.
- `stage2/compiler/kir_dump.kai` — `KFnFfi` print arm.
- `stage2/compiler/emit_native_ffi.kai` — NEW, the shim synthesis (km A+,
  147 LOC, cogcom avg 1.4 / max 3).
- `stage2/compiler/emit_native.kai` / `emit_native_term.kai` — `KFnFfi` in the
  declare / emit / thunk passes + the new import.
- `stage0/runtime_llvm.c` — `kaix_int_field` / `kaix_str_bytes` borrows.
- `stage2/Makefile` — `emit_native_ffi.kai` in `BUNDLE_SRCS`.
- `tools/native-parity-baseline.txt` — 2 gaps removed (5 → 3) + the burn-down
  note.
