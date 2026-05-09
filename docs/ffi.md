# FFI — Foreign Function Interface

kaikai talks to existing C libraries through an `extern "C"`
declaration plus the `Ffi` effect capability. This document
covers what FFI v1 supports, what it doesn't, the canonical
workaround for the gap (C shims), and the planned scope for
FFI v2.

## Quick reference

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
   symbol. The compiler emits a forward declaration; the linker
   resolves `name` at link time.
2. `extern "C"("symbol") pub fn name(args) : T / Ffi` overrides
   the C symbol when the kaikai-side identifier doesn't match
   (e.g. wrapping `printf` as `c_printf` to avoid kai-side
   keyword conflicts; issues #260 / #261).
3. The `Ffi` effect carries the capability. Any function that
   directly or transitively calls an `extern "C"` declaration
   has `Ffi` in its row. The handler is compiler-synthesised —
   `Ffi` operations lower directly to a C ABI call.

## What FFI v1 supports

### Argument and return types (primitives only)

- `Int`  → `int64_t`
- `Real` → `double`
- `Bool` → `int8_t` (0 / 1)
- `Char` → `int32_t` (Unicode codepoint)
- `String` → `const char *` (NUL-terminated, kaikai-allocated;
  caller-side lifetime, do not free from C)
- `Unit` → `void` (return only)

### Calling conventions

- C ABI of the host platform (System V AMD64 or AAPCS AArch64
  on supported targets). The C compiler handles register vs.
  stack assignment.
- Kaikai's call site emits a single C function call after
  unboxing arguments from `KaiValue *` to raw scalars. Return
  values are wrapped back to `KaiValue *` at the boundary.

### Linking

The C source you bind against compiles independently and links
into the kaikai binary as a normal `.o` / `.a` / `.so`. The
package manager does not automate C source compilation; the
consumer's build (`Makefile`, shell script, etc.) must produce
the C object before invoking `kai build`.

## What FFI v1 does NOT support

| Feature | Status | Tracking |
| --- | --- | --- |
| Struct / record by value | **Not supported** | #417 (FFI v2) |
| Out-parameters / pointer-to-T arguments | **Not supported** | #417 |
| Return-by-value of structs | **Not supported** | #417 |
| Opaque pointers / handles | Workaround via `Int` | #417 |
| C unions | Not planned | — |
| Variadic C functions (`printf` family) | Not planned | — |
| Callbacks from C back into kaikai | Not planned | post-FFI-v2 |
| Bitfields | Not planned | — |
| Fixed-width integer types (`U8`, `I32`, …) | **Not supported** | #417 |

## The C shim pattern

When binding a library that uses small structs by value (raylib's
`Color` and `Vector2`, SDL2's `SDL_Rect`, etc.), the canonical
workaround is a **C shim** that flattens aggregates into
primitives at the boundary, reconstructing them inside C before
calling the real library.

### Example: raylib `Color`

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

### Building the shim

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

### Cost of the workaround

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

## What changes in FFI v2

Tracked under #417. Target: v1.1 (post-2026-05-21 release).

The minimum subset planned for v2:

1. **Struct-by-value across the boundary.** A kaikai record maps
   to a C struct, passable both ways:

   ```kai
   extern "C" type Color = { r: U8, g: U8, b: U8, a: U8 }
   extern "C" type Vector2 = { x: F32, y: F32 }

   extern "C"("DrawCircleV")
   pub fn draw_circle_v(center: Vector2, radius: F32,
                        color: Color) : Unit / Ffi
   ```

2. **Return-by-value of structs**: `GetMousePosition() : Vector2`.

3. **Opaque handles**: `extern "C" opaque Font` for types the
   kaikai side passes around but never inspects.

4. **Fixed-width primitive annotations**: `U8`, `U16`, `U32`,
   `U64`, `I8`, `I16`, `I32`, `F32` — boundary-only types
   (cannot do arithmetic in kaikai-land), used for layout
   correctness in struct fields.

After FFI v2 lands, the raylib binding from uira shrinks ~40
trivial shim wrappers to bare `extern "C"` declarations. C shims
remain available for cases that genuinely need them (callbacks
from C into kaikai, unions, variadics) but the common case of
"library uses small structs by value" stops requiring them.

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
