/* C shim for the honest fixed-width FFI fixture (numeric lane A). Each
 * function takes and returns the exact C width, so the kaikai output
 * reveals whether the boundary preserved the value AND the width — in
 * particular whether a uint32_t above i32 max and a uint64_t above i63
 * survive the round trip without sign corruption.
 *
 * Standalone — only <stdint.h>. */

#include <stdint.h>

int32_t  add_i32(int32_t a, int32_t b)    { return a + b; }
uint32_t add_u32(uint32_t a, uint32_t b)  { return a + b; }
uint64_t add_u64(uint64_t a, uint64_t b)  { return a + b; }
