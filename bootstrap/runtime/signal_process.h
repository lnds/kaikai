/* Fragment of runtime.h; include runtime.h, never this file. */
/* =================================================================
 * Signal effect — issue #107. POSIX SIGINT/SIGTERM/SIGHUP/SIGUSR1/
 * SIGUSR2 trap for graceful shutdown.
 *
 * Shape (v1, on/off/await):
 *   on(sig)  : Unit  -- block sig at the process level, mark subscribed
 *   off(sig) : Unit  -- unblock sig, drop from subscribed set
 *   await()  : Sig   -- sigwait on the subscribed set, return arrived
 *
 * Why sigwait and not an async sa_handler:
 *   The async path is restricted to async-signal-safe calls; building a
 *   KaiValue variant inside the handler is not. The handler would have
 *   to set a pending bit and let the next yield-point drain it — that
 *   matches the BEAM-faithful `on_cancel(sig)` shape sketched in the
 *   issue, which v1 cannot honour cleanly: `kai_default_cancel_raise`
 *   exits the program when main_fiber has no cancel_pad, so a runtime-
 *   triggered Cancel never runs the user's `with Cancel { raise(_) ->
 *   cleanup }`. The on/off/await shape sidesteps the async problem
 *   entirely: signals are blocked via sigprocmask, queued by the
 *   kernel, and synchronously dequeued by sigwait inside `await()`
 *   where the full runtime is available.
 *
 * v1 limitations (mirrored in docs/effects-stdlib.md §Signal):
 *   - Posix only. Windows / WASM are out of scope.
 *   - `await()` blocks the OS thread. Other fibers cannot run while
 *     it is parked. Acceptable for the v1 use case (main parks on
 *     Signal after spawning workers).
 *   - SIGCHLD intentionally absent — Process.wait reaps children.
 *   - Real-time signals (SIGRTMIN+n) and siginfo_t are out of scope.
 */

#if KAI_HOST_SIGNALS
typedef struct {
    int         signo;
    const char *name;
    int32_t     tag;       /* atom-style variant tag, docs/variant-tags.md */
} KaiSignalEntry;

static const KaiSignalEntry kai_signal_entries[] = {
    { SIGINT,  "SigInt",  4 },
    { SIGTERM, "SigTerm", 5 },
    { SIGHUP,  "SigHup",  6 },
    { SIGUSR1, "SigUsr1", 7 },
    { SIGUSR2, "SigUsr2", 8 },
    { SIGWINCH, "SigWinch", 11 },
    { 0,       NULL,      0 }
};

static int _kai_signal_from_variant(KaiValue *sig_v) {
    if (!sig_v || sig_v->tag != KAI_VARIANT) return 0;
    const char *n = kai_variant_name_of(sig_v->variant_tag);
    if (!n) return 0;
    for (const KaiSignalEntry *e = kai_signal_entries; e->name; ++e) {
        if (strcmp(e->name, n) == 0) return e->signo;
    }
    return 0;
}

static KaiValue *_kai_signal_to_variant(int signo) {
    for (const KaiSignalEntry *e = kai_signal_entries; e->name; ++e) {
        if (e->signo == signo) {
            return kai_variant_u(e->tag, e->name, 0, 0, NULL);
        }
    }
    return NULL;
}

/* Subscribed-signal set for the Signal effect: one module subscribes, a
 * handler in another tests membership, so it is shared process state (owner
 * defines, others extern) — a per-TU set would split the subscription. */
#if defined(KAI_SEPARATE_COMPILATION)
extern sigset_t kai_signal_subscribed;
extern int      kai_signal_subscribed_init;
#  if defined(KAI_RUNTIME_OWNER)
sigset_t kai_signal_subscribed;
int      kai_signal_subscribed_init = 0;
#  endif
#else
static sigset_t kai_signal_subscribed;
static int      kai_signal_subscribed_init = 0;
#endif

static void _kai_signal_init_subscribed(void) {
    if (!kai_signal_subscribed_init) {
        sigemptyset(&kai_signal_subscribed);
        kai_signal_subscribed_init = 1;
    }
}

/* Issue #671 — Phase R4: install an async-signal-safe sa_handler
 * for `signo` that writes the signo byte into the reactor's self-
 * pipe. Replaces v1's sigprocmask-only block, which relied on
 * sigwait() inside `signal_await` to dequeue. Under R4 the kernel
 * delivers the signal asynchronously to our handler (write(2) is
 * async-signal-safe), and the scheduler's poll() loop reads the
 * byte and wakes the parked fiber. */
static void _kai_signal_install_handler(int signo) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = kai_reactor_signal_handler;
    sigemptyset(&sa.sa_mask);
    /* SA_RESTART so a signal arriving during a syscall in another
     * thread does not abort that syscall — only the scheduler
     * thread's poll() needs to wake, and poll() returns -1/EINTR
     * cleanly on its own. The reactor already runs without
     * SA_RESTART on SIGCHLD because the wake byte is its own
     * notification; here SA_RESTART is fine because the wake byte
     * IS the signal payload. */
    sa.sa_flags = SA_RESTART;
    sigaction(signo, &sa, NULL);
}

static void _kai_signal_uninstall_handler(int signo) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(signo, &sa, NULL);
}

