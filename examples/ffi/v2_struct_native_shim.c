/* Standalone clang-compiled callee for the FFI v2 struct-by-value fixture
 * on the NATIVE backend (issue #1030). Unlike v2_struct_shim.c (which
 * `#include`s the C-backend-generated kai_ffi.h), this names the Color /
 * Vector2 C types directly, so the native object links against a callee
 * built by clang with the real platform ABI — the parity gate that proves
 * the emitter's classification matches the C compiler's, not just itself.
 *
 * Standalone — only <stdint.h>. */

#include <stdint.h>

typedef struct { uint8_t r, g, b, a; } Color;
typedef struct { float x, y; } Vector2;

int64_t color_brightness(Color c) { return (c.r + c.g + c.b) / 3; }

Color make_gray(int32_t level) {
    Color c = { (uint8_t) level, (uint8_t) level, (uint8_t) level, 255 };
    return c;
}

Vector2 vec_add(Vector2 a, Vector2 b) {
    Vector2 r = { a.x + b.x, a.y + b.y };
    return r;
}
