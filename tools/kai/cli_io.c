/* Byte-exact file plumbing, symlinks, the running executable and the wall clock for the kai CLI. */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

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

/* The target a symlink names, as written; empty when `path` is not a symlink. */
const char *kaicli_readlink(const char *path) {
    static char buf[4096];
    ssize_t n = readlink(path, buf, sizeof(buf) - 1);
    if (n < 0) return "";
    buf[n] = '\0';
    return buf;
}

/* The path the kernel ran this process from; empty when the OS cannot say. */
const char *kaicli_self_exe(void) {
    static char buf[4096];
#ifdef __APPLE__
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return "";
    return buf;
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n < 0) return "";
    buf[n] = '\0';
    return buf;
#endif
}

static int wasm_leb(const unsigned char *b, size_t end, size_t *i, uint32_t *v) {
    uint32_t r = 0;
    for (int shift = 0; shift < 35 && *i < end; shift += 7) {
        unsigned char c = b[(*i)++];
        r |= (uint32_t) (c & 0x7f) << shift;
        if (!(c & 0x80)) {
            *v = r;
            return 1;
        }
    }
    return 0;
}

static int wasm_name(const unsigned char *b, size_t end, size_t *i, const unsigned char **s, uint32_t *len) {
    if (!wasm_leb(b, end, i, len) || *len > end - *i) return 0;
    *s = b + *i;
    *i += *len;
    return 1;
}

static int wasm_limits(const unsigned char *b, size_t end, size_t *i) {
    uint32_t flags, v;
    if (!wasm_leb(b, end, i, &flags) || !wasm_leb(b, end, i, &v)) return 0;
    return (flags & 1) ? wasm_leb(b, end, i, &v) : 1;
}

/* One import's descriptor, skipped; 0 when the bytes do not parse. */
static int wasm_skip_desc(const unsigned char *b, size_t end, size_t *i) {
    uint32_t v;
    if (*i >= end) return 0;
    switch (b[(*i)++]) {
    case 0: return wasm_leb(b, end, i, &v);
    case 1: (*i)++; return wasm_limits(b, end, i);
    case 2: return wasm_limits(b, end, i);
    case 3: *i += 2; return *i <= end;
    case 4: (*i)++; return wasm_leb(b, end, i, &v);
    default: return 0;
    }
}

/* True when the import section of the wasm bytes `b` names module `mod`. */
static int wasm_imports_from(const unsigned char *b, size_t n, const char *mod) {
    size_t i = 8, want = strlen(mod);
    while (i < n) {
        unsigned char id = b[i++];
        uint32_t size;
        if (!wasm_leb(b, n, &i, &size) || size > n - i) return 0;
        size_t end = i + size;
        if (id == 2) {
            uint32_t count;
            if (!wasm_leb(b, end, &i, &count)) return 0;
            for (; count > 0; count--) {
                const unsigned char *s;
                uint32_t len;
                if (!wasm_name(b, end, &i, &s, &len)) return 0;
                if (len == want && memcmp(s, mod, len) == 0) return 1;
                if (!wasm_name(b, end, &i, &s, &len) || !wasm_skip_desc(b, end, &i)) return 0;
            }
            return 0;
        }
        i = end;
    }
    return 0;
}

/* True when the wasm module at `path` imports something from the module named `mod`. */
int kaicli_wasm_imports(const char *path, const char *mod) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = len > 8 ? malloc((size_t) len) : NULL;
    int found = b && fread(b, 1, (size_t) len, f) == (size_t) len && wasm_imports_from(b, (size_t) len, mod);
    free(b);
    fclose(f);
    return found;
}