static KaiValue *kai_default_signal_on(void *self, KaiValue *sig_v, KaiCont *k) {
    (void) self;
    _kai_signal_init_subscribed();
    int signo = _kai_signal_from_variant(sig_v);
    if (signo > 0) {
        sigaddset(&kai_signal_subscribed, signo);
        /* Make sure the signal is NOT blocked at the process level —
         * R4 wants the handler to fire, not for the kernel to queue
         * the signal until sigwait() pulls it. Idempotent. */
        sigset_t one;
        sigemptyset(&one);
        sigaddset(&one, signo);
        sigprocmask(SIG_UNBLOCK, &one, NULL);
        _kai_signal_install_handler(signo);
        /* Ensure the reactor self-pipe exists before any handler can
         * fire — kai_reactor_init is idempotent so paying the call
         * here costs nothing if the reactor is already up. */
        kai_reactor_init();
    }
    return kai_cont_resume(k, kai_unit());
}

/* off(sig): restore the default disposition. A signal that arrives
 * after `off` but before the SIG_DFL takes effect is best-effort —
 * sigaction is atomic w.r.t. the calling thread but not w.r.t.
 * concurrent signal delivery, the kernel may have already queued
 * one byte to the self-pipe that the next `signal_await` (if any)
 * will consume. Documented as a sharp edge in
 * docs/effects-stdlib.md §Signal. */
static KaiValue *kai_default_signal_off(void *self, KaiValue *sig_v, KaiCont *k) {
    (void) self;
    _kai_signal_init_subscribed();
    int signo = _kai_signal_from_variant(sig_v);
    if (signo > 0) {
        sigdelset(&kai_signal_subscribed, signo);
        _kai_signal_uninstall_handler(signo);
    }
    return kai_cont_resume(k, kai_unit());
}

/* await(): empty subscribed set adds SIGINT defensively so Ctrl-C
 * still wakes the caller. R4 path: park on the singleton signal
 * waiter slot, yield to the scheduler, and resume when the reactor
 * drains the self-pipe. The signo arrives in reactor_wait_status. */
