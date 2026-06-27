# ffi

Foreign function interface — calling C via the `Ffi` capability.

## Description

kaikai reaches C through `extern "C"` declarations. Every FFI call
crosses the `Ffi` capability, so an extern signature MUST carry `/ Ffi`
in its effect row. There is one ABI (`"C"`); the symbol the linker
binds is the kaikai identifier, or an explicit override.

```text
extern "C" fn cos(x: Real) : Real / Ffi             # binds C `cos`
extern "C"("DrawCircleV") pub fn draw(c: Real) : Unit / Ffi   # override
```

## Boundary types

Scalars cross directly:

| kaikai      | C            |
|-------------|--------------|
| `Int`       | `int64_t`    |
| `Real`      | `double`     |
| `Bool`      | `int`        |
| `Char`      | `int32_t` (Unicode codepoint) |
| `String`    | `const char *` (copied in on return via `kai_str`) |
| `Unit`      | `void` (return only) |

The compiler emits a forward declaration over these C types
(`extern <ret> sym(<params>);`) on both backends, so the call links
without an implicit declaration. The declaration IS the binding
contract: a symbol a system header already declares (libc) must be
bound with the C-exact type — `abs(n: I32) : I32` (libc's `int`),
not `: Int` — or `cc` rejects the redeclaration. A symbol whose C
type has no kaikai mapping (`size_t`, a struct return) is wrapped in
a small `.c`.

## Fixed-width boundary types

`U8 U16 U32 U64 I8 I16 I32 I64 F32` annotate the EXACT C width at the
boundary — `uint8_t`, `int32_t`, `float`, … They are boundary
annotations ONLY: a value flowing through one is a plain `Int` (or
`Real` for `F32`) on the kaikai side, so it unifies with integer/real
literals and feeds arithmetic. The shim C-casts at the call (no
range-check — exactly C's own truncation). There is no `U8 + U8` in
kaikai-land; the wider numeric-primitive question is separate.

```text
extern "C"("SetVolume") fn set_volume(level: U8) : Unit / Ffi
```

## Struct-by-value

`extern "C" type Name = { field: <fixed-width>, ... }` maps a kaikai
record to a C struct passed BY VALUE, both ways (including
return-by-value). Each field MUST carry an exact C width (a fixed-width
type, or a nested `extern "C" type`) — `Int`/`Real`/`String` are
rejected as struct fields, because an `int64_t`/`double`/pointer field
breaks the small-struct layout the C compiler lays out.

```text
extern "C" type Color = { r: U8, g: U8, b: U8, a: U8 }
extern "C" type Vector2 = { x: F32, y: F32 }

extern "C"("DrawCircleV")
pub fn draw_circle_v(center: Vector2, radius: F32, color: Color) : Unit / Ffi

extern "C"("GetMousePosition")
pub fn get_mouse_position() : Vector2 / Ffi
```

The shim unwraps the record's fields into a local C struct with the
real widths, calls by value, and re-boxes a returned struct into a
fresh record. The C ABI (small struct in registers vs. memory per
SysV / AAPCS) is the C compiler's job — kaikai emits the struct type
and lets the C compiler classify it.

### The shim names the struct from `kai_ffi.h`

A program with an `extern "C" type` struct generates a `kai_ffi.h`
beside the build, with one `typedef` per declared type. A hand-written
C shim `#include`s it and names the SAME type — so the boundary is one
authoritative struct definition the C compiler can check, not a layout
twin the linker silently matches:

```c
#include "kai_ffi.h"        /* provides Color, Vector2, ... */

void draw_circle_v(Vector2 center, float radius, Color color) { ... }
Vector2 get_mouse_position(void) { ... }
```

Build with the shim on `CFLAGS`: `CFLAGS="shim.c" kai build --backend=c app.kai`.

> Struct-by-value compiles on the **C-direct backend** (`--backend=c`).
> The native (libLLVM) backend does NOT yet marshal it: a small struct's
> C ABI (coerced to `iN` / `[N x float]` / `sret` per SysV / AAPCS) is
> frontend classification LLVM does not derive from a struct value, so a
> native build of a struct-FFI program fails with an actionable message
> naming the function and the `--backend=c` fix. The C compiler owns that
> classification; native ABI classification is a planned follow-up.

## Opaque handles

`extern "C" opaque Name` is a value the kaikai side passes around but
never inspects — the database / socket-driver case (`PGconn *`,
`PGresult *`). Backed by a reference-counted box that parks the C
`void *`.

```text
extern "C" opaque Conn

extern "C"("PQconnectdb") fn connect(s: String) : Conn / Ffi
extern "C"("PQexec")      fn exec(c: Conn, q: String) : Int / Ffi
extern "C"("PQfinish")    fn finish(c: Conn) : Unit / Ffi
```

**Ownership rule (no Drop integration).** kaikai's reference counting
manages the handle BOX (it is dup'd before each use and freed at the
handle's last use), but it NEVER frees the parked C resource. The
driver author calls the C destructor explicitly (`PQfinish` above). A
handle may be threaded through any number of calls; passing it to two
calls does not double-free it. Opaque handles work on BOTH backends.

A `char *` a C function returns while borrowing from a live handle
(e.g. `PQgetvalue` tied to a `PGresult`) is COPIED at the boundary
(same as a returned `String`), so it never dangles when the handle is
later destroyed.

## Still rejected

These are out of scope and rejected at compile time:

- C unions, bitfields, variadic functions.
- Callbacks from C (a function-typed extern parameter): a kaikai
  closure is a heap box with captures, not a bare C function pointer.
- A struct field that is not a fixed-width type or a nested
  `extern "C" type`.

## See also

`kai info syntax`, `docs/ffi.md`.
