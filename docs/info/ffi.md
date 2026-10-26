# ffi

Foreign function interface — calling C via the `Ffi` capability.

## Description

kaikai reaches C through `extern "C"` declarations. Every FFI call
crosses the `Ffi` capability, so an extern signature MUST carry `/ Ffi`
in its effect row. C reaches kaikai through an `extern "C" fn` with a body
(*Exporting kaikai functions to C*, below). There is one ABI (`"C"`); the symbol the linker
binds is the kaikai identifier, or an explicit override.

```kaikai-fragment
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

`Int32`, `UInt32` and `UInt64` also cross at their exact C width. Any other
type in an `extern "C" fn` signature, parameter or return, is rejected at
check time unless it names a declared `extern "C" type` or `opaque`.

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

```kaikai-fragment
extern "C"("SetVolume") fn set_volume(level: U8) : Unit / Ffi
```

## Struct-by-value

`extern "C" type Name = { field: <fixed-width>, ... }` maps a kaikai
record to a C struct passed BY VALUE, both ways (including
return-by-value). Each field MUST carry an exact C width (a fixed-width
type, or a nested `extern "C" type`) — `Int`/`Real`/`String` are
rejected as struct fields, because an `int64_t`/`double`/pointer field
breaks the small-struct layout the C compiler lays out.

```kaikai-fragment
extern "C" pub type Color = { r: U8, g: U8, b: U8, a: U8 }
extern "C" pub type Vector2 = { x: F32, y: F32 }

extern "C"("DrawCircleV")
pub fn draw_circle_v(center: Vector2, radius: F32, color: Color) : Unit / Ffi

extern "C"("GetMousePosition")
pub fn get_mouse_position() : Vector2 / Ffi
```

An extern type is a record on the kaikai side: `Vector2 { x: 1.0, y: 2.0 }`
builds one, `Vector2 { ...v, x: 5.0 }` copies one with a field changed, and
a field reads as `v.x`. It follows the visibility rules of `type`: declare
it `pub` (the keyword goes after `extern "C"`, as on `extern "C"("sym") pub
fn`) to use it from another module, and a `pub` fn signature cannot name a
private one. An importer builds it with `m.Vector2 { ... }` or imports the
name.

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
A header the generated code must see first rides the same variable,
`CFLAGS="-include shim.h shim.c"`; it may include system headers freely.

In a package, declare the shim in `kai.toml` instead — a `[native]`
table (`sources`/`include`/`libs`) travels with the dependency, so
consumers build with plain `kai build` and no `CFLAGS` (see
`kai info packages`). A `[native]` source includes `kai_ffi.h` the same
way a `CFLAGS` shim does.

`kai` writes `kai_ffi.h` per build, under the build's scratch dir, and
puts that directory on the include path of the C sources it compiles
by hand (`CFLAGS` shims and `[native]` sources), on every backend and
in `kai build`, `kai run` and `kai test`. The header is regenerated from
the program, so a struct that changes rebuilds the shim against it.
`kaic2 --ffi-header-out <file>` writes it during a compile.

> Struct-by-value works on **both backends**. The C-direct backend
> (`--backend=c`) lets the C compiler own the ABI; the native (libLLVM)
> backend classifies each aggregate in the emitter and coerces it to the
> platform calling convention (`iN` / `[N x float]` / `sret`), so a
> clang-compiled callee receives it correctly. AArch64 (arm64 Darwin,
> AAPCS64) and x86-64 SysV are marshalled; a target the classifier does
> not model falls back to an actionable `--backend=c` diagnostic naming
> the function.

## Opaque handles

`extern "C" opaque Name` is a value the kaikai side passes around but
never inspects — the database / socket-driver case (`PGconn *`,
`PGresult *`). Backed by a reference-counted box that parks the C
`void *`.

```kaikai-fragment
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

## Exporting kaikai functions to C

`extern "C" fn` WITH a body exports a top-level kaikai function under the
C ABI, so C can call it or take its address (`qsort`, iterators, webview
bindings). Without a body it imports a C function, as above.

```kaikai-fragment
extern "C" fn on_save(id: Int, req: String) : Unit / Stdout =
  Stdout.print("saving #{id}: #{req}")
extern "C"("app_cmp") pub fn cmp(a: Int, b: Int) : Int = a - b   # symbol named
```

The `("sym")` override names the EXPORTED symbol.

Rules, all checked at compile time:

- Top-level functions only — no closures, no type parameters.
- The row is closed and lists only effects with a default handler
  (`Stdout`, `Stderr`, `Clock`, `Random`, `Log`, `Spawn`, ...). C calls
  with no `handle` on the stack, so `State`, a user effect or an
  open row is an error at the declaration naming the effect. `Ffi` is
  not required: the body is kaikai code.
- Boundary types are the import ones minus two: a `String` is accepted
  as a PARAMETER only (copied in from the C string, which the caller keeps),
  and a struct by value is not supported. Scalars, fixed-width types and
  `extern "C" opaque` handles cross as they do for imports; a
  function-typed parameter is still rejected.

