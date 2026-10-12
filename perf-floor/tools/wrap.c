/* Fixed-target wrapper: run REAL with our argv under cpuexec's accounting. */
#include <unistd.h>
#include <stdlib.h>
int main(int argc, char **argv) {
  char **v = malloc(sizeof(char*) * (argc + 3));
  v[0] = CPUEXEC; v[1] = TAG; v[2] = REAL;
  for (int i = 1; i < argc; i++) v[i + 2] = argv[i];
  v[argc + 2] = 0;
  execv(CPUEXEC, v); return 127;
}
