/* Phase 3 unboxing benchmark reference — fib(35) in C.
 *
 * Companion to `fib35_bench.kai`. Use this as the C / -O2
 * reference in `docs/benchmarks/compute_2026-05-09.md`.
 *
 *   clang -O2 fib35_bench_c.c -o /tmp/fib35_c
 *   /tmp/fib35_c
 */
#include <stdio.h>
#include <stdint.h>

static int64_t fib(int64_t n) {
  if (n < 2) return n;
  return fib(n - 1) + fib(n - 2);
}

int main(void) {
  printf("%lld\n", (long long) fib(35));
  return 0;
}
