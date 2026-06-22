/* C shim for the FFI v2 struct-by-value + opaque fixtures (#417).
 *
 * The struct tags MUST match the names the kaikai emitter mints
 * (`struct __kai_ffi_<Name>`), so the by-value calling convention lines
 * up field-for-field. The opaque handle is an ordinary `void *` the
 * driver owns (malloc here, freed by `counter_free`).
 *
 * Standalone — depends only on <stdint.h> / <stdlib.h>, so it links
 * with the kaikai-emitted C without any kaikai headers. */

#include <stdint.h>
#include <stdlib.h>

typedef struct __kai_ffi_Color   { uint8_t r, g, b, a; } Color;
typedef struct __kai_ffi_Vector2 { float x, y; } Vector2;

int64_t color_brightness(Color c) { return (c.r + c.g + c.b) / 3; }

Color make_gray(int32_t level) {
    Color c = { (uint8_t) level, (uint8_t) level, (uint8_t) level, 255 };
    return c;
}

Vector2 vec_add(Vector2 a, Vector2 b) {
    Vector2 r = { a.x + b.x, a.y + b.y };
    return r;
}

/* Opaque handle: an external resource the kaikai side threads through
 * but never inspects. The driver owns the lifetime. */
typedef struct { long count; } Counter;

void *counter_new(void) {
    Counter *c = (Counter *) malloc(sizeof(Counter));
    c->count = 0;
    return c;
}

int64_t counter_bump(void *p, int64_t by) {
    Counter *c = (Counter *) p;
    c->count += by;
    return c->count;
}

void counter_free(void *p) { free(p); }