KAI_SCHED_FN KaiValue *kai_default_signal_await(void *self, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
    _kai_signal_init_subscribed();
    /* Defensive SIGINT subscription if nothing has been on()'d —
     * mirrors the v1 behaviour so existing code that does
     * `Signal.await()` directly without `Signal.on(SigInt)` still
     * traps Ctrl-C. */
    int any = 0;
    for (const KaiSignalEntry *e = kai_signal_entries; e->name; ++e) {
        if (sigismember(&kai_signal_subscribed, e->signo)) { any = 1; break; }
    }
    if (!any) {
        sigaddset(&kai_signal_subscribed, SIGINT);
        sigset_t one;
        sigemptyset(&one);
        sigaddset(&one, SIGINT);
        sigprocmask(SIG_UNBLOCK, &one, NULL);
        _kai_signal_install_handler(SIGINT);
        kai_reactor_init();
    }
    for (;;) {
        KaiFiber *me = kai_current_fiber();
        if (kai_reactor_park_signal(me) != 0) {
            /* v1 contract: only one fiber may sit in Signal.await()
             * at a time. The byte in the self-pipe carries no
             * identity, so two concurrent waiters would race over
             * who picks it up. Mirrors the R3 stdin-multiplex
             * panic. */
            kai_core_panic(kai_str(
                "signal_await: a fiber is already awaiting a signal — "
                "concurrent Signal.await() is undefined; "
                "serialize via an actor or supervisor"));
        }
        /* Drain set f->reactor_wait_status before unparking. A
         * spurious wake (signo == 0) loops back and re-parks. */
        int signo = me->reactor_wait_status;
        me->reactor_wait_status = 0;
        if (signo > 0) {
            KaiValue *v = _kai_signal_to_variant(signo);
            if (v) return kai_cont_resume(k, v);
        }
    }
}
#endif
#else
static KaiValue *kai_default_signal_on(void *self, KaiValue *sig_v, KaiCont *k) { kai_host_absent("signals"); }
static KaiValue *kai_default_signal_off(void *self, KaiValue *sig_v, KaiCont *k) { kai_host_absent("signals"); }
KAI_SCHED_FN KaiValue *kai_default_signal_await(void *self, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{ kai_host_absent("signals"); }
#endif
#endif

/* =================================================================
 * Process effect — issue #126. POSIX subprocess primitives.
 *
 * Shape (v1):
 *   start(cmd, args)  : Child                       -- fork + execvp
 *   wait(c)           : Result[Exit, String]        -- waitpid blocking
 *   kill(c, sig: Int) : Result[Unit, String]        -- kill(2)
 *   exit(code)        : Nothing                     -- _exit(2)
 *
 * `Child = { pid: Int }` and `Exit = Exited(Int) | Signaled(Int)`
 * are declared by the compiler's `builtin_child_decl` /
 * `builtin_exit_decl` (stage2/compiler.kai) and minted here by
 * name, same convention as Conn / Listener for NetTcp.
 *
 * Divergences from docs/effects-stdlib.md §Process pinned in the
 * compiler's `builtin_process_decl` comment:
 *   1. start returns Child, not Result[Child, String]. fork/exec
 *      failure surfaces through `kai_panic`, matching the
 *      "primitive failure" convention used elsewhere in the runtime.
 *   2. kill takes a raw signo Int rather than the issue #107 Sig
 *      sum type — kill needs the full POSIX signal set including
 *      SIGKILL, which Sig deliberately excludes.
 *
 * v1 limitations (mirrored in docs/effects-stdlib.md §Process
 * *What's not in v1*):
 *   - POSIX only. Windows / WASM out of scope.
 *   - All ops are blocking: `Process.wait` parks the OS thread
 *     inside `waitpid` rather than the calling fiber. The m8.x
 *     cooperative scheduler (landed v0.4.0) provides the suspend /
 *     resume primitives, but reactor-driven cancellation-aware
 *     waiting (`wait_or_kill`) still needs a SIGCHLD-aware reactor
 *     plug that registers the pid and wakes the fiber when the
 *     child terminates. Tier 2 follow-up tracked in
 *     docs/fibers-honesty-targets.md §Reactor.
 *   - Stdio plumbing is popen-shaped: `start_piped` attaches pipes
 *     to any of the child's stdin/stdout/stderr; `write_stdin` /
 *     `read_stdout` / `read_stderr` / `close_stdin` drive them; `wait`
 *     closes whatever is still open before reaping (pclose semantics).
 *   - SIGCHLD handling is implicit through blocking waitpid; the
 *     Signal effect intentionally omits SIGCHLD per its catalog
 *     comment so the two effects don't fight over the disposition.
 */

#if KAI_HOST_PROCESS
/* Build a Child record `{ pid: Int }`. The "pid" field name is
 * load-bearing — `_kai_process_record_pid` reads the slot by
 * strcmp on the name pointer's contents, in lockstep with
 * `builtin_child_decl` in stage2/compiler.kai. */
static KaiValue *_kai_process_make_child(int pid) {
    KaiValue *pid_kv = kai_int((int64_t) pid);
    KaiValue *fields[1] = { pid_kv };
    static const char *names[1] = { "pid" };
    return kai_record(1, fields, names);
}

/* Pull the `pid` slot out of a Child record. Returns -1 on shape
 * mismatch — the caller surfaces that as an error path; v1 trusts
 * the typer to keep this honest in normal flows. */
static int _kai_process_record_pid(KaiValue *v) {
    if (!v || v->tag != KAI_RECORD) return -1;
    for (int i = 0; i < v->as.rec.n_fields; ++i) {
        if (v->as.rec.names[i] && strcmp(v->as.rec.names[i], "pid") == 0) {
            KaiValue *f = v->as.rec.fields[i];
            if (!kai_is_int(f)) return -1;
            return (int) kai_intf(f);
        }
    }
    return -1;
}

/* Mint Exited(code) / Signaled(signo) — variant *names* are the
 * runtime contract with `builtin_exit_decl`. The typer-declared
 * shape is `Exited(Int)` / `Signaled(Int)`. The emitter's slot
 * representation for a variant Int field has flipped twice; this
 * runtime constructor must track whichever convention the emitter
 * reads, because the cell it mints is matched by emitter-generated
 * code:
 *   - #440 Phase 2: raw scalar in `.i64`, slot kind KAI_VAR_SLOT_INT.
 *   - #741: re-boxed to a tagged pointer (`{.ptr = kai_int(n)}`,
 *     slot_mask 0) when `variant_slot_kind` reported Int as a pointer
 *     kind and the emitter bound every slot via `.ptr`.
 *   - i64-inline Lane B (commit 1f4f66f): back to raw int64 in `.i64`
 *     with slot kind KAI_VAR_SLOT_INT. `variant_slot_kind(Int) == 1`,
 *     so the emitter constructs `Exited` as
 *     `kai_variant_u_fast(9, 1, {{.i64 = code}})` (mask 1) and the
 *     match reads `kai_var_slots(_scr)[0].i64` raw. With the old boxed
 *     mask-0 cell the match read the pointer bits as the exit code, so
 *     every `Exited(0)` looked non-zero (process_basic #741 went red).
 * Mirror the emitter exactly: raw `.i64`, mask KAI_VAR_SLOT_INT. The
 * mask also keeps the generic drop walker from decref-ing a raw int
 * as if it were a pointer. */
static KaiValue *_kai_process_make_exit_exited(int code) {
    KaiVarSlot s; s.i64 = (int64_t) code;
    return kai_variant_u(9, "Exited", 1, KAI_VAR_SLOT_INT, &s);
}

static KaiValue *_kai_process_make_exit_signaled(int signo) {
    KaiVarSlot s; s.i64 = (int64_t) signo;
    return kai_variant_u(10, "Signaled", 1, KAI_VAR_SLOT_INT, &s);
}

static KaiValue *_kai_process_err_msg(KaiCont *k, const char *msg) {
    KaiValue *m = kai_str(msg);
    KaiValue *err = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = m}});
    return kai_cont_resume(k, err);
}

static KaiValue *_kai_process_err(KaiCont *k, int saved_errno) {
    const char *msg = strerror(saved_errno);
    return _kai_process_err_msg(k, msg ? msg : "unknown error");
}

static KaiValue *_kai_process_ok(KaiCont *k, KaiValue *payload) {
    KaiValue *ok = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = payload}});
    return kai_cont_resume(k, ok);
}

/* Walk a [String] cons list once to count entries, then again to
 * copy the C strings into a NULL-terminated argv vector. The
 * vector and its element copies are owned by the caller and freed
 * after execvp returns / does not return. argv[0] is `cmd`; the
 * supplied `args` list fills argv[1..n]. */
static char **_kai_process_build_argv(const char *cmd, KaiValue *args, int *out_n) {
    int n = 1;  /* slot 0 is cmd */
    for (KaiValue *p = args; p && p->tag == KAI_CONS; p = p->as.cons.tail) ++n;
    char **argv = (char **) malloc(((size_t) n + 1) * sizeof(char *));
    if (!argv) return NULL;
    argv[0] = strdup(cmd ? cmd : "");
    int i = 1;
    for (KaiValue *p = args; p && p->tag == KAI_CONS; p = p->as.cons.tail) {
        KaiValue *h = p->as.cons.head;
        if (kai_is_ptr(h) && h->tag == KAI_STR) {
            argv[i] = strdup(h->as.s.bytes ? h->as.s.bytes : "");
        } else {
            argv[i] = strdup("");
        }
        ++i;
    }
    argv[n] = NULL;
    *out_n = n;
    return argv;
}

