/* Issue #107 — Signal trap fixture harness.
 *
 * Forks the kaikai binary as a child, waits for it to install its
 * signal subscription, then sends a signal. Returns the child's
 * exit status. Used by stage2/Makefile's `test-effects` rule for
 * `examples/effects/issue_107_signal_trap.kai` and
 * `m8x_signal_await_parks.kai`.
 *
 * Why a parent-child harness instead of a shell `kill -INT $!`:
 * macOS's Claude Code sandbox (and other sandboxes) filter
 * kill(2) across process trees. A direct parent → child kill
 * always works regardless of sandbox profile. The pattern also
 * mirrors how real supervisors (lnds/ahu's run_app,
 * lnds/manutara's server) cancel children: parent signals, child
 * cleans up.
 *
 * Readiness synchronisation (NOT a fixed timer). The harness pipes
 * the child's stdout, reads until the child prints a "ready" line,
 * and only THEN sends the signal — forwarding everything it reads to
 * its own stdout so the golden diff is unchanged. A fixed `usleep`
 * raced under parallel `make -j` load: when the box was busy the
 * child had not yet reached `Signal.await()` (or had not run its
 * pre-await compute steps) by the time the signal fired, so the
 * output order broke or the program hung waiting on a never-delivered
 * park. Synchronising on the marker the child already emits removes
 * the timing dependency entirely. A child that never prints "ready"
 * (EOF first) falls back to sending the signal at EOF so a regression
 * still surfaces rather than hanging the harness.
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

#define READY_MARKER "ready"

/* Whole-run deadline. A child that never prints "ready" or never
 * exits after the signal must become an actionable failure, not an
 * open-ended hang that eats a CI shard's wall clock with no
 * diagnostic. Override with KAI_SIGHARNESS_TIMEOUT_S. On expiry the
 * harness reports which phase stalled, SIGKILLs the child, and
 * exits 124 (the timeout(1) convention). */
#define DEFAULT_TIMEOUT_S 30

static volatile sig_atomic_t timed_out = 0;

static void on_alarm(int sig) {
    (void) sig;
    timed_out = 1;
}

/* SIGALRM must interrupt read()/waitpid(), so no SA_RESTART. */
static void arm_deadline(unsigned int secs) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, NULL);
    alarm(secs);
}

static int timeout_secs(void) {
    const char *e = getenv("KAI_SIGHARNESS_TIMEOUT_S");
    if (e && *e) {
        int v = atoi(e);
        if (v > 0) return v;
    }
    return DEFAULT_TIMEOUT_S;
}

/* Report the stalled phase, kill the child hard, reap it, exit 124. */
static void give_up(pid_t cpid, const char *phase, const char *expected,
                    const char *got) {
    fprintf(stderr,
            "sigharness: TIMEOUT after %ds %s\n"
            "sigharness:   expected: %s\n"
            "sigharness:   got:      %s\n"
            "sigharness: sending SIGKILL to child pid %d\n",
            timeout_secs(), phase, expected, got, (int) cpid);
    kill(cpid, SIGKILL);
    while (waitpid(cpid, NULL, 0) < 0 && errno == EINTR) {}
    exit(124);
}

static int signal_for_name(const char *name) {
    if (!strcmp(name, "INT"))  return SIGINT;
    if (!strcmp(name, "TERM")) return SIGTERM;
    if (!strcmp(name, "HUP"))  return SIGHUP;
    if (!strcmp(name, "USR1")) return SIGUSR1;
    if (!strcmp(name, "USR2")) return SIGUSR2;
    return -1;
}

/* Forward `buf[0..n)` to stdout, returning 0 on success. */
static int forward(const char *buf, ssize_t n) {
    ssize_t off = 0;
    while (off < n) {
        ssize_t w = write(STDOUT_FILENO, buf + off, (size_t)(n - off));
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        off += w;
    }
    return 0;
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

    int pipefd[2];
    if (pipe(pipefd) != 0) { perror("pipe"); return 2; }

    pid_t cpid = fork();
    if (cpid < 0) { perror("fork"); return 2; }
    if (cpid == 0) {
        /* Child: stdout -> pipe write end; the harness reads + forwards. */
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) { perror("dup2"); _exit(127); }
        close(pipefd[1]);
        execvp(argv[argi], &argv[argi]);
        perror("execvp");
        _exit(127);
    }

    /* Parent: read the child's stdout, forwarding it, until the
     * READY_MARKER line is seen — then signal. A small line buffer is
     * enough; the marker is a whole line the fixtures print on its own. */
    close(pipefd[1]);
    arm_deadline((unsigned int) timeout_secs());
    char    buf[256];
    char    line[512];
    size_t  linelen = 0;
    int     signalled = 0;
    ssize_t n;
    for (;;) {
        n = read(pipefd[0], buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                if (!timed_out) continue;
                line[linelen] = '\0';
                give_up(cpid,
                        signalled ? "waiting for output after signalling"
                                  : "waiting for \"" READY_MARKER "\" line",
                        signalled ? "child output then EOF"
                                  : "a line reading \"" READY_MARKER "\"",
                        linelen ? line : "(no partial line)");
            }
            perror("read");
            break;
        }
        if (n == 0) break;
        if (forward(buf, n) != 0) { perror("write"); }
        if (signalled) continue;
        for (ssize_t i = 0; i < n && !signalled; i++) {
            char c = buf[i];
            if (c == '\n') {
                line[linelen] = '\0';
                if (!strcmp(line, READY_MARKER)) {
                    if (kill(cpid, signo) != 0) fprintf(stderr, "kill: %s\n", strerror(errno));
                    signalled = 1;
                }
                linelen = 0;
            } else if (linelen + 1 < sizeof(line)) {
                line[linelen++] = c;
            }
        }
    }
    /* EOF before "ready": send the signal anyway so a regression that
     * never reaches the await is caught, not silently hung. */
    if (!signalled) {
        if (kill(cpid, signo) != 0) fprintf(stderr, "kill: %s\n", strerror(errno));
    }
    close(pipefd[0]);

    int status = 0;
    while (waitpid(cpid, &status, 0) < 0) {
        if (errno == EINTR) {
            if (!timed_out) continue;
            give_up(cpid, "waiting for child to exit after the signal",
                    "child exit status 0", "child still running");
        }
        perror("waitpid");
        return 2;
    }
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}