**Where the call runs.** The adapter picks per call:

1. C called back from inside an FFI call on a kaikai fiber (`qsort`'s
   comparator): the body runs on that fiber, synchronously.
2. A thread the runtime does not own (a `pthread`, a GUI thread): the
   body runs on a scheduler fiber and the calling thread blocks until it
   returns. The body may park (`await`, actor round trips, sleeps) like
   any fiber. Rule: an exported function called from a foreign thread
   must not wait on that same thread, or the two deadlock. This needs
   the scheduler pool: with `KAI_THREADS=1` the call aborts with
   `set KAI_THREADS>1 (or unset it)`.

**Panics stop at the boundary.** A panic, trap or cancellation inside the
body never unwinds through C frames: the process prints
`kai: panic in exported function <name>` and aborts.

**Context as a value.** A C API with a `void *userdata` carries a kaikai
value through `ffi.retain_userdata`, which parks a private copy of it behind a
`Userdata` handle (an `extern "C" opaque`, so it crosses as `void *`).
The exported function receives the handle and reads the value back with
`ffi.take_userdata[T]`; `ffi.release_userdata` frees the parked copy. State belongs in
actors: park the owner's `Pid` and forward from the callback with
`mailbox_send(pid, msg)`.

```kaikai
import ffi

type Msg = Tick(Int) | Done

extern "C" fn on_event(ud: Userdata, n: Int) : Unit = {
  let pid: Pid[Msg] = take_userdata(ud)
  mailbox_send(pid, if n < 0 { Done } else { Tick(n) })
}

fn main() : Int = 0
```

`take_userdata[T]` trusts `T` to be the type the handle was retained at.

## JavaScript imports

`extern "js"("path") fn ...` declares a JavaScript import for the
`wasm32-browser` target. Its capability is `Js`, not `Ffi`: the row MUST
carry `/ Js`. The path is mandatory.

```kaikai-fragment
extern "js"("console.log")     fn log(s: String) : Unit / Js
extern "js"("document.title=") fn set_title(s: String) : Unit / Js
extern "js"("document.title")  fn title() : String / Js
```

The page host resolves a path by walking it from `globalThis` and
calls the function with `this` bound to the parent object. The path
forms:

| Path | Meaning |
|------|---------|
| `a.b.c` | call `a.b.c(args)`; a zero-argument decl whose target is not a function reads the property |
| `a.b.c=` | write the property; exactly one parameter, returns `Unit` |
| `.name` | member of the FIRST argument: `.appendChild(parent, child)` calls `parent.appendChild(child)`; with no further arguments and a non-function member, reads it |
| `.name=` | write a member of the first argument: `(n, s)` sets `n.name = s` |

A package that needs code which is not a global lists its own module
in `kai.toml`; the modules of the entry package and of its dependencies are
loaded, the entry package's exports win over a dependency's, and both win
over `globalThis`:

```text
[js]
module = "glue.mjs"
```

Boundary types: `Real` is an `f64`, `Bool` a boolean, `Int` and
`U64`/`I64` an `i64` (a `BigInt` in JavaScript), the narrower
fixed-width types a `Number`. A `String` is copied both ways as UTF-8
(pointer and length through the host). `extern "js" opaque Node`
declares a handle: an index into a table the host keeps, released the
moment the last reference dies (the same Perceus drop as any value), so
the table never outlives the program's use. The host builtin
`kai.handles` (declare `extern "js"("kai.handles") fn live() : Int / Js`)
returns the number of live handles.

Rejected: a function-typed parameter or return (a callback cannot cross
to JavaScript; deliver events as values), a struct by value, and a
member path with no first parameter.

`Js` type-checks on every target and has no handler: a `main` whose row
reaches it is refused on a host build (`kai build` with no `--target`)
and runs on `--target=wasm32-browser`. The native backend does not
reach JavaScript; the target builds through the C backend.
`extern "C"` on `wasm32-browser` is unchanged: a C symbol linked into
the module.

## Thread affinity

`main` runs on the OS thread the process entered on, at every
`KAI_THREADS`. That is a guarantee, not an accident of scheduling: a
thread-affine C library refuses to work anywhere else. AppKit — and
through it GLFW, raylib, SDL, GTK — traps outright if a window is
created or events are pumped off the process main thread.

So the calls those libraries demand be made "on the main thread" can
be made directly from `main`, with the parallel scheduler on. The
fibers `main` spawns are NOT covered: they migrate across the worker
pool by design, so a thread-affine call belongs in `main` itself, not
in a spawned fiber.

## Still rejected

These are out of scope and rejected at compile time:

- C unions, bitfields, variadic functions.
- A function-typed extern parameter: a kaikai closure is a heap box with
  captures, not a bare C function pointer. Export a top-level function
  instead and hand C its symbol.
- A struct field that is not a fixed-width type or a nested
  `extern "C" type`.

## See also

`kai info syntax`, `docs/ffi.md`.
