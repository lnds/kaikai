#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static void host_handler(int sig, siginfo_t *info, void *ucp) {
    (void) sig; (void) info; (void) ucp;
    static const char msg[] = "host handler ran\n";
    ssize_t w = write(1, msg, sizeof msg - 1);
    (void) w;
    _exit(0);
}

/* Installed before the runtime starts, as an embedded engine's handler would be. */
__attribute__((constructor)) static void install_host_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = host_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

int64_t fault_outside_guard(void) {
    *(volatile int *) 8 = 1;
    return 0;
}
