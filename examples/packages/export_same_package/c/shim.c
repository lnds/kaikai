#include <stdint.h>

extern void on_tick(int64_t n);

void fire(int64_t n) {
  for (int64_t i = 1; i <= n; i++) on_tick(i);
}
