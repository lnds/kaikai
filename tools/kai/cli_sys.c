/* POSIX calls the kai CLI needs that the runtime does not expose. */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

int64_t kaicli_mtime(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (int64_t) st.st_mtime : -1;
}

int64_t kaicli_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (int64_t) st.st_size : -1;
}

int kaicli_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int kaicli_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int kaicli_is_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int kaicli_is_exec(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && !S_ISDIR(st.st_mode) && access(path, X_OK) == 0;
}

/* ---- argv and per-child environment, consumed by the next spawn/exec ---- */

typedef struct { char **items; int len; } StrList;

static StrList exec_argv, child_set, child_unset;
static char *child_fd3;

static void list_push(StrList *l, const char *s) {
    char **grown = realloc(l->items, (size_t) (l->len + 2) * sizeof(char *));
    if (!grown) abort();
    l->items = grown;
    l->items[l->len++] = strdup(s);
    l->items[l->len] = NULL;
}

static void list_clear(StrList *l) {
    for (int i = 0; i < l->len; i++) free(l->items[i]);
    l->len = 0;
    if (l->items) l->items[0] = NULL;
}

void kaicli_argv_push(const char *arg) { list_push(&exec_argv, arg); }

/* Environment changes applied in the next child only. */
void kaicli_child_setenv(const char *name, const char *value) {
    list_push(&child_set, name);
    list_push(&child_set, value);
}

void kaicli_child_unsetenv(const char *name) { list_push(&child_unset, name); }

/* Open `path` for appending as the next child's fd 3 (the test-record stream). */
void kaicli_child_fd3(const char *path) {
    free(child_fd3);
    child_fd3 = strdup(path);
}

static void reset_signals(void) {
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);
    signal(SIGPIPE, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
}

/* The runtime blocks signals and may ignore SIGPIPE; both survive execv,
   so restore the defaults the replacement program expects. Returns only
   on failure, with the reason. */
const char *kaicli_exec(const char *path) {
    reset_signals();
    execv(path, exec_argv.items);
    return strerror(errno);
}

/* `&2` sends stdout to stderr, as `>&2` does. */
static int redirect(const char *path, int fd) {
    if (!path || !*path) return 0;
    if (strcmp(path, "&2") == 0) return dup2(2, fd) < 0 ? -1 : 0;
    int f = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f < 0 || dup2(f, fd) < 0) return -1;
    close(f);
    return 0;
}

/* The runtime reaps any child with waitpid(-1) whenever a fiber parks on
   I/O, so a status must never depend on our own waitpid winning that race.
   Each spawn goes through a forked middleman that waits for the real child
   and reports the status down a pipe; the handle is the pipe's read end. */

#define KAICLI_MAX_FD 4096
static pid_t middleman[KAICLI_MAX_FD];

static void run_child(const char *dir, const char *out, const char *err) {
    reset_signals();
    for (int i = 0; i + 1 < child_set.len; i += 2) setenv(child_set.items[i], child_set.items[i + 1], 1);
    for (int i = 0; i < child_unset.len; i++) unsetenv(child_unset.items[i]);
    if ((dir && *dir && chdir(dir) != 0) || redirect(out, 1) != 0 || redirect(err, 2) != 0) _exit(126);
    if (child_fd3) {
        int f = open(child_fd3, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (f < 0 || (f != 3 && (dup2(f, 3) < 0 || close(f) != 0))) _exit(126);
    }
    execvp(exec_argv.items[0], exec_argv.items);
    dprintf(2, "kai: %s: %s\n", exec_argv.items[0], strerror(errno));
    _exit(127);
}

/* Shell-style status: the exit code, or 128 + the signal that killed it. */
static int shell_status(int st) {
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return -1;
}

static void run_middleman(int report, const char *dir, const char *out, const char *err) {
    signal(SIGCHLD, SIG_DFL);
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    pid_t pid = fork();
    if (pid == 0) run_child(dir, out, err);
    int st = 0, code = -1;
    if (pid > 0) {
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        code = shell_status(st);
    }
    ssize_t w = write(report, &code, sizeof(code));
    (void) w;
    _exit(0);
}

/* Start the pushed argv (execvp) in `dir` with stdout/stderr sent to the
   named files (empty inherits). Returns a handle for kaicli_wait, or -1. */
int64_t kaicli_spawn(const char *dir, const char *out, const char *err) {
    int p[2];
    int64_t handle = -1;
    fflush(NULL);
    if (pipe(p) == 0) {
        fcntl(p[0], F_SETFD, FD_CLOEXEC);
        fcntl(p[1], F_SETFD, FD_CLOEXEC);
        pid_t pid = p[0] < KAICLI_MAX_FD ? fork() : -1;
        if (pid == 0) {
            close(p[0]);
            run_middleman(p[1], dir, out, err);
        }
        close(p[1]);
        if (pid > 0) {
            middleman[p[0]] = pid;
            handle = p[0];
        } else {
            close(p[0]);
        }
    }
    list_clear(&exec_argv);
    list_clear(&child_set);
    list_clear(&child_unset);
    free(child_fd3);
    child_fd3 = NULL;
    return handle;
}

/* The status of a spawned child: its exit code, or 128 + the signal that
   killed it; -1 when it could not be observed. */
int64_t kaicli_wait(int64_t handle) {
    int code = -1;
    ssize_t n;
    if (handle < 0 || handle >= KAICLI_MAX_FD) return -1;
    while ((n = read((int) handle, &code, sizeof(code))) < 0 && errno == EINTR) {}
    if (n != (ssize_t) sizeof(code)) code = -1;
    close((int) handle);
    while (waitpid(middleman[handle], NULL, 0) < 0 && errno == EINTR) {}
    return code;
}

/* ---- interrupts: a Ctrl-C lands in the foreground child; kai outlives it
   to clean up, then exits 128 + the signal ---- */

static volatile sig_atomic_t pending_signal;

static void note_signal(int sig) { pending_signal = sig; }

void kaicli_defer_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = note_signal;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGINT);
    sigaddset(&s, SIGTERM);
    sigaddset(&s, SIGHUP);
    sigprocmask(SIG_UNBLOCK, &s, NULL);
}

