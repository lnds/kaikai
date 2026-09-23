/* Byte-exact file plumbing and the wall clock for the kai CLI. */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int pump(int from, int to) {
    char buf[65536];
    for (;;) {
        ssize_t n = read(from, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n == 0 ? 0 : -1;
        for (ssize_t off = 0; off < n;) {
            ssize_t w = write(to, buf + off, (size_t) (n - off));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) return -1;
            off += w;
        }
    }
}

/* `cat > path`: all of stdin into a fresh file. */
int kaicli_stdin_to(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    int ok = pump(0, fd) == 0;
    return close(fd) == 0 && ok;
}

/* `cp from to` for a binary: exact bytes, and the source's permission bits. */
int kaicli_copy_exec(const char *from, const char *to) {
    struct stat st;
    int in = open(from, O_RDONLY);
    if (in < 0) return 0;
    if (fstat(in, &st) != 0) {
        close(in);
        return 0;
    }
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 07777);
    int ok = out >= 0 && pump(in, out) == 0 && fchmod(out, st.st_mode & 07777) == 0;
    close(in);
    if (out >= 0 && close(out) != 0) ok = 0;
    return ok;
}

/* Point fd 2 at a fresh `path`; the saved descriptor for kaicli_stderr_restore, or -1. */
int64_t kaicli_stderr_to(const char *path) {
    fflush(stderr);
    int f = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f < 0) return -1;
    int saved = dup(2);
    if (saved < 0 || dup2(f, 2) < 0) {
        if (saved >= 0) close(saved);
        close(f);
        return -1;
    }
    close(f);
    fcntl(saved, F_SETFD, FD_CLOEXEC);
    return saved;
}

void kaicli_stderr_restore(int64_t saved) {
    if (saved < 0) return;
    fflush(stderr);
    dup2((int) saved, 2);
    close((int) saved);
}

void kaicli_msleep(int64_t ms) {
    struct timespec ts = { (time_t) (ms / 1000), (long) (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

/* `date +%s`. */
int64_t kaicli_epoch(void) { return (int64_t) time(NULL); }

/* `date '+%H:%M:%S'`, in local time. */
const char *kaicli_local_hms(void) {
    static char buf[16];
    time_t now = time(NULL);
    struct tm tm;
    if (!localtime_r(&now, &tm) || strftime(buf, sizeof(buf), "%H:%M:%S", &tm) == 0) return "";
    return buf;
}