static void _kai_process_free_argv(char **argv, int n) {
    if (!argv) return;
    for (int i = 0; i < n; ++i) free(argv[i]);
    free(argv);
}

/* Child-side, between fork and exec: reset the signal mask and the
 * SIGPIPE disposition. Both survive execvp, and the runtime's own
 * mask manipulation must not leak into the exec'd image — a child
 * pipeline whose SIGPIPE is blocked reports EPIPE noise on stderr
 * instead of dying silently when its reader closes (GNU `yes`
 * observably differs between Linux CI and macOS without this).
 * Async-signal-safe callees only. */
static void _kai_process_child_reset_signals(void) {
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(SIGPIPE, &dfl, NULL);
}

/* Parent-side pipe ends of a piped child, keyed by pid. `start_piped`
 * registers, the stdio ops look up, and `wait` closes whatever is
 * still open before reaping (pclose semantics) — an unclosed parent
 * end would keep the child from ever seeing EOF. Shared process
 * state for the same reason as kai_signal_subscribed: the ops may
 * run from different TUs under separate compilation.
 *
 * A group leader's entry outlives `wait`: its group can outlive the
 * leader, and `kill_group` must still reach it after the reap. */
typedef struct KaiProcPipe {
    int pid;
    int in_fd;   /* parent writes the child's stdin; -1 = not piped / closed */
    int out_fd;  /* parent reads the child's stdout; -1 = not piped */
    int err_fd;  /* parent reads the child's stderr; -1 = not piped */
    int own_group;
    struct KaiProcPipe *next;
} KaiProcPipe;

#if defined(KAI_SEPARATE_COMPILATION)
extern KaiProcPipe    *kai_proc_pipes;
extern pthread_mutex_t kai_proc_pipes_mu;
#  if defined(KAI_RUNTIME_OWNER)
KaiProcPipe    *kai_proc_pipes = NULL;
pthread_mutex_t kai_proc_pipes_mu = PTHREAD_MUTEX_INITIALIZER;
#  endif
#else
static KaiProcPipe    *kai_proc_pipes = NULL;
static pthread_mutex_t kai_proc_pipes_mu = PTHREAD_MUTEX_INITIALIZER;
#endif

/* Unlink `pid`'s entry, caller holds the lock. */
static KaiProcPipe *_kai_proc_pipe_unlink(int pid) {
    KaiProcPipe **p = &kai_proc_pipes;
    while (*p && (*p)->pid != pid) p = &(*p)->next;
    KaiProcPipe *e = *p;
    if (e) *p = e->next;
    return e;
}

/* A fresh child whose pid matches a stale entry reuses a reaped
 * leader's pid; the stale entry (fds already closed) must not make
 * the new child look like a group leader. */
static void _kai_proc_pipe_forget(int pid) {
    pthread_mutex_lock(&kai_proc_pipes_mu);
    KaiProcPipe *e = _kai_proc_pipe_unlink(pid);
    pthread_mutex_unlock(&kai_proc_pipes_mu);
    free(e);
}

static void _kai_proc_pipe_add(int pid, int in_fd, int out_fd, int err_fd, int own_group) {
    KaiProcPipe *e = (KaiProcPipe *) malloc(sizeof(KaiProcPipe));
    if (!e) { kai_eputs("kai: out of memory\n"); kai_exit(1); }
    e->pid = pid; e->in_fd = in_fd; e->out_fd = out_fd; e->err_fd = err_fd;
    e->own_group = own_group;
    pthread_mutex_lock(&kai_proc_pipes_mu);
    free(_kai_proc_pipe_unlink(pid));
    e->next = kai_proc_pipes;
    kai_proc_pipes = e;
    pthread_mutex_unlock(&kai_proc_pipes_mu);
}

static int _kai_proc_pipe_own_group(int pid) {
    int own = 0;
    pthread_mutex_lock(&kai_proc_pipes_mu);
    for (KaiProcPipe *e = kai_proc_pipes; e; e = e->next) {
        if (e->pid == pid) { own = e->own_group; break; }
    }
    pthread_mutex_unlock(&kai_proc_pipes_mu);
    return own;
}

/* Fetch one end (0 = in_fd, 1 = out_fd, 2 = err_fd) under the lock.
 * The blocking IO itself runs unlocked so a parked write cannot stall
 * unrelated process ops. */
static int _kai_proc_pipe_fd(int pid, int which) {
    int fd = -1;
    pthread_mutex_lock(&kai_proc_pipes_mu);
    for (KaiProcPipe *e = kai_proc_pipes; e; e = e->next) {
        if (e->pid == pid) {
            fd = which == 0 ? e->in_fd : which == 1 ? e->out_fd : e->err_fd;
            break;
        }
    }
    pthread_mutex_unlock(&kai_proc_pipes_mu);
    return fd;
}

static void _kai_proc_pipe_close_stdin(int pid) {
    pthread_mutex_lock(&kai_proc_pipes_mu);
    for (KaiProcPipe *e = kai_proc_pipes; e; e = e->next) {
        if (e->pid == pid) {
            if (e->in_fd >= 0) { close(e->in_fd); e->in_fd = -1; }
            break;
        }
    }
    pthread_mutex_unlock(&kai_proc_pipes_mu);
}

