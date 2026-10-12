#define _GNU_SOURCE
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
static struct timespec kperf_t0;
static void kperf_done(void) {
  struct timespec t1, c; clock_gettime(CLOCK_MONOTONIC, &t1); clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c);
  long long ns = (t1.tv_sec - kperf_t0.tv_sec) * 1000000000LL + (t1.tv_nsec - kperf_t0.tv_nsec);
  long long cpu = c.tv_sec * 1000000000LL + c.tv_nsec;
  const char *p = getenv("KPERF_LOG"); if (!p) return;
  int fd = open(p, O_WRONLY | O_APPEND | O_CREAT, 0644); if (fd < 0) return;
  char buf[256]; int n = snprintf(buf, sizeof buf, "run %lld cpu=%lld\n", ns, cpu);
  if (write(fd, buf, n) < 0) {} close(fd);
}
__attribute__((constructor)) static void kperf_start(void) {
  clock_gettime(CLOCK_MONOTONIC, &kperf_t0); atexit(kperf_done);
}
