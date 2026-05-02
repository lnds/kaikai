/* Issue #107 — Signal trap fixture harness.
 *
 * Forks the kaikai binary as a child, waits for it to install its
 * signal subscription, then sends a signal. Returns the child's
 * exit status. Used by stage2/Makefile's `test-effects` rule for
 * `examples/effects/issue_107_signal_trap.kai`.
 *
 * Why a parent-child harness instead of a shell `kill -INT $!`:
 * macOS's Claude Code sandbox (and other sandboxes) filter
 * kill(2) across process trees. A direct parent → child kill
 * always works regardless of sandbox profile. The pattern also
 * mirrors how real supervisors (lnds/ahu's run_app,
 * lnds/manutara's server) cancel children: parent signals, child
 * cleans up.
 *
 * Default signal is SIGTERM, override with `--sig INT|TERM|HUP|
 * USR1|USR2`. SIGTERM is the default so a regression that makes
 * `Signal.await()` return without parking can't accidentally
 * pass — the fixture program prints the variant name back, and
 * a stub return value would mismatch.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int signal_for_name(const char *name) {
    if (!strcmp(name, "INT"))  return SIGINT;
    if (!strcmp(name, "TERM")) return SIGTERM;
    if (!strcmp(name, "HUP"))  return SIGHUP;
    if (!strcmp(name, "USR1")) return SIGUSR1;
    if (!strcmp(name, "USR2")) return SIGUSR2;
    return -1;
}

int main(int argc, char **argv) {
    int signo = SIGTERM;
    int argi  = 1;
    if (argi + 1 < argc && !strcmp(argv[argi], "--sig")) {
        signo = signal_for_name(argv[argi + 1]);
        if (signo < 0) {
            fprintf(stderr, "unknown signal: %s\n", argv[argi + 1]);
            return 2;
        }
        argi += 2;
    }
    if (argi >= argc) {
        fprintf(stderr, "usage: %s [--sig INT|TERM|HUP|USR1|USR2] <prog> [args...]\n", argv[0]);
        return 2;
    }
    pid_t cpid = fork();
    if (cpid < 0) { perror("fork"); return 2; }
    if (cpid == 0) {
        execvp(argv[argi], &argv[argi]);
        perror("execvp");
        _exit(127);
    }
    /* 500ms is comfortably enough for the child to reach
     * `Signal.on(...)` + `Signal.await()` under v1's inline-eager
     * scheduler on a typical dev machine. Tighten if it becomes a
     * regression vector; loosen if CI is flaky. */
    usleep(500 * 1000);
    if (kill(cpid, signo) != 0) {
        fprintf(stderr, "kill: %s\n", strerror(errno));
    }
    int status = 0;
    if (waitpid(cpid, &status, 0) < 0) { perror("waitpid"); return 2; }
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}
