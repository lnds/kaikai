# FFI — Foreign Function Interface

kaikai bridges to C through two distinct mechanisms:

1. **Prelude primitives** — compiler-internal C bindings used by
   the standard library (libm, filesystem, process, time, …).
   Wired in `stage0/runtime.h`, registered in the resolver as
   globally-available identifiers. Users do not see them; the
   compiler-dev does. This is how `real_sqrt`, `read_file`,
   `int_to_string`, etc. reach C code.

2. **`extern "C"` declarations** (FFI v1) — user-facing FFI.
   How a kaikai program binds to an external C library it
   doesn't own (raylib, sqlite3, libcurl, your own .so).
   Carries the `Ffi` effect capability.

This document covers both paths: the prelude primitives mechanism
for compiler-dev, and the `extern "C"` user-facing surface
including its current limits, the canonical workaround (C
shims), and the planned scope for FFI v2.

## Path 1 — Prelude primitives (compiler-internal)

Used by every stdlib module that needs to call C: `stdlib/math/real.kai`
(libm), `stdlib/fs/file.kai` (POSIX file ops), `stdlib/os/process.kai`
(fork/exec/wait), `stdlib/time.kai` (clock_gettime), and so on.
Users never write `extern "C"` for these — the names are
**globally available** because the resolver pre-loads them as
prelude-level identifiers.

### Mechanism

Three pieces wire a prelude primitive end-to-end:

1. **Runtime function** in `stage0/runtime.h`:

   ```c
   static KaiValue *kai_prelude_real_sqrt(KaiValue *x) {
     return kai_real(sqrt(kai_real_get(x)));
   }
   ```

2. **Thunk** registered in `stage0/runtime.h`'s prelude table so
   the call site can dispatch:

   ```c
   static KaiValue *_kai_prelude_real_sqrt_thunk(KaiValue *s, KaiValue **a, int n) {
     (void) s; (void) n;
     return kai_prelude_real_sqrt(a[0]);
   }
   ```

3. **Compiler entry** in `stage2/compiler.kai` registering the
   identifier in the prelude environment with its `Ty` schema:

   ```kai
   TyEntry { name: "real_sqrt", scheme: mono(fn_ty([TyReal], TyReal)) }
   EP("real_sqrt", "kai_prelude_real_sqrt", 1)
   ```

   Three locations to keep in sync: the prelude name list, the
   type-environment entry (so the typer knows the schema), and
   the EP (Emit-Prelude) record (so the C emit dispatches to the
   right thunk).

### libm bindings macro

For trig / log / pow families, the runtime uses macros to
collapse the boilerplate:

```c
KAI_LIBM_REAL1(sqrt, sqrt)
KAI_LIBM_REAL1(sin,  sin)
KAI_LIBM_REAL2(pow,  pow)
KAI_LIBM_REAL2(rem,  fmod)
```

Each `KAI_LIBM_REAL1(name, c_fn)` expands into a thunk plus a
prelude-table entry. Adding a new libm binding is a 3-line
change in `stage0/runtime.h` plus the compiler-side prelude
registration.

### When to add a prelude primitive

Add one when:

- The function is **standard / portable** (libc, libm, POSIX) and
  belongs in stdlib.
- Stdlib needs it directly and the alternative is the user-facing
  `extern "C"` (which would force the user to declare it
  themselves — wrong for stdlib).
- The cost is a single C function with primitive args and return.

Do **not** add a prelude primitive when:

- It's a third-party library (raylib, sqlite3) — that's
  user-facing FFI v1, the user declares the `extern "C"`.
- The signature uses structs by value or out-parameters — neither
  mechanism supports that today (see FFI v2 below).

### Examples in stdlib

| stdlib module | Prelude primitives it uses |
| --- | --- |
| `stdlib/math/real.kai` | `real_sqrt`, `real_sin`, `real_pow`, `real_rem`, … (libm) |
| `stdlib/math/int.kai` | `int_to_string`, arithmetic builtins |
| `stdlib/fs/file.kai` | `kai_prelude_read_file`, `kai_prelude_write_file`, `kai_prelude_file_exists`, `kai_prelude_file_delete`, `kai_prelude_file_rename`, `kai_prelude_file_append` |
| `stdlib/os/process.kai` | `kai_default_process_start`, `kai_default_process_wait`, `kai_default_process_kill`, `kai_default_process_exit` |
| `stdlib/encoding/json.kai` | `string_to_real`, `string_to_int` |
| `stdlib/time.kai` | clock primitives |
| `stdlib/crypto/hash.kai` | hashing primitives |

This path is **not** documented as a stable surface for users.
The names are pre-bound for stdlib's convenience; if you write a
program that calls `real_sqrt(2.0)` directly, it works (the
identifier is in scope), but the contract is the stdlib's, not
the language's.

## Path 2 — `extern "C"` (FFI v1, user-facing)

How user code binds to an external C library it doesn't own.

### Quick reference