int64_t kaicli_pending_signal(void) { return pending_signal; }

/* ---- filesystem ---- */

/* A fresh private directory from a mkdtemp template; empty on failure. */
const char *kaicli_mkdtemp(const char *tmpl) {
    static char buf[4096];
    if (snprintf(buf, sizeof(buf), "%s", tmpl) >= (int) sizeof(buf)) return "";
    return mkdtemp(buf) ? buf : "";
}

static int remove_entry(const char *p, const struct stat *st, int flag, struct FTW *ftw) {
    (void) st; (void) ftw;
    return flag == FTW_DP ? rmdir(p) : unlink(p);
}

void kaicli_remove_tree(const char *path) {
    nftw(path, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

/* `cat path >&2`: the file's exact bytes on stderr. */
void kaicli_copy_to_stderr(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char buf[65536];
    size_t n;
    fflush(stdout);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) fwrite(buf, 1, n, stderr);
    fflush(stderr);
    fclose(f);
}

/* Set once kaic2 proves it lacks libLLVM: later builds of this run skip the native attempt. */
static int no_llvm;

void kaicli_mark_no_llvm(void) { no_llvm = 1; }

int kaicli_no_llvm(void) { return no_llvm; }

/* `cat path`: the file's exact bytes on stdout. */
void kaicli_copy_to_stdout(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char buf[65536];
    size_t n;
    fflush(stdout);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) fwrite(buf, 1, n, stdout);
    fflush(stdout);
    fclose(f);
}

/* The working directory the invoking shell reports: $PWD when it still
   names the current directory, else the physical path. */
const char *kaicli_cwd(void) {
    static char buf[4096];
    const char *pwd = getenv("PWD");
    struct stat a, b;
    if (pwd && pwd[0] == '/' && stat(pwd, &a) == 0 && stat(".", &b) == 0
        && a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
        return pwd;
    }
    return getcwd(buf, sizeof(buf)) ? buf : ".";
}

/* ---- host ---- */

void kaicli_raise_stack(int64_t bytes) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) != 0) return;
    rlim_t want = (rlim_t) bytes;
    if (rl.rlim_max != RLIM_INFINITY && want > rl.rlim_max) return;
    rl.rlim_cur = want;
    setrlimit(RLIMIT_STACK, &rl);
}

const char *kaicli_uname_s(void) {
    static struct utsname u;
    return uname(&u) == 0 ? u.sysname : "";
}

const char *kaicli_uname_m(void) {
    static struct utsname u;
    return uname(&u) == 0 ? u.machine : "";
}

void kaicli_sleep(int64_t secs) { sleep((unsigned) secs); }

int64_t kaicli_getpid(void) { return (int64_t) getpid(); }

int64_t kaicli_ncpu(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? n : 4;
}

/* ---- SHA-256 over a byte stream fed in pieces (FIPS 180-4) ---- */

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t sha_h[8];
static unsigned char sha_block[64];
static size_t sha_fill;
static uint64_t sha_bits;

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_compress(const unsigned char *p) {
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t) p[4 * i] << 24 | (uint32_t) p[4 * i + 1] << 16 | (uint32_t) p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = sha_h[0]; b = sha_h[1]; c = sha_h[2]; d = sha_h[3];
    e = sha_h[4]; f = sha_h[5]; g = sha_h[6]; h = sha_h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    sha_h[0] += a; sha_h[1] += b; sha_h[2] += c; sha_h[3] += d;
    sha_h[4] += e; sha_h[5] += f; sha_h[6] += g; sha_h[7] += h;
}

static void sha_feed(const unsigned char *p, size_t n) {
    sha_bits += (uint64_t) n * 8;
    while (n > 0) {
        size_t take = 64 - sha_fill < n ? 64 - sha_fill : n;
        memcpy(sha_block + sha_fill, p, take);
        sha_fill += take; p += take; n -= take;
        if (sha_fill == 64) { sha_compress(sha_block); sha_fill = 0; }
    }
}

void kaicli_sha_reset(void) {
    static const uint32_t init[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    memcpy(sha_h, init, sizeof(init));
    sha_fill = 0;
    sha_bits = 0;
}

void kaicli_sha_str(const char *s) { sha_feed((const unsigned char *) s, strlen(s)); }

/* Feeds the file's bytes; false when it cannot be read. */
int kaicli_sha_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) sha_feed(buf, n);
    int ok = !ferror(f);
    fclose(f);
    return ok;
}

const char *kaicli_sha_hex(void) {
    static char hex[65];
    uint64_t bits = sha_bits;
    unsigned char pad = 0x80, zero = 0, len[8];
    sha_feed(&pad, 1);
    while (sha_fill != 56) sha_feed(&zero, 1);
    for (int i = 0; i < 8; i++) len[i] = (unsigned char) (bits >> (56 - 8 * i));
    sha_feed(len, 8);
    for (int i = 0; i < 8; i++) snprintf(hex + 8 * i, 9, "%08x", sha_h[i]);
    return hex;
}
