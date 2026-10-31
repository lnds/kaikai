#include <stdint.h>

extern int64_t kai_cb(int64_t);

int64_t call_cb(int64_t x) {
    return kai_cb(x) * 10;
}