```kai
extern "C" fn libm_sqrt(x: Real) : Real / Ffi
extern "C"("printf") pub fn c_printf(fmt: String) : Int / Ffi

fn main() : Unit / Ffi + Stdout = {
  let r = libm_sqrt(2.0)
  Stdout.print("sqrt(2) = #{real_to_string(r)}")
}
```

Three things are happening:

1. `extern "C" fn name(args) : T / Ffi` declares an external
   symbol. The compiler emits a forward declaration over the C ABI
   types the boundary uses (`extern <ret> name(<params>);`) on both
   backends, and the linker resolves `name` at link time. The
   declaration IS the binding contract: when you bind a symbol a
   system header already declares (libc), the kaikai-side type must
   map to the C type the header uses, or `cc` rejects the
   redeclaration. So `abs` binds as `abs(n: I32) : I32` (libc's
   `int`), not `: Int` (`int64_t`). A symbol whose C type has no
   exact kaikai mapping — `size_t`, a struct return — must be wrapped
   in a small `.c` whose signature uses a width kaikai can express
   (see *The C shim pattern*).
2. `extern "C"("symbol") pub fn name(args) : T / Ffi` overrides
   the C symbol when the kaikai-side identifier doesn't match
   (e.g. wrapping `printf` as `c_printf` to avoid kai-side
   keyword conflicts; issues #260 / #261).
3. The `Ffi` effect carries the capability. Any function that
   directly or transitively calls an `extern "C"` declaration
   has `Ffi` in its row. The handler is compiler-synthesised —
   `Ffi` operations lower directly to a C ABI call.

### What FFI v1 supports

#### Argument and return types (primitives only)

- `Int`  → `int64_t`
- `Real` → `double`
- `Bool` → `int` (0 / 1)
- `Char` → `int32_t` (Unicode scalar value: `0..0x10FFFF` excluding
  the surrogate range `0xD800..0xDFFF`). A `Char` produced inside
  kaikai always satisfies this invariant; an `int32_t` crossing back
  from C that is not a scalar value is undefined at the type level —
  validate on the C side, or route through `int_to_char` (which panics
  on a non-scalar-value argument).
- `String` → `const char *` (NUL-terminated, kaikai-allocated;
  caller-side lifetime, do not free from C)
- `Unit` → `void` (return only)

#### Calling conventions

- C ABI of the host platform (System V AMD64 or AAPCS AArch64
  on supported targets). The C compiler handles register vs.
  stack assignment.
- Kaikai's call site emits a single C function call after
  unboxing arguments from `KaiValue *` to raw scalars. Return
  values are wrapped back to `KaiValue *` at the boundary.

#### Linking

The C source you bind against compiles independently and links
into the kaikai binary as a normal `.o` / `.a` / `.so`. The
package manager does not automate C source compilation; the
consumer's build (`Makefile`, shell script, etc.) must produce
the C object before invoking `kai build`.

### FFI capability matrix

| Feature | Status | Notes |
| --- | --- | --- |
| Scalars (`Int`/`Real`/`Bool`/`Char`/`String`/`Unit`) | **Shipped (v1)** | both backends |
| Fixed-width types (`U8`..`F32`) | **Shipped (v2, #417)** | C backend; native routes via `--backend=c` |
| Struct / record by value | **Shipped (v2, #417)** | C backend; native routes via `--backend=c` |
| Return-by-value of structs | **Shipped (v2, #417)** | C backend |
| Opaque pointers / handles | **Shipped (v2, #417)** | both backends; no-Drop ownership |
| Out-parameters / pointer-to-T arguments | Not supported | model via opaque handle + accessor |
| C unions | Not planned | — |
| Variadic C functions (`printf` family) | Not planned | — |
| Callbacks from C back into kaikai | Not planned | post-v2 |
| Bitfields | Not planned | — |

The C shim pattern below remains valid for what stays out of scope
(unions, variadics, callbacks) and for the native backend's struct
gap. For the common cases it is no longer needed — see
[What FFI v2 adds](#what-ffi-v2-adds-417) and `kai info ffi`.

### The C shim pattern

When binding a library that uses small structs by value (raylib's
`Color` and `Vector2`, SDL2's `SDL_Rect`, etc.), the canonical
workaround is a **C shim** that flattens aggregates into
primitives at the boundary, reconstructing them inside C before
calling the real library.

#### Example: raylib `Color`

raylib's signature:

```c
void DrawCircleV(Vector2 center, float radius, Color color);
typedef struct Color { uint8_t r, g, b, a; } Color;
typedef struct Vector2 { float x, y; } Vector2;
```

kaikai cannot pass a `{r, g, b, a}` record directly across `extern
"C"` in v1. The shim packs `Color` into a single `int64_t`
(`0xRRGGBBAA`) and splits `Vector2` into two `Real`s:

```c
// raylib_shim.c
void kai_raylib_draw_circle_v(double cx, double cy,
                              double radius, int64_t rgba) {
    Vector2 center = { (float) cx, (float) cy };
    Color color = {
        (uint8_t)((rgba >> 24) & 0xFF),
        (uint8_t)((rgba >> 16) & 0xFF),
        (uint8_t)((rgba >>  8) & 0xFF),
        (uint8_t)( rgba        & 0xFF),
    };
    DrawCircleV(center, radius, (float) radius, color);
}
```

The kaikai-side declaration is then a flat call:

```kai
extern "C"("kai_raylib_draw_circle_v")
pub fn draw_circle_v(cx: Real, cy: Real,
                     radius: Real, rgba: Int) : Unit / Ffi
```

#### Building the shim

The shim is a regular C source file. It compiles with the host
C compiler, linked into the kaikai binary as part of the
consumer's build:

```sh
cc -c -O2 raylib_shim.c -o raylib_shim.o
cc kaikai_program.c raylib_shim.o -lraylib -o app
```

A real kaikai consumer typically wraps this in a `Makefile` or
similar that runs alongside `kai build`. See
[`lnds/uira`](https://github.com/lnds/uira) for a complete
working example: `uira/raylib.kai` (kaikai-side bindings) +
`uira/raylib_shim.c` (the C shim) + `Makefile` (link
orchestration).

#### Cost of the workaround

The shim approach works but has real costs:

- **Every binding doubles in size**: kaikai-side declaration +
  C shim wrapper. raylib has ~40 functions taking `Color` or
  `Vector2`; that's ~40 shim entries plus their forward
  declarations.
- **Consumers must thread non-kaikai assets through their build**.
  The package manager v1 (#405) ships `lib/` files cleanly via
  `kai install` but does not automate C source. Documenting the
  shim build as part of the package's README is the current
  pattern (see uira's README).
- **Future bindings** (SDL2, GLFW, libcurl, …) will hit the same
  wall and need their own shim layer until FFI v2 lands.

## What FFI v2 adds (#417)

FFI v2 ships three pieces on top of the v1 scalar boundary. The
authoritative surface reference is `kai info ffi`.

1. **Fixed-width boundary annotations** — `U8 U16 U32 U64 I8 I16 I32
   I64 F32`. Boundary-only types: a value flowing through one is a
   plain `Int` (or `Real` for `F32`) on the kaikai side (so it unifies
   with literals and feeds arithmetic), C-cast to the exact width at
   the shim. No arithmetic on `U8` in kaikai-land — these exist for C
   layout correctness, not as a numeric primitive set.

2. **Struct-by-value across the boundary** (incl. return-by-value):

   ```kai
   extern "C" type Color = { r: U8, g: U8, b: U8, a: U8 }
   extern "C" type Vector2 = { x: F32, y: F32 }

   extern "C"("DrawCircleV")
   pub fn draw_circle_v(center: Vector2, radius: F32,
                        color: Color) : Unit / Ffi

   extern "C"("GetMousePosition")
   pub fn get_mouse_position() : Vector2 / Ffi
   ```

   The shim unwraps the record's fields into a local C struct with the
   real widths, passes by value, and re-boxes a returned struct. Each
   struct field must be a fixed-width type (or a nested `extern "C"
   type`) — `Int`/`Real`/`String` fields are rejected (they break the
   small-struct layout). The C ABI register/memory classification is
   the C compiler's job.

3. **Opaque handles** — `extern "C" opaque Name`, for types the kaikai
   side threads through but never inspects (libpq `PGconn *`,
   `PGresult *`). Backed by a reference-counted box parking the C
   `void *`. **Ownership rule: no Drop integration** — kaikai RC frees
   the box but NEVER the C resource; the driver calls the C destructor
   explicitly. A handle threaded through many calls is never
   double-freed.

> **Backend status.** Fixed-width, struct-by-value, and opaque all work
> on the **C-direct backend** (`--backend=c`). On the **native**
> (libLLVM) default backend, **opaque handles** are fully supported;
> **struct-by-value and standalone fixed-width** route through the C
> backend (a native build reports the gap and the `--backend=c` fix).
> Native LLVM struct/width marshalling is a planned follow-up.

The raylib-style binding (small structs by value) and the libpq-style
driver (opaque handles + strings) both stop needing hand-written C
shims for the common case. C shims remain only for what stays out of
scope: callbacks from C into kaikai, unions, variadics, bitfields.

## Bindgen (post-FFI-v2)

The roadmap (`docs/design.md`) mentions `kai bindgen foo.h` —
read a C header, emit an `extern` module. Not in scope for v1.0
or v1.1; tracked separately.

## Related

- [`docs/design.md`](design.md) §"FFI / Interop" — design pin.
- [`docs/stage2-design.md`](stage2-design.md) §"FFI" — current
  v1 implementation notes.
- Issue [#260](https://github.com/lnds/kaikai/issues/260) /
  [#261](https://github.com/lnds/kaikai/issues/261) — `extern
  "C"` declaration syntax + name override.
- Issue [#417](https://github.com/lnds/kaikai/issues/417) —
  FFI v2 planning (struct-by-value, opaque handles, return-by-value).
- [`lnds/uira`](https://github.com/lnds/uira) — canonical
  example of a real C library binding (raylib) using FFI v1
  with C shims.
