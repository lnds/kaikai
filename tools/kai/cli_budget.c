/* Machine-wide compile-slot budget: a GNU make jobserver client under
   `make -j`, else flock'ed slot files (a dead process frees its slot). */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_HELD 256

typedef struct { int fd; char tok; } Held;

static Held held[MAX_HELD];
static int in_use[MAX_HELD];
static int jr = -2, jw = -2;
static int implicit_taken;
static int armed;

/* Read and write ends of the jobserver, or -1 when there is none. */
static int jobserver(void) {
    if (jr != -2) return jr >= 0;
    jr = jw = -1;
    const char *mf = getenv("MAKEFLAGS");
    const char *p = mf ? strstr(mf, "--jobserver-auth=") : NULL;
    if (!p) p = mf ? strstr(mf, "--jobserver-fds=") : NULL;
    if (!p) return 0;
    p = strchr(p, '=') + 1;
    if (strncmp(p, "fifo:", 5) == 0) {
        char path[1024];
        size_t n = strcspn(p + 5, " \t");
        if (n >= sizeof(path)) return 0;
        memcpy(path, p + 5, n);
        path[n] = 0;
        int f = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (f < 0) return 0;
        jr = jw = f;
        return 1;
    }
    int r, w;
    if (sscanf(p, "%d,%d", &r, &w) != 2 || r < 0 || w < 0) return 0;
    if (fcntl(r, F_GETFD) < 0 || fcntl(w, F_GETFD) < 0) return 0;
    jr = r;
    jw = w;
    return 1;
}

static void give_back(const Held *h) {
    if (h->fd < 0) {
        if (h->tok) {
            ssize_t w = write(jw, &h->tok, 1);
            (void) w;
        }
    } else {
        close(h->fd);
    }
}

static void release_all(void) {
    for (int i = 0; i < MAX_HELD; i++)
        if (in_use[i]) {
            in_use[i] = 0;
            give_back(&held[i]);
        }
}

static void on_signal(int sig) {
    release_all();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void arm(void) {
    if (armed) return;
    armed = 1;
    atexit(release_all);
    if (jr >= 0) {
        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);
        signal(SIGHUP, on_signal);
    }
}

static int64_t record(int fd, char tok) {
    for (int i = 0; i < MAX_HELD; i++)
        if (!in_use[i]) {
            in_use[i] = 1;
            held[i].fd = fd;
            held[i].tok = tok;
            arm();
            return i;
        }
    if (fd >= 0) close(fd);
    else if (tok) { ssize_t w = write(jw, &tok, 1); (void) w; }
    return -1;
}

static int64_t try_jobserver(void) {
    if (!implicit_taken) {
        implicit_taken = 1;
        return record(-1, 0);
    }
    struct pollfd pf = { jr, POLLIN, 0 };
    char c;
    if (poll(&pf, 1, 0) > 0 && read(jr, &c, 1) == 1) return record(-1, c);
    return -1;
}

static int64_t try_flock(const char *dir, int64_t n) {
    mkdir(dir, 0755);
    for (int64_t i = 0; i < n; i++) {
        char path[1100];
        snprintf(path, sizeof(path), "%s/slot-%lld", dir, (long long) i);
        int f = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (f < 0) continue;
        if (flock(f, LOCK_EX | LOCK_NB) == 0) return record(f, 0);
        close(f);
    }
    return -1;
}

/* A slot handle (>= 0), or -1 when none is free right now. */
int64_t kaicli_slot_try(const char *dir, int64_t n) {
    return jobserver() ? try_jobserver() : try_flock(dir, n);
}

/* Waits for a slot; call only when holding none. */
int64_t kaicli_slot_wait(const char *dir, int64_t n) {
    for (;;) {
        int64_t h = kaicli_slot_try(dir, n);
        if (h >= 0) return h;
        struct timespec ts = { 0, 25 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
}

void kaicli_slot_release(int64_t h) {
    if (h < 0 || h >= MAX_HELD || !in_use[h]) return;
    in_use[h] = 0;
    if (held[h].fd < 0 && held[h].tok == 0) implicit_taken = 0;
    give_back(&held[h]);
}
