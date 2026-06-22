/* C shim for the advanced struct-by-value FFI fixture: padding, a
 * >16-byte struct (sret on SysV/AAPCS), and a nested struct. The struct
 * tags MUST match the `struct __kai_ffi_<Name>` names the kaikai emitter
 * mints, so the by-value calling convention lines up field-for-field.
 *
 * Standalone — only <stdint.h>. */

#include <stdint.h>

typedef struct __kai_ffi_Padded { uint8_t a; uint32_t b; } Padded;
typedef struct __kai_ffi_Big    { int32_t x, y, z, w, v; } Big;
typedef struct __kai_ffi_Point  { float px, py; } Point;
typedef struct __kai_ffi_Rect   { Point tl, br; } Rect;

int64_t padded_sum(Padded p) { return (int64_t) p.a + (int64_t) p.b; }

Big make_big(int32_t seed) {
    Big b = { seed, seed + 1, seed + 2, seed + 3, seed + 4 };
    return b;
}

int64_t big_sum(Big b) { return b.x + b.y + b.z + b.w + b.v; }

float rect_area(Rect r) {
    float w = r.br.px - r.tl.px;
    float h = r.br.py - r.tl.py;
    return w * h;
}

Rect make_rect(float s) {
    Rect r = { { 0.0f, 0.0f }, { s, s } };
    return r;
}
