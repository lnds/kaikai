#include <stdint.h>
#include <stdlib.h>

extern int64_t kai_cmp(int64_t, int64_t);

static int cmp(const void *a, const void *b) {
    return (int) kai_cmp(*(const int64_t *) a, *(const int64_t *) b);
}

int64_t sort_weighted(void) {
    int64_t v[] = { 5, 3, 9, 1, 7, 2 };
    qsort(v, 6, sizeof v[0], cmp);
    int64_t sum = 0;
    for (int i = 0; i < 6; i++) sum += v[i] * (i + 1);
    return sum;
}
