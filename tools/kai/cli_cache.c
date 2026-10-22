/* Recency and eviction of the shared build caches. */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* A hit younger than this is not worth a metadata write. */
#define REFRESH_AFTER 3600

int kaicli_refresh(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (time(NULL) - st.st_mtime >= REFRESH_AFTER)
        (void) utimensat(AT_FDCWD, path, NULL, 0);
    return 1;
}

static int64_t tree_total;

static int add_size(const char *p, const struct stat *st, int flag, struct FTW *ftw) {
    (void) p; (void) ftw;
    if (flag == FTW_F) tree_total += (int64_t) st->st_size;
    return 0;
}

int64_t kaicli_tree_bytes(const char *root) {
    tree_total = 0;
    nftw(root, add_size, 16, FTW_PHYS);
    return tree_total;
}

static int remove_entry(const char *p, const struct stat *st, int flag, struct FTW *ftw) {
    (void) st; (void) ftw;
    if (flag == FTW_DP) rmdir(p);
    else unlink(p);
    return 0;
}

static void remove_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) nftw(path, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
    else unlink(path);
}

static void trim_dir(const char *dir, int unit, time_t cutoff);

static void trim_entry(const char *p, int unit, time_t cutoff) {
    struct stat st;
    if (lstat(p, &st) != 0) return;
    int old = st.st_mtime < cutoff;
    if (unit == 1) {
        if (old) remove_tree(p);
    } else if (S_ISDIR(st.st_mode)) {
        trim_dir(p, unit == 0 ? 0 : unit - 1, cutoff);
        if (unit == 0 && old) rmdir(p);
    } else if (unit == 0 && old) {
        unlink(p);
    }
}

/* Entries are the files below `dir` when `unit` is 0, else whatever sits
   `unit` levels down (a directory goes whole). One older than `cutoff` is
   removed; a vanished path is someone else's doing and not an error. A
   directory emptied this way goes only if it was already old: a fresh one
   is a build's object dir, still waiting for its first object. */
static void trim_dir(const char *dir, int unit, time_t cutoff) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[PATH_MAX];
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) < (int) sizeof p)
            trim_entry(p, unit, cutoff);
    }
    closedir(d);
}

#define MAX_ROOTS 16
static char trim_roots[MAX_ROOTS][PATH_MAX];
static int trim_units[MAX_ROOTS];
static int trim_n;

void kaicli_trim_add(const char *root, int64_t unit) {
    if (trim_n >= MAX_ROOTS || strlen(root) >= PATH_MAX) return;
    strcpy(trim_roots[trim_n], root);
    trim_units[trim_n++] = (int) unit;
}

/* The trim runs in a detached, low-priority child so the build that
   triggered it never waits; the child holds no stdio of the caller. */
int kaicli_trim_spawn(int64_t max_age) {
    pid_t pid = fork();
    if (pid != 0) return pid > 0;
    setsid();
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
        dup2(null, 0);
        dup2(null, 1);
        dup2(null, 2);
        if (null > 2) close(null);
    }
    nice(10);
    time_t cutoff = time(NULL) - (time_t) max_age;
    for (int i = 0; i < trim_n; i++) trim_dir(trim_roots[i], trim_units[i], cutoff);
    _exit(0);
}
