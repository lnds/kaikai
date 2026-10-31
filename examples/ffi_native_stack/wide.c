#include <stdint.h>

int64_t wide16(int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5, int64_t a6, int64_t a7,
               int64_t a8, int64_t a9, int64_t a10, int64_t a11, int64_t a12, int64_t a13, int64_t a14, int64_t a15) {
    int64_t v[] = { a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15 };
    int64_t sum = 0;
    for (int i = 0; i < 16; i++) sum += v[i] * (i + 1);
    return sum;
}
