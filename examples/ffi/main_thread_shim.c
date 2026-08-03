/* Is the caller running on the OS thread the process entered `main` on?
 * The question a thread-affine C library (AppKit, and through it
 * GLFW/SDL/GTK) asks before it will initialise. */

#include <stdint.h>

#if defined(__APPLE__)
#  include <pthread.h>
int64_t kai_on_main_thread(void) { return pthread_main_np() ? 1 : 0; }
#elif defined(__linux__)
#  include <sys/syscall.h>
#  include <unistd.h>
int64_t kai_on_main_thread(void) {
    return (pid_t) syscall(SYS_gettid) == getpid() ? 1 : 0;
}
#else
/* No portable test: report success so the fixture asserts nothing it
 * cannot observe rather than failing on an untested platform. */
int64_t kai_on_main_thread(void) { return 1; }
#endif