static void _kai_proc_pipe_drop(int pid) {
    int fds[3] = { -1, -1, -1 };
    pthread_mutex_lock(&kai_proc_pipes_mu);
    KaiProcPipe *e = kai_proc_pipes;
    while (e && e->pid != pid) e = e->next;
    if (e) {
        fds[0] = e->in_fd; fds[1] = e->out_fd; fds[2] = e->err_fd;
        e->in_fd = e->out_fd = e->err_fd = -1;
        if (!e->own_group) e = _kai_proc_pipe_unlink(pid);
        else e = NULL;
    }
    pthread_mutex_unlock(&kai_proc_pipes_mu);
    for (int i = 0; i < 3; ++i) if (fds[i] >= 0) close(fds[i]);
    free(e);
}

/* One write attempt on the non-blocking stdin pipe; returns write(2)'s
 * result with errno intact. A dead reader must surface as EPIPE, not
 * kill the process: where the OS has a per-fd opt-out (F_SETNOSIGPIPE)
 * start_piped already set it; else SIGPIPE is blocked and drained around
 * this one syscall only. The mask is per-thread and a parked fiber may
 * resume on another OS thread, so it must never span a park. */
#if !KAI_SCHED_DECL_ONLY
static ssize_t _kai_proc_write_once(int fd, const char *buf, size_t len) {
#if defined(F_SETNOSIGPIPE)
    return write(fd, buf, len);
#else
    sigset_t pipe_set, prev_set;
    sigemptyset(&pipe_set);
    sigaddset(&pipe_set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe_set, &prev_set);
    ssize_t w = write(fd, buf, len);
    int e = errno;
    if (w < 0 && e == EPIPE) {
        struct timespec zero = { 0, 0 };
        while (sigtimedwait(&pipe_set, NULL, &zero) >= 0) {}
    }
    pthread_sigmask(SIG_SETMASK, &prev_set, NULL);
    errno = e;
    return w;
#endif
}
#endif

/* start(cmd, args) -> Child. fork + execvp; on failure of either
 * primitive, panic with strerror — start has no Result wrapper in
 * v1 (see the divergence note in builtin_process_decl).
 *
 * Issue #611 — install the SIGCHLD handler before fork() so that
 * a fast-exiting child cannot deliver SIGCHLD to the kernel's
 * default disposition (zombie reaper only) before the reactor is
 * armed. Idempotent. */
static KaiValue *kai_default_process_start(void *self, KaiValue *cmd, KaiValue *args, KaiCont *k) {
    (void) self;
    if (!cmd || cmd->tag != KAI_STR) {
        kai_eputs("kai: Process.start: cmd must be a String\n");
        kai_exit(1);
    }
    kai_reactor_init();
    /* cmd is heap-allocated by kai_str_from_bytes with a trailing
     * NUL, so passing bytes directly to execvp is safe. */
    const char *cmd_cstr = cmd->as.s.bytes ? cmd->as.s.bytes : "";
    int argc = 0;
    char **argv = _kai_process_build_argv(cmd_cstr, args, &argc);
    if (!argv) {
        kai_eputs("kai: Process.start: out of memory building argv\n");
        kai_exit(1);
    }

    pid_t pid = fork();
    if (pid < 0) {
        int e = errno;
        _kai_process_free_argv(argv, argc);
        kai_eprintf("kai: Process.start: fork: %s\n", strerror(e));
        kai_exit(1);
    }
    if (pid == 0) {
        /* Child: replace image. execvp searches PATH for relative
         * names; absolute paths bypass the search. On any failure
         * exit 127, the shell convention for "command not found". */
        _kai_process_child_reset_signals();
        execvp(cmd_cstr, argv);
        /* execvp only returns on failure. Async-signal-safe writers
         * only — keep the message minimal. */
        const char *prefix = "kai: Process.start: execvp: ";
        const char *msg    = strerror(errno);
        ssize_t w;
        w = write(2, prefix, strlen(prefix)); (void) w;
        if (msg) { w = write(2, msg, strlen(msg)); (void) w; }
        w = write(2, "\n", 1); (void) w;
        _exit(127);
    }
    /* Parent: clean up argv copies and resume with the Child
     * handle. The Child borrows nothing from argv — it carries
     * just the pid. */
    _kai_process_free_argv(argv, argc);
    _kai_proc_pipe_forget((int) pid);
    KaiValue *child = _kai_process_make_child((int) pid);
    return kai_cont_resume(k, child);
}

/* wait(c) -> Result[Exit, String]. Issue #611 Phase R1: the wait
 * parks the calling fiber on the reactor's pid waiter map and the
 * SIGCHLD self-pipe drives the wake. WIFEXITED → Exited(status);
 * WIFSIGNALED → Signaled(signo). The reactor's waitpid drain uses
 * `-1` to harvest any pending child, so a SIGCHLD that fires while
 * the parking fiber is mid-park is handled by the drain helper at
 * the next reactor wait (or immediately if the byte was already
 * pending in the self-pipe). */
