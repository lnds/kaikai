/* C shim for the fixed-width FFI fixture. Each `id_*` returns its
 * argument unchanged, so the kaikai output reveals whether the boundary
 * preserved the value — in particular whether the return widened with
 * the correct signedness (zext for U*, sext for I*).
 *
 * Standalone — only <stdint.h>. */

#include <stdint.h>

uint8_t  id_u8(uint8_t x)   { return x; }
int8_t   id_i8(int8_t x)    { return x; }
uint32_t id_u32(uint32_t x) { return x; }
int32_t  id_i32(int32_t x)  { return x; }
uint8_t  trunc_to_u8(uint8_t x) { return x; }   /* arg already truncated at the boundary */
float    scale_f32(float x) { return x * 2.0f; }
