/* C shim for the FFI scalar regression fixtures (#933 / #934).
 *
 * These are USER symbols — they have no system-header declaration, so
 * the kaikai-emitted forward declaration (`extern <ret> sym(...);`) is
 * the only one the C compiler sees. Each symbol's C type matches the
 * kaikai-side boundary type exactly: `Int`→int64_t, `Char`→int32_t.
 *
 * Standalone — depends only on <stdint.h>, so it links with the
 * kaikai-emitted C without any kaikai headers. */

#include <stdint.h>

int64_t ffi_add_ints(int64_t a, int64_t b) { return a + b; }

int32_t ffi_char_inc(int32_t c) { return c + 1; }