KAI_SCHED_FN KaiValue *kai_default_process_wait(void *self, KaiValue *child, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
    int pid = _kai_process_record_pid(child);
    if (pid <= 0) {
        return _kai_process_err_msg(k, "wait: invalid Child");
    }
    /* pclose semantics: EOF the child's stdin and release every parent
     * end before reaping — a still-open write end would deadlock a
     * child that reads stdin to exhaustion. Drain stdout BEFORE wait;
     * closing the read end here means an undrained child still writing
     * takes EPIPE instead of blocking forever. */
    _kai_proc_pipe_drop(pid);
    kai_reactor_init();
    /* Race: the child may have terminated before we registered the
     * waiter — reaped either by us here (non-blocking waitpid) or by
     * the reactor drain's waitpid(-1), in which case the status sits
     * in the pending-exits buffer. Check the buffer on both sides of
     * our own waitpid: the drain can win between the two calls. */
    int status = 0;
    if (!kai_reactor_take_child_exit(pid, &status)) {
        pid_t rc = waitpid((pid_t) pid, &status, WNOHANG);
        if (rc == 0) {
            /* Child still running; park on the pid map and let the
             * SIGCHLD drain wake us with the status. Re-park on a spurious
             * resume — only the drain's own splice carries a real status. */
            KaiFiber *me = kai_current_fiber();
            do {
                kai_reactor_park_pid(me, pid);
            } while (!me->reactor_fired);
            status = me->reactor_wait_status;
            me->reactor_wait_pid    = 0;
            me->reactor_wait_status = 0;
        } else if (rc < 0) {
            int e = errno;
            if (!kai_reactor_take_child_exit(pid, &status)) {
                return _kai_process_err(k, e);
            }
        }
    }
    KaiValue *exit_v;
    if (WIFEXITED(status)) {
        exit_v = _kai_process_make_exit_exited(WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        exit_v = _kai_process_make_exit_signaled(WTERMSIG(status));
    } else {
        exit_v = _kai_process_make_exit_exited(-1);
    }
    return _kai_process_ok(k, exit_v);
}
#endif

/* kill(c, sig) -> Result[Unit, String]. Maps directly to kill(2);
 * sig is taken as a raw signo Int (full POSIX set including
 * SIGKILL — see the builtin_process_decl divergence note). */
static KaiValue *kai_default_process_kill(void *self, KaiValue *child, KaiValue *sig, KaiCont *k) {
    (void) self;
    int pid = _kai_process_record_pid(child);
    if (pid <= 0) {
        return _kai_process_err_msg(k, "kill: invalid Child");
    }
    int signo = (kai_is_int(sig)) ? (int) kai_intf(sig) : 0;
    if (kill((pid_t) pid, signo) < 0) {
        return _kai_process_err(k, errno);
    }
    return _kai_process_ok(k, kai_unit());
}

/* kill_group(c, sig) -> Result[Unit, String]. kill(-pgid, sig) on a
 * child started by `start_group`. Invariant: never the caller's own
 * group — a child without its own group is Err, not a fallback. */
static KaiValue *kai_default_process_kill_group(void *self, KaiValue *child, KaiValue *sig, KaiCont *k) {
    (void) self;
    int pid = _kai_process_record_pid(child);
    if (pid <= 1) {
        return _kai_process_err_msg(k, "kill_group: invalid Child");
    }
    if (!_kai_proc_pipe_own_group(pid) || (pid_t) pid == getpgrp()) {
        return _kai_process_err_msg(k, "kill_group: child has no process group of its own");
    }
    int signo = (kai_is_int(sig)) ? (int) kai_intf(sig) : 0;
    if (kill(-(pid_t) pid, signo) < 0) {
        return _kai_process_err(k, errno);
    }
    return _kai_process_ok(k, kai_unit());
}

/* uid() -> Int. The effective uid: what a peer reads of this process
 * through SO_PEERCRED / LOCAL_PEERCRED. */
static KaiValue *kai_default_process_uid(void *self, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_int((int64_t) geteuid()));
}
#else
static KaiValue *kai_default_process_start(void *self, KaiValue *cmd, KaiValue *args, KaiCont *k) { kai_host_absent("processes"); }
KAI_SCHED_FN KaiValue *kai_default_process_wait(void *self, KaiValue *child, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{ kai_host_absent("processes"); }
#endif
static KaiValue *kai_default_process_kill(void *self, KaiValue *child, KaiValue *sig, KaiCont *k) { kai_host_absent("processes"); }
static KaiValue *kai_default_process_kill_group(void *self, KaiValue *child, KaiValue *sig, KaiCont *k) { kai_host_absent("processes"); }
static KaiValue *kai_default_process_uid(void *self, KaiCont *k) { kai_host_absent("processes"); }
#endif

/* exit(code) -> Nothing. _exit(2) — skip libc atexit / stdio flush
 * to match the doc spec contract. The `: Nothing` return type is
 * load-bearing: we never resume k. */
static KaiValue *kai_default_process_exit(void *self, KaiValue *code, KaiCont *k) {
    (void) self;
    (void) k;
    int c = (kai_is_int(code)) ? (int) kai_intf(code) : 0;
    _exit(c);
    /* unreachable */
    return NULL;
}

#if KAI_HOST_PROCESS
/* start_piped(cmd, args, pipe_stdin, pipe_stdout, pipe_stderr)
 *   -> Result[Child, String].
 * popen-shaped: attach a pipe to any of the child's stdin, stdout and
 * stderr; the rest inherit. Unlike `start`, primitive failure surfaces
 * as Err — a missing binary is an ordinary outcome when shelling out.
 * Parent ends are FD_CLOEXEC so a later fork cannot hold a stray
 * write end open and starve a sibling reader of EOF. */
static int _kai_proc_pipe_open(int want, int p[2]) {
    if (!want || pipe(p) == 0) return 0;
    int e = errno;
    p[0] = p[1] = -1;
    return e;
}

static void _kai_proc_pipe_close_pair(int p[2]) {
    if (p[0] >= 0) close(p[0]);
    if (p[1] >= 0) close(p[1]);
}

/* Child side, between fork and exec: async-signal-safe calls only. */
static void _kai_proc_pipe_child_dup(int p[2], int child_end, int target) {
    if (p[child_end] < 0) return;
    if (p[child_end] != target) { dup2(p[child_end], target); close(p[child_end]); }
    close(p[1 - child_end]);
}

static int _kai_proc_pipe_parent_read_end(int p[2]) {
    if (p[0] < 0) return -1;
    close(p[1]);
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
    return p[0];
}

static int _kai_proc_want(KaiValue *flag) {
    return flag && flag->tag == KAI_BOOL && flag->as.b;
}

static KaiValue *_kai_process_spawn(KaiValue *cmd, KaiValue *args, KaiValue *pipe_in_v,
                                    KaiValue *pipe_out_v, KaiValue *pipe_err_v,
                                    int own_group, KaiCont *k) {
    if (!cmd || cmd->tag != KAI_STR) {
        return _kai_process_err_msg(k, "start_piped: cmd must be a String");
    }
    int in_p[2]  = { -1, -1 };
    int out_p[2] = { -1, -1 };
    int err_p[2] = { -1, -1 };
    int e = _kai_proc_pipe_open(_kai_proc_want(pipe_in_v), in_p);
    if (!e) e = _kai_proc_pipe_open(_kai_proc_want(pipe_out_v), out_p);
    if (!e) e = _kai_proc_pipe_open(_kai_proc_want(pipe_err_v), err_p);
    if (e) {
        _kai_proc_pipe_close_pair(in_p);
        _kai_proc_pipe_close_pair(out_p);
        _kai_proc_pipe_close_pair(err_p);
        return _kai_process_err(k, e);
    }
    const char *cmd_cstr = cmd->as.s.bytes ? cmd->as.s.bytes : "";
    int argc = 0;
    char **argv = _kai_process_build_argv(cmd_cstr, args, &argc);
    if (!argv) { kai_eputs("kai: out of memory\n"); kai_exit(1); }
    kai_reactor_init();
    pid_t pid = fork();
    if (pid < 0) {
        e = errno;
        _kai_process_free_argv(argv, argc);
        _kai_proc_pipe_close_pair(in_p);
        _kai_proc_pipe_close_pair(out_p);
        _kai_proc_pipe_close_pair(err_p);
        return _kai_process_err(k, e);
    }
    if (pid == 0) {
        if (own_group) setpgid(0, 0);
        _kai_process_child_reset_signals();
        _kai_proc_pipe_child_dup(in_p, 0, 0);
        _kai_proc_pipe_child_dup(out_p, 1, 1);
        _kai_proc_pipe_child_dup(err_p, 1, 2);
        execvp(cmd_cstr, argv);
        /* Async-signal-safe writers only. 127 = command not found. */
        const char *prefix = "kai: Process.start_piped: execvp: ";
        const char *msg    = strerror(errno);
        ssize_t w;
        w = write(2, prefix, strlen(prefix)); (void) w;
        if (msg) { w = write(2, msg, strlen(msg)); (void) w; }
        w = write(2, "\n", 1); (void) w;
        _exit(127);
    }
    _kai_process_free_argv(argv, argc);
    /* Also set from the parent: a `kill_group` issued before the child
     * runs its own setpgid must still find the group. EACCES after the
     * child's exec is harmless — the child already did it. */
    if (own_group) setpgid(pid, pid);
    int wr = -1;
    if (in_p[1] >= 0) {
        close(in_p[0]);
        wr = in_p[1];
        fcntl(wr, F_SETFD, FD_CLOEXEC);
        fcntl(wr, F_SETFL, fcntl(wr, F_GETFL) | O_NONBLOCK);
#if defined(F_SETNOSIGPIPE)
        fcntl(wr, F_SETNOSIGPIPE, 1);
#endif
    }
    _kai_proc_pipe_add((int) pid, wr, _kai_proc_pipe_parent_read_end(out_p),
                       _kai_proc_pipe_parent_read_end(err_p), own_group);
    return _kai_process_ok(k, _kai_process_make_child((int) pid));
}

static KaiValue *kai_default_process_start_piped(void *self, KaiValue *cmd, KaiValue *args,
                                                 KaiValue *pipe_in_v, KaiValue *pipe_out_v,
                                                 KaiValue *pipe_err_v, KaiCont *k) {
    (void) self;
    return _kai_process_spawn(cmd, args, pipe_in_v, pipe_out_v, pipe_err_v, 0, k);
}

/* start_group(...) -> Result[Child, String]. `start_piped` with the
 * child as leader of a new process group, for `kill_group`. */
static KaiValue *kai_default_process_start_group(void *self, KaiValue *cmd, KaiValue *args,
                                                 KaiValue *pipe_in_v, KaiValue *pipe_out_v,
                                                 KaiValue *pipe_err_v, KaiCont *k) {
    (void) self;
    return _kai_process_spawn(cmd, args, pipe_in_v, pipe_out_v, pipe_err_v, 1, k);
}

/* write_stdin(c, data) -> Result[Unit, String]. Writes every byte or
 * reports the failing errno; a reader that died early is Err("Broken
 * pipe"), never a fatal SIGPIPE. A full pipe parks the fiber on
 * write-readiness, never the thread. The fd is looked up again after
 * each park: a concurrent `close_stdin` or `wait` may have closed it,
 * and its number reused by an unrelated file. */
KAI_SCHED_FN KaiValue *kai_default_process_write_stdin(void *self, KaiValue *child, KaiValue *data, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
    int pid = _kai_process_record_pid(child);
    int fd  = pid > 0 ? _kai_proc_pipe_fd(pid, 0) : -1;
    if (fd < 0) {
        return _kai_process_err_msg(k, "write_stdin: stdin is not piped");
    }
    if (!data || data->tag != KAI_STR || !data->as.s.bytes) {
        return _kai_process_ok(k, kai_unit());
    }
    kai_reactor_init();
    const char *buf = data->as.s.bytes;
    size_t len = data->as.s.len, off = 0;
    while (off < len) {
        ssize_t w = _kai_proc_write_once(fd, buf + off, len - off);
        if (w >= 0) { off += (size_t) w; continue; }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return _kai_process_err(k, errno);
        kai_reactor_park_socket_write(kai_current_fiber(), fd);
        fd = _kai_proc_pipe_fd(pid, 0);
        if (fd < 0) return _kai_process_err_msg(k, "write_stdin: stdin was closed");
    }
    return _kai_process_ok(k, kai_unit());
}
#endif

/* close_stdin(c) -> Result[Unit, String]. EOFs the child's stdin.
 * Idempotent — closing an already-closed end is Ok, matching pclose's
 * tolerance; only a malformed Child is an error. */
static KaiValue *kai_default_process_close_stdin(void *self, KaiValue *child, KaiCont *k) {
    (void) self;
    int pid = _kai_process_record_pid(child);
    if (pid <= 0) {
        return _kai_process_err_msg(k, "close_stdin: invalid Child");
    }
    _kai_proc_pipe_close_stdin(pid);
    return _kai_process_ok(k, kai_unit());
}

/* One read of up to 64 KiB from a piped child stream (1 = stdout,
 * 2 = stderr); Ok("") is EOF. Reading chunk-by-chunk (rather than one
 * read-to-end op) is what lets a caller drain output larger than the
 * OS pipe capacity without deadlocking the child. The read end is
 * O_NONBLOCK: an empty pipe parks the fiber on read-readiness, never
 * the thread. The fd is looked up again after each park: a concurrent
 * `wait` may have closed it, and its number reused by an unrelated file. */
#if !KAI_SCHED_DECL_ONLY
static KaiValue *_kai_proc_read_chunk(KaiValue *child, int which, KaiCont *k) {
    const char *name = which == 1 ? "stdout" : "stderr";
    char msg[64];
    int pid = _kai_process_record_pid(child);
    int fd  = pid > 0 ? _kai_proc_pipe_fd(pid, which) : -1;
    if (fd < 0) {
        snprintf(msg, sizeof msg, "read_%s: %s is not piped", name, name);
        return _kai_process_err_msg(k, msg);
    }
    kai_reactor_init();
    enum { KAI_PROC_READ_CHUNK = 65536 };
    char *buf = (char *) malloc(KAI_PROC_READ_CHUNK);
    if (!buf) { kai_eputs("kai: out of memory\n"); kai_exit(1); }
    ssize_t n;
    for (;;) {
        n = read(fd, buf, KAI_PROC_READ_CHUNK);
        if (n >= 0) break;
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) break;
        kai_reactor_park_socket_read(kai_current_fiber(), fd);
        fd = _kai_proc_pipe_fd(pid, which);
        if (fd < 0) {
            free(buf);
            snprintf(msg, sizeof msg, "read_%s: %s was closed", name, name);
            return _kai_process_err_msg(k, msg);
        }
    }
    if (n < 0) {
        int e = errno;
        free(buf);
        return _kai_process_err(k, e);
    }
    KaiValue *s = kai_str_from_bytes(buf, (size_t) n);
    free(buf);
    return _kai_process_ok(k, s);
}
#endif

