/* POSIX calls the kai CLI needs that the runtime does not expose. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int64_t kaicli_mtime(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (int64_t) st.st_mtime : -1;
}

int64_t kaicli_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (int64_t) st.st_size : -1;
}

int kaicli_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int kaicli_is_exec(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && !S_ISDIR(st.st_mode) && access(path, X_OK) == 0;
}

static char **exec_argv;
static int exec_argc;

void kaicli_argv_push(const char *arg) {
    char **grown = realloc(exec_argv, (size_t) (exec_argc + 2) * sizeof(char *));
    if (!grown) abort();
    exec_argv = grown;
    exec_argv[exec_argc++] = strdup(arg);
    exec_argv[exec_argc] = NULL;
}

/* The runtime blocks signals and may ignore SIGPIPE; both survive execv,
   so restore the defaults the replacement program expects. Returns only
   on failure, with the reason. */
const char *kaicli_exec(const char *path) {
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);
    signal(SIGPIPE, SIG_DFL);
    execv(path, exec_argv);
    return strerror(errno);
}
