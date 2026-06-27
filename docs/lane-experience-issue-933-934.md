# Lane experience — FFI scalar forward-decl (#933) + `Char` boundary (#934)

Two FFI boundary-type gaps in the same area (FFI lowering in `emit_c.kai`
/ `emit_ffi_c.kai` / `emit_native_ffi.kai`), shipped together.

## Scope as planned vs as shipped

Planned: emit a forward declaration for non-libc scalar `extern "C"`
symbols on the C backend (#933), and either implement or honestly reject
the documented `Char → int32_t` boundary type (#934).

Shipped: both, plus a fixture migration the brief did not anticipate. The
forward-decl fix (#933) broke every fixture that bound a libc symbol with
a *mistyped* kaikai signature — there were five, not the one the brief
named — so the lane also migrated them to honest C-exact widths. `Char`
(#934) was implemented in full (option A) on both backends.

## #933 — the forward declaration and what it forces

The v1 scalar shim deliberately minted no `extern` declaration. Its
comment gave a real reason: a local `extern int64_t puts(const char*);`
clashes with `<stdio.h>`'s `int puts(const char*)` (a redeclaration with
an incompatible return type is a hard C error). So the v1 shim relied on
the system header already declaring the symbol — which only works for
libc symbols, and silently mistyped them (`puts(s: String) : Int` maps
`Int`→`int64_t`, but libc's `puts` returns `int`; it "worked" only
because `<stdio.h>` declared it and `int`→`int64_t` return promotion is
benign).

The design call (validated with the language architect): the emitted
forward declaration **is the binding contract**, exactly like Rust/Zig
`extern "C"`. The compiler emits `extern <ret> sym(<params>);` over the
call site's C ABI types on every scalar extern (matching what the v2
shim already does). A user binding a libc symbol directly must declare a
kaikai type whose C mapping matches the system header, or `cc` rejects
the redeclaration *at compile time* — which is the audit Tier 1 wants,
not a regression. A symbol whose C type has no exact kaikai mapping
(`size_t`, a struct return) is wrapped in a small `.c`.

This drew the line cleanly: `int`-returning libc (`abs`, `puts`, `atoi`)
binds directly as `I32` (libc's `int`, which is `int32_t` on every
supported ABI — SysV AMD64 / AArch64 are LP64 with 32-bit `int`).
`strlen` (returns `size_t`, no kaikai mapping) cannot be bound directly
and must be wrapped — so the name-override and symbol-separation fixtures
that used `strlen` moved to `atoi`, preserving the exact property each
tested (override decoupling; two distinct externs coexisting).

### Migration, not sidecar-for-everything

The tempting uniform move — wrap every libc binding in a user `.c` — was
rejected: the backend-parity harness builds each fixture with `kai build
f -o bin` and has **no sidecar support**, so wrapping the
`examples/effects/ffi_*` fixtures would force new parity skips (silent
coverage loss). The honest binding `abs(n: I32) : I32` needs no sidecar
and stays parity-tested, so only `strlen`'s lack of a kaikai type drove a
change of vehicle, not the category "binds libc".

## #934 — `Char` is just an int32 at the ABI

A `Char` is a Unicode codepoint carried as `uint32_t` in the box
(`KAI_CHAR`, `as.c`). At the boundary it is an `int32_t`, re-boxed
through `kai_char` (C) / `kaix_char` (native). Every runtime piece
already existed (`kai_char`, `kaix_char`, `kaix_to_char` reading
`as.c`), so option A was a small, sound addition: one `FfiRetChar`
variant in `FfiRetKind`, one `KFfiChar` in the KIR, and the marshalling
arm in each backend's shim. The typer already accepted `Char` in an
extern signature (it resolves to `TyChar`, and `is_c_abi_safe_type`
admitted it via the uppercase-name fallthrough), so no typer or resolver
change was needed beyond making `Char` an *explicit* entry in the C-ABI
allowlist and its error messages.

`Char` is **not** a valid struct field (a record field needs an exact
width; `is_c_field_type` rejects it), so the struct-field rebox paths
keep their wildcard fallback — `Char` never reaches them.

## Structural surprises the brief did not anticipate

- **Five mistyped-libc fixtures, not one.** The brief named the lone
  `puts`/`abs` fixture; the forward-decl change also broke
  `examples/ffi/extern_c_basic`, `extern_c_name_override`,
  `ffi_native_symbol_separation`, and `ffi_pub_axiom_cross_module`.
- **The parity harness has no sidecar path**, which is what made
  "migrate everything to wrappers" the wrong call and "declare the
  C-exact width" the right one.
- **The v1 shim comment was already lying** about forward declarations
  ("forward-declares the extern symbol") — the duplicate header comment
  block described behaviour the code did not have. Collapsed to one
  truthful block.

## Fixtures added + coverage

- `examples/ffi/extern_c_user_symbol.kai` (+ `.out.expected`) — the #933
  case the existing suite never had: a non-libc *user* symbol whose only
  declaration is the compiler-emitted forward decl.
- `examples/ffi/extern_c_char.kai` (+ `.out.expected`) — the #934 case,
  backend-independent golden (prints the returned codepoint).
- `examples/ffi/scalar_shim.c` — user-symbol shim for both.
- `test-ffi-scalar-char` Makefile target runs both on the C backend +
  ASAN, compiling with `-Werror=implicit-function-declaration` so a
  regression of #933 is a hard failure (it cannot hide behind the
  undocumented `-Wno-implicit-function-declaration`). Native is covered
  by the parity ratchet (and verified manually).

Coverage gap left: no fixture goldens the *redeclaration error* a
mistyped libc binding now produces — that error comes from `cc`, not the
kaikai compiler, and the negative-FFI harness only goldens compiler
diagnostics. The contract is documented in `docs/ffi.md` / `kai info ffi`
instead.

## How no other FFI shape regressed

All FFI Makefile targets stay green (`test-ffi-extern-c`,
`test-issue-260-261-extern-c-fn`, `test-ffi-pub-axiom-cross-module`,
`test-ffi-extern-c-asan`, `test-ffi-v2-struct/opaque/negatives`), serial
native-vs-C parity over `examples/effects` is `fail=0`, and selfhost is
byte-identical. Fixed-width / struct / opaque marshalling is untouched —
the `Char` arms slot beside the existing scalar arms, and the forward
decl reuses the same `ffi_c_ret_type_str` / `ffi_c_param_list` helpers
the v2 shim already used.

## Follow-ups for next lanes

- Native struct-FFI (#923) remains the only FFI shape not on the native
  backend — out of scope here.
- The `#error` message and a few helpers still carry a stale `m12.7.x`
  milestone tag (pre-existing); a doc-hygiene pass could drop it.
