/* C shim for the FFI v2 struct-by-value fixture.
 *
 * Includes the generated `kai_ffi.h` and names the SAME `Color` /
 * `Vector2` types the boundary uses — the documented struct-FFI contract
 * (issue #924). One authoritative struct definition shared by `out.c` and
 * this shim, so a layout mismatch is a C compile error, not silent
 * boundary corruption. */

#include "kai_ffi.h"

int64_t color_brightness(Color c) { return (c.r + c.g + c.b) / 3; }

Color make_gray(int32_t level) {
    Color c = { (uint8_t) level, (uint8_t) level, (uint8_t) level, 255 };
    return c;
}

Vector2 vec_add(Vector2 a, Vector2 b) {
    Vector2 r = { a.x + b.x, a.y + b.y };
    return r;
}