KAI_SCHED_FN KaiValue *kai_default_process_read_stdout(void *self, KaiValue *child, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
    return _kai_proc_read_chunk(child, 1, k);
}
#endif

KAI_SCHED_FN KaiValue *kai_default_process_read_stderr(void *self, KaiValue *child, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
    return _kai_proc_read_chunk(child, 2, k);
}
#endif
#else
static KaiValue *kai_default_process_start_piped(void *self, KaiValue *cmd, KaiValue *args, KaiValue *pipe_in_v, KaiValue *pipe_out_v, KaiValue *pipe_err_v, KaiCont *k) { kai_host_absent("processes"); }
static KaiValue *kai_default_process_start_group(void *self, KaiValue *cmd, KaiValue *args, KaiValue *pipe_in_v, KaiValue *pipe_out_v, KaiValue *pipe_err_v, KaiCont *k) { kai_host_absent("processes"); }
KAI_SCHED_FN KaiValue *kai_default_process_write_stdin(void *self, KaiValue *child, KaiValue *data, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{ kai_host_absent("processes"); }
#endif
static KaiValue *kai_default_process_close_stdin(void *self, KaiValue *child, KaiCont *k) { kai_host_absent("processes"); }
KAI_SCHED_FN KaiValue *kai_default_process_read_stdout(void *self, KaiValue *child, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{ kai_host_absent("processes"); }
#endif
KAI_SCHED_FN KaiValue *kai_default_process_read_stderr(void *self, KaiValue *child, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{ kai_host_absent("processes"); }
#endif
#endif

