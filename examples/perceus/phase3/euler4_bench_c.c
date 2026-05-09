/* Phase 3 unboxing benchmark reference — Euler #4 palindrome.
 *
 *   clang -O2 euler4_bench_c.c -o /tmp/euler4_c
 *   /tmp/euler4_c
 */
#include <stdio.h>
#include <stdint.h>

static int64_t reverse_int(int64_t n, int64_t acc) {
  if (n == 0) return acc;
  return reverse_int(n / 10, acc * 10 + n % 10);
}

static int is_palindrome(int64_t n) {
  return reverse_int(n, 0) == n;
}

static int64_t search(int64_t a, int64_t b, int64_t best) {
  if (a < 100) return best;
  if (b < a)   return search(a - 1, 999, best);
  int64_t p = a * b;
  int64_t new_best = (p > best && is_palindrome(p)) ? p : best;
  return search(a, b - 1, new_best);
}

int main(void) {
  printf("%lld\n", (long long) search(999, 999, 0));
  return 0;
}
