/* Negative companion of v2_struct_shim.c: the shim disagrees with the
 * declared `Color` layout (it reads a field the declaration does not have),
 * so compiling it against the generated `kai_ffi.h` must fail. */

#include "kai_ffi.h"

int64_t color_brightness(Color c) { return (c.r + c.g + c.alpha) / 3; }

Color make_gray(int32_t level) {
    Color c = { (uint8_t) level, (uint8_t) level, (uint8_t) level, 255 };
    return c;
}

Vector2 vec_add(Vector2 a, Vector2 b) {
    Vector2 r = { a.x + b.x, a.y + b.y };
    return r;
}
