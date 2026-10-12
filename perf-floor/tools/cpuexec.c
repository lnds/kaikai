/* cpuexec TAG REAL [args...]: run REAL, append "TAG wall_ns cpu_ns rc INFO" to $KPERF_LOG (INFO from $KPERF_INFO). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/resource.h>
int main(int argc, char **argv) {
  if (argc < 3) return 127;
  struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
  pid_t p = fork();
  if (p == 0) { execvp(argv[2], argv + 2); _exit(127); }
  int st = 0; struct rusage ru; wait4(p, &st, 0, &ru);
  clock_gettime(CLOCK_MONOTONIC, &b);
  long long wall = (b.tv_sec - a.tv_sec) * 1000000000LL + (b.tv_nsec - a.tv_nsec);
  long long cpu = (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000000LL + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1000LL;
  int rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
  const char *log = getenv("KPERF_LOG"), *info = getenv("KPERF_INFO");
  if (log) { int fd = open(log, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd >= 0) { char buf[512]; int n = snprintf(buf, sizeof buf, "%s %lld cpu=%lld rc=%d %s\n", argv[1], wall, cpu, rc, info ? info : "");
      if (write(fd, buf, n) < 0) {} close(fd); } }
  return rc;
}
