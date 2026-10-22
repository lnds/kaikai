/* Fragment of runtime.h; include runtime.h, never this file. */
/* ==================================================================
 * F2 — dedicated reactor thread. At N>1 the reactor no longer runs
 * inline on a scheduler thread (F1 drained it on thread 0 only when
 * that thread went idle, so CPU-bound fibers starved a concurrent
 * sleeper). Instead a dedicated thread owns the poll() loop and every
 * shared reactor structure — the timer wheel, the socket/pid/stdin/
 * signal/file-pool waiter lists, and `kai_reactor_parked_count` — under
 * `kai_reactor_mu`. Scheduler threads never touch those directly: they
 * stamp a park reason and hand the fiber to the root, which commits it
 * under the lock (`kai_sched_commit_park`) and pokes the reactor's
 * self-pipe so a poll() already asleep with a stale timeout re-arms.
 *
 * Lock order: `kai_reactor_mu` (wheel + waiter lists + parked_count), a slot
 * lock (state + steal list + home_thread + wake_pending) and a mailbox lock
 * are disjoint — every site takes exactly one, never two nested, so there is
 * no order to get wrong. Timeout-receive is the one fiber on two structures
 * at once (recv-waiter chain plus timer wheel): it disarms the wheel after
 * releasing the mailbox lock, never under it. */
#if defined(KAI_SEPARATE_COMPILATION)
extern pthread_mutex_t kai_reactor_mu;
extern pthread_t       kai_reactor_thread;
#  if defined(KAI_RUNTIME_OWNER)
pthread_mutex_t kai_reactor_mu;
pthread_t       kai_reactor_thread;
#  endif
#else
static pthread_mutex_t kai_reactor_mu;
static pthread_t       kai_reactor_thread;
#endif

/* No-op at N=1 (byte-identical) — the whole F2 machinery is gated on
 * nthreads>1 and the inline single-thread reactor keeps its lock-free
 * path. At N>1 these serialize every shared-reactor access. */
static inline void kai_reactor_lock(void)   { if (kai_nthreads > 1) pthread_mutex_lock(&kai_reactor_mu); }
static inline void kai_reactor_unlock(void) { if (kai_nthreads > 1) pthread_mutex_unlock(&kai_reactor_mu); }

/* Wake the reactor out of poll() so it recomputes its timeout against a
 * freshly linked waiter. A byte on the file-pool self-pipe (which the
 * reactor always polls) breaks the wait; the drain reads it harmlessly.
 * The pipe buffers the byte, so a wake that races the reactor between
 * "unlock" and "poll" is not lost — the next poll returns at once. */
static void kai_reactor_wake(void) {
    if (kai_reactor_filepool_pipe[1] >= 0) {
        unsigned char b = 1;
        ssize_t w = write(kai_reactor_filepool_pipe[1], &b, 1);
        (void) w;
    }
}

/* The reactor thread collects every fiber a drain readied into this batch
 * under `kai_reactor_mu`, then unparks them after releasing the lock — so
 * a slot lock (taken by remote_unpark) is never nested under reactor_mu.
 * Single-owner (only the reactor thread touches it), so no lock of its
 * own. At N=1 the batch is unused: `kai_reactor_mark_ready` unparks inline,
 * exactly as the F1 drains did. */
#if defined(KAI_SEPARATE_COMPILATION)
extern KaiFiber **kai_reactor_ready_batch;
extern int        kai_reactor_ready_n;
extern int        kai_reactor_ready_cap;
#  if defined(KAI_RUNTIME_OWNER)
KaiFiber **kai_reactor_ready_batch = NULL;
int        kai_reactor_ready_n = 0;
int        kai_reactor_ready_cap = 0;
#  endif
#else
static KaiFiber **kai_reactor_ready_batch = NULL;
static int        kai_reactor_ready_n = 0;
static int        kai_reactor_ready_cap = 0;
#endif

/* Record one park/wake transition. `event` must be a string literal — the ring
 * stores the pointer, not a copy. Entries are claimed with an atomic counter
 * and written unsynchronised: a torn entry costs one garbled trace line, which
 * is the right trade against perturbing the very timing under investigation. */
static void kai_park_trace(const KaiFiber *f, const char *event) {
    int on = atomic_load(&kai_park_trace_on);
    if (on < 0) {
        on = getenv("KAI_TRACE_PARK") ? 1 : 0;
        atomic_store(&kai_park_trace_on, on);
    }
    if (!on) return;
    unsigned slot = atomic_fetch_add(&kai_park_trace_seq, 1) % KAI_PARK_TRACE_CAP;
    KaiParkTraceEntry *e = &kai_park_trace_ring[slot];
    e->fiber         = f;
    e->event         = event;
    e->thread        = kai_worker_here()->id;
    e->state         = f ? f->state : -1;
    e->wake_pending  = f ? f->wake_pending : -1;
    e->reactor_fired = f ? f->reactor_fired : -1;
    e->blocked_count = atomic_load(&kai_blocked_fiber_count);
    e->parked_count  = kai_reactor_parked_count;
}

static const char *kai_fiber_state_name(int state) {
    switch (state) {
        case KAI_FIBER_READY:     return "READY";
        case KAI_FIBER_RUNNING:   return "RUNNING";
        case KAI_FIBER_PARKED:    return "PARKED";
        case KAI_FIBER_DONE:      return "DONE";
        case KAI_FIBER_CANCELLED: return "CANCELLED";
        default:                  return "?";
    }
}

/* Dump the park/wake ring oldest-first, for the deadlock banner. Silent when
 * the trace is off, so the banner is unchanged on a normal run. */
static void kai_park_trace_dump(void) {
    if (atomic_load(&kai_park_trace_on) <= 0) return;
    unsigned total = atomic_load(&kai_park_trace_seq);
    unsigned shown = total < KAI_PARK_TRACE_CAP ? total : KAI_PARK_TRACE_CAP;
    kai_eprintf("kai: park trace (%u events, last %u):\n", total, shown);
    for (unsigned i = 0; i < shown; i++) {
        const KaiParkTraceEntry *e =
            &kai_park_trace_ring[(total - shown + i) % KAI_PARK_TRACE_CAP];
        kai_eprintf(
                "  fiber=%p %-19s tid=%d state=%s wake_pending=%d "
                "reactor_fired=%d blocked=%d parked=%d\n",
                (const void *) e->fiber, e->event ? e->event : "?", e->thread,
                kai_fiber_state_name(e->state), e->wake_pending,
                e->reactor_fired, e->blocked_count, e->parked_count);
    }
}

/* A drain readied `f`. At N=1 unpark inline (F1 behaviour, byte-identical
 * scheduling order). At N>1 defer into the batch — the reactor holds
 * `kai_reactor_mu` here and must not take a slot lock until it releases it. */
static void kai_reactor_mark_ready(KaiFiber *f) {
    /* The caller just spliced `f` off a reactor structure: stamp the wake
     * as reactor-delivered so the park site can tell it from a spurious
     * resume. Written under kai_reactor_mu (N>1) or on the only thread
     * (N=1); the fiber reads it only after the wake's happens-before. */
    f->reactor_fired = 1;
    if (kai_nthreads <= 1) { kai_sched_unpark(f); return; }
    if (kai_reactor_ready_n == kai_reactor_ready_cap) {
        int ncap = kai_reactor_ready_cap ? kai_reactor_ready_cap * 2 : 16;
        KaiFiber **nb = (KaiFiber **) realloc(kai_reactor_ready_batch,
                                              (size_t) ncap * sizeof(KaiFiber *));
        if (!nb) { kai_eprintf("kai: reactor ready-batch realloc failed\n"); kai_exit(1); }
        kai_reactor_ready_batch = nb;
        kai_reactor_ready_cap   = ncap;
    }
    kai_reactor_ready_batch[kai_reactor_ready_n++] = f;
}

/* Unpark everything the drains batched this round, after `kai_reactor_mu`
 * is released. Each remote_unpark flips PARKED→READY on the fiber's home
 * slot and enqueues it there; the home thread picks it up on its next
 * scheduler pass. No-op at N=1 (batch empty). */
static void kai_reactor_flush_ready(void) {
    for (int i = 0; i < kai_reactor_ready_n; i++) {
        kai_sched_unpark(kai_reactor_ready_batch[i]);
    }
    kai_reactor_ready_n = 0;
}

/* File-pool work item. Each fiber-side park allocates one of these
 * on the calling fiber's stack (lifetime = until wake) and pushes
 * onto kai_filepool_queue. A worker thread pops, invokes `work` on
 * `arg`, stores the return value in `result`, and writes one byte
 * to the completion pipe so the scheduler can pick the waiter up.
 *
 * `arg` is owned by the calling fiber for the duration of the work
 * (the worker only reads it); `result` is published by the worker
 * for the calling fiber to consume on resume. The typedef alias
 * sits up next to the reactor forward decls; only the struct
 * definition lives here. */
struct KaiFilepoolItem {
    KaiValue *(*work)(void *arg);
    void            *arg;
    KaiValue        *result;
    KaiFiber        *waiter;
    KaiFilepoolItem *queue_next;
};

#define KAI_FILEPOOL_WORKERS 4
/* One file-pool per process: a file op parked in one TU enqueues here and a
 * worker thread drains it, so the queue, its lock, and the started/threads
 * state are shared (owner defines, others extern). Two per-TU pools would
 * spawn two thread sets and split the work queue. */
#if defined(KAI_SEPARATE_COMPILATION)
extern pthread_mutex_t  kai_filepool_mu;
extern pthread_cond_t   kai_filepool_cv;
extern KaiFilepoolItem *kai_filepool_q_head;
extern KaiFilepoolItem *kai_filepool_q_tail;
extern int              kai_filepool_started;
extern pthread_t        kai_filepool_threads[KAI_FILEPOOL_WORKERS];
#  if defined(KAI_RUNTIME_OWNER)
pthread_mutex_t  kai_filepool_mu    = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t   kai_filepool_cv    = PTHREAD_COND_INITIALIZER;
KaiFilepoolItem *kai_filepool_q_head = NULL;
KaiFilepoolItem *kai_filepool_q_tail = NULL;
int              kai_filepool_started = 0;
pthread_t        kai_filepool_threads[KAI_FILEPOOL_WORKERS];
#  endif
#else
static pthread_mutex_t  kai_filepool_mu    = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   kai_filepool_cv    = PTHREAD_COND_INITIALIZER;
static KaiFilepoolItem *kai_filepool_q_head = NULL;
static KaiFilepoolItem *kai_filepool_q_tail = NULL;
static int              kai_filepool_started = 0;
static pthread_t        kai_filepool_threads[KAI_FILEPOOL_WORKERS];
#endif

/* SIGCHLD delivery slot. The handler does the minimum allowed
 * inside signal context: write a single byte to the pipe. The
 * scheduler's reactor drain reaps every child that has terminated
 * via waitpid(-1, ..., WNOHANG) and wakes the matching fiber from
 * kai_reactor_pid_waiters. */
static void kai_reactor_sigchld_handler(int sig) {
    (void) sig;
    /* write(2) is the only stdio call the SIGCHLD handler issues —
     * it is on POSIX's async-signal-safe list and the destination
     * pipe is O_NONBLOCK, so EAGAIN simply means "byte already
     * pending, the reactor will drain on next wake". */
    unsigned char b = 1;
    int saved = errno;
    if (kai_reactor_sigchld_pipe[1] >= 0) {
        ssize_t w = write(kai_reactor_sigchld_pipe[1], &b, 1);
        (void) w;
    }
    errno = saved;
}

/* Issue #671 — Phase R4: Signal-effect handler. Writes the signo
 * to the signal self-pipe so the reactor can promote the parked
 * `Signal.await()` fiber on the next poll wake. The signo fits in
 * the bottom byte (kai_signal_entries only ever contains values
 * ≤ SIGUSR2 == 31 on every POSIX system we target). write(2) and
 * the cast to unsigned char are async-signal-safe. */
static void kai_reactor_signal_handler(int sig) {
    int saved = errno;
    if (kai_reactor_signal_pipe[1] >= 0 && sig > 0 && sig <= 255) {
        unsigned char b = (unsigned char) sig;
        ssize_t w = write(kai_reactor_signal_pipe[1], &b, 1);
        (void) w;
    }
    errno = saved;
}

/* Monotonic clock helper. Used by the timer wheel for sleep
 * deadlines so wall-clock skew (NTP step, RTC adjust) does not
 * leak into Spawn.sleep semantics. */
static uint64_t kai_reactor_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        /* clock_gettime on CLOCK_MONOTONIC is guaranteed by POSIX;
         * any failure is a kernel bug. Fall back to zero so a sleep
         * still terminates rather than spinning. */
        return 0;
    }
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

/* Sorted-insert into the timer wheel. O(n) over concurrent
 * sleepers; expected n is small in v1. */
static void kai_reactor_timer_insert(KaiFiber *f) {
    f->reactor_next = NULL;
    if (!kai_reactor_timer_head ||
        f->reactor_deadline_ns < kai_reactor_timer_head->reactor_deadline_ns) {
        f->reactor_next = kai_reactor_timer_head;
        kai_reactor_timer_head = f;
        return;
    }
    KaiFiber *c = kai_reactor_timer_head;
    while (c->reactor_next &&
           c->reactor_next->reactor_deadline_ns <= f->reactor_deadline_ns) {
        c = c->reactor_next;
    }
    f->reactor_next = c->reactor_next;
    c->reactor_next = f;
}

/* Pop every fiber whose deadline is <= `now` from the head and
 * promote it to READY. Returns the number woken. */
static int kai_reactor_timer_drain(uint64_t now) {
    int woken = 0;
    while (kai_reactor_timer_head &&
           kai_reactor_timer_head->reactor_deadline_ns <= now) {
        KaiFiber *f = kai_reactor_timer_head;
        kai_reactor_timer_head = f->reactor_next;
        f->reactor_next = NULL;
        f->reactor_deadline_ns = 0;
        kai_reactor_parked_count--;
        kai_reactor_mark_ready(f);
        woken++;
    }
    return woken;
}

/* Splice `f` out of the timer wheel if present. Returns 1 if it was
 * found (and unparks accounting reconciled), 0 if absent. The dual-park
 * receive uses this to disarm its deadline timer once a message wakes
 * it first, so a later drain cannot wake a fiber that already returned. */
static int kai_reactor_timer_remove(KaiFiber *f) {
    KaiFiber **link = &kai_reactor_timer_head;
    while (*link) {
        if (*link == f) {
            *link = f->reactor_next;
            f->reactor_next = NULL;
            f->reactor_deadline_ns = 0;
            kai_reactor_parked_count--;
            return 1;
        }
        link = &(*link)->reactor_next;
    }
    return 0;
}

/* Splice the fiber parked on `pid` (if any) off the waiter list and
 * wake it with `status`. Caller holds the reactor lock. */
static int kai_reactor_wake_pid_waiter(int pid, int status) {
    KaiFiber **link = &kai_reactor_pid_waiters;
    while (*link) {
        if ((*link)->reactor_wait_pid == pid) {
            KaiFiber *f = *link;
            *link = f->reactor_next;
            f->reactor_next = NULL;
            f->reactor_wait_status = status;
            /* Leave reactor_wait_pid intact so the wait op
             * can confirm it matches on resume; clear elsewhere. */
            kai_reactor_parked_count--;
            kai_reactor_mark_ready(f);
            return 1;
        }
        link = &(*link)->reactor_next;
    }
    return 0;
}

#if KAI_HOST_REACTOR
/* Drain SIGCHLD self-pipe and waitpid(-1, ..., WNOHANG) until no
 * more children have terminated, waking the fiber parked on each
 * pid. Idempotent — safe to call when no children are pending. */
static int kai_reactor_sigchld_drain(void) {
    if (kai_reactor_sigchld_pipe[0] < 0) return 0;
    /* Drain the pipe (its content is just a wake notification —
     * the real state is in the kernel's child table). */
    for (;;) {
        unsigned char buf[64];
        ssize_t n = read(kai_reactor_sigchld_pipe[0], buf, sizeof(buf));
        if (n <= 0) break;
    }
    int woken = 0;
    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) break;  /* 0 = none ready, -1 = ECHILD/EINTR */
        if (kai_reactor_wake_pid_waiter((int) pid, status)) {
            woken++;
            continue;
        }
        /* No fiber parked on this pid yet — the exit raced the wait
         * op's own WNOHANG/park window, or wait has not been called.
         * Buffer the status; `kai_reactor_take_child_exit` or the
         * reconcile below hands it over. */
        KaiPendingExit *pe = (KaiPendingExit *) malloc(sizeof(KaiPendingExit));
        if (!pe) continue;  /* status lost only under OOM */
        pe->pid    = (int) pid;
        pe->status = status;
        pe->next   = kai_reactor_pending_exits;
        kai_reactor_pending_exits = pe;
    }
    /* Reconcile: wake any waiter whose status was buffered before it
     * finished parking. Runs on every reactor pass with live pid
     * waiters, so a fiber that lost the race is woken one pass later. */
    KaiPendingExit **pp = &kai_reactor_pending_exits;
    while (*pp) {
        if (kai_reactor_wake_pid_waiter((*pp)->pid, (*pp)->status)) {
            KaiPendingExit *dead = *pp;
            *pp = dead->next;
            free(dead);
            woken++;
        } else {
            pp = &(*pp)->next;
        }
    }
    return woken;
}
#endif

/* Consume a buffered exit for `pid`. Called by the wait op before
 * and after its own non-blocking waitpid, outside the reactor lock. */
static int kai_reactor_take_child_exit(int pid, int *status) {
    int found = 0;
    kai_reactor_lock();
    KaiPendingExit **pp = &kai_reactor_pending_exits;
    while (*pp) {
        if ((*pp)->pid == pid) {
            KaiPendingExit *e = *pp;
            *pp = e->next;
            *status = e->status;
            free(e);
            found = 1;
            break;
        }
        pp = &(*pp)->next;
    }
    kai_reactor_unlock();
    return found;
}

/* Drain the file-pool completion pipe and promote every fiber whose
 * work item just completed. */
static int kai_reactor_filepool_drain(void) {
    if (kai_reactor_filepool_pipe[0] < 0) return 0;
    /* Empty the pipe; the real signal is in each item's `result`
     * slot being non-NULL (workers store the result before writing
     * the byte). Items waiting for promotion are still in
     * kai_reactor_filepool_waiters. */
    for (;;) {
        unsigned char buf[64];
        ssize_t n = read(kai_reactor_filepool_pipe[0], buf, sizeof(buf));
        if (n <= 0) break;
    }
    int woken = 0;
    KaiFiber **link = &kai_reactor_filepool_waiters;
    while (*link) {
        KaiFiber *f = *link;
        KaiFilepoolItem *item = (KaiFilepoolItem *) f->reactor_data;
        if (item && item->result != (KaiValue *) NULL) {
            *link = f->reactor_next;
            f->reactor_next = NULL;
            kai_reactor_parked_count--;
            kai_reactor_mark_ready(f);
            woken++;
        } else {
            link = &(*link)->reactor_next;
        }
    }
    return woken;
}

/* Wake the parked Signal waiter if a signo is pending. Caller holds
 * the reactor lock at N>1. Returns 1 if a fiber was promoted. */
static int kai_reactor_signal_try_wake(void) {
    if (kai_reactor_signal_pending == 0) return 0;
    if (!kai_reactor_signal_waiter)      return 0;
    KaiFiber *f = kai_reactor_signal_waiter;
    kai_reactor_signal_waiter = NULL;
    /* Stash the signo in reactor_wait_status so the await handler
     * can recover it on resume. Re-use of the slot is safe: the
     * fiber is parked on a singleton waiter, never simultaneously
     * on a pid / socket waiter. */
    f->reactor_wait_status = kai_reactor_signal_pending;
    kai_reactor_signal_pending = 0;
    kai_reactor_parked_count--;
    kai_park_trace(f, "signal-try-wake");
    kai_reactor_mark_ready(f);
    return 1;
}

/* Drain the Signal self-pipe and promote the parked `Signal.await()`
 * waiter. Concurrent deliveries collapse to the most recent signo
 * under v1's "single waiter, single fire" contract. A signo arriving
 * while no fiber is parked is NOT discarded: it stays in
 * kai_reactor_signal_pending until the next await consumes it. The
 * drain can run on a reactor thread between a waiter's "ready"
 * handshake and its park commit — discarding here strands that
 * waiter forever. */
static int kai_reactor_signal_drain(void) {
    if (kai_reactor_signal_pipe[0] < 0) return 0;
    int signo = 0;
    unsigned char buf[64];
    for (;;) {
        ssize_t n = read(kai_reactor_signal_pipe[0], buf, sizeof(buf));
        if (n <= 0) break;
        if (n > 0) signo = (int) buf[n - 1];
    }
    if (signo != 0) kai_reactor_signal_pending = signo;
    return kai_reactor_signal_try_wake();
}

/* File-pool worker loop. Pops items off the FIFO queue, runs the
 * work function on the worker thread (so the blocking syscall stays
 * off the scheduler thread), publishes the result, and writes one
 * byte to the completion pipe to wake the scheduler. */
static void *kai_filepool_worker(void *arg) {
    (void) arg;
    kai_rc_ledger_register();
    for (;;) {
        pthread_mutex_lock(&kai_filepool_mu);
        while (!kai_filepool_q_head) {
            pthread_cond_wait(&kai_filepool_cv, &kai_filepool_mu);
        }
        KaiFilepoolItem *item = kai_filepool_q_head;
        kai_filepool_q_head = item->queue_next;
        if (!kai_filepool_q_head) kai_filepool_q_tail = NULL;
        pthread_mutex_unlock(&kai_filepool_mu);

        /* Sentinel item with NULL `work` signals shutdown — not
         * exercised in v1 (the runtime never tears down) but kept
         * symmetric with the queue protocol. */
        if (!item->work) { kai_rc_ledger_fold(); return NULL; }

        KaiValue *r = item->work(item->arg);

        /* Publish the result, then notify. The order matters: the
         * scheduler reads `result` after the byte arrives, so the
         * store must be visible first. v1 single-CPU x86/arm64
         * provides release ordering on aligned word stores, but a
         * pthread_mutex round-trip gives us the same guarantee
         * portably. */
        pthread_mutex_lock(&kai_filepool_mu);
        item->result = r ? r : kai_unit();
        pthread_mutex_unlock(&kai_filepool_mu);

        unsigned char b = 1;
        if (kai_reactor_filepool_pipe[1] >= 0) {
            ssize_t w = write(kai_reactor_filepool_pipe[1], &b, 1);
            (void) w;
        }
    }
}

/* Issue #620 — restore stdin's original flags on process exit. The
 * runtime flips fd 0 to O_NONBLOCK once the first stdin op runs;
 * leaving the shell's stdin in non-blocking mode after exit is a
 * subtle, hard-to-diagnose footgun (tools downstream of the kaikai
 * program would see EAGAIN on every read). atexit guarantees this
 * runs on normal termination and on `exit()` calls. */
static void kai_reactor_stdin_restore(void) {
    if (kai_reactor_stdin_orig_flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, kai_reactor_stdin_orig_flags);
        kai_reactor_stdin_orig_flags = -1;
    }
}

/* Issue #620 — set fd 0 to O_NONBLOCK once per process. Saves the
 * original flags into kai_reactor_stdin_orig_flags so atexit can
 * restore them. No-op on subsequent calls. If F_GETFL fails (eg. fd
 * 0 closed by the parent program), the function is a no-op and the
 * stdin parking path will surface the failure as a Fail/error. */
static void kai_reactor_stdin_set_nonblocking(void) {
    if (kai_reactor_stdin_orig_flags >= 0) return;
    int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (fl < 0) return;
    kai_reactor_stdin_orig_flags = fl;
    fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);
    atexit(kai_reactor_stdin_restore);
}

/* Lazy initialisation of the reactor on first use. Idempotent;
 * safe to call from every parking site. Installs SIGCHLD additively
 * (only if no existing handler is registered) so the stack-guard
 * SIGSEGV path and any future signal users continue to operate.
 *
 * The file-pool workers (4 OS threads) are deferred to the first
 * actual file op via kai_reactor_init_filepool — sleep-only and
 * process-only workloads should not pay the pthread_create cost. */
static void kai_reactor_init_filepool(void);
/* Idempotency is process-global, not per-TU: kai_reactor_init is called from
 * many sites that land in different modular TUs, and it opens the shared
 * self-pipes. A per-TU flag would let each TU re-run init and reopen the
 * pipes over the shared fds. */
#if defined(KAI_SEPARATE_COMPILATION)
extern int kai_reactor_inited;
#  if defined(KAI_RUNTIME_OWNER)
int kai_reactor_inited = 0;
#  endif
#else
static int kai_reactor_inited = 0;
#endif
static void kai_reactor_init(void) {
#if KAI_HOST_REACTOR
    if (kai_reactor_inited) return;
    kai_reactor_inited = 1;

    /* Self-pipes for the three wake sources (SIGCHLD, file-pool,
     * Signal R4). O_NONBLOCK so handlers and worker threads never
     * block; O_CLOEXEC so a forked child does not inherit them. */
    if (pipe(kai_reactor_sigchld_pipe)  != 0 ||
        pipe(kai_reactor_filepool_pipe) != 0 ||
        pipe(kai_reactor_signal_pipe)   != 0) {
        kai_eprintf("kai: reactor pipe() failed: %s\n", strerror(errno));
        kai_exit(1);
    }
    for (int i = 0; i < 3; i++) {
        int fds[3][2] = {
            { kai_reactor_sigchld_pipe[0],  kai_reactor_sigchld_pipe[1]  },
            { kai_reactor_filepool_pipe[0], kai_reactor_filepool_pipe[1] },
            { kai_reactor_signal_pipe[0],   kai_reactor_signal_pipe[1]   },
        };
        for (int p = 0; p < 2; p++) {
            int fd = fds[i][p];
            int fl = fcntl(fd, F_GETFL, 0);
            if (fl != -1) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
            int cl = fcntl(fd, F_GETFD, 0);
            if (cl != -1) fcntl(fd, F_SETFD, cl | FD_CLOEXEC);
        }
    }

    /* Install SIGCHLD additively. The stack-guard handler grabs
     * SIGSEGV/SIGBUS; we are explicit about touching only SIGCHLD
     * to avoid stomping on it. Refuse to install if SIGCHLD is
     * already taken by user code; the runtime exits with a clear
     * diagnostic rather than silently overriding. */
    struct sigaction old;
    if (sigaction(SIGCHLD, NULL, &old) == 0) {
        if (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN) {
            kai_eprintf(
                "kai: reactor cannot install SIGCHLD — slot already taken\n");
            kai_exit(1);
        }
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = kai_reactor_sigchld_handler;
    sigemptyset(&sa.sa_mask);
    /* SA_NOCLDSTOP so we are not woken for stop/continue traffic —
     * only terminal child exits matter. SA_RESTART is intentionally
     * NOT set: the scheduler's poll() must return early on EINTR so
     * the wake path is observed even when the self-pipe write loses
     * the race with the kernel's signal delivery. The worker
     * threads block SIGCHLD via their thread mask (see below), so
     * the absence of SA_RESTART does not affect their blocking
     * reads/writes. */
    sa.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
#else
    kai_host_absent("reactor");
#endif
}

/* Spin up the 4-worker file pool on the first file op. SIGCHLD is
 * blocked in every worker thread's mask so the signal always lands
 * on the scheduler (main) thread, which is the one draining the
 * self-pipe in kai_reactor_wait. The pool runs detached for the
 * lifetime of the process; the OS reclaims the threads on exit. */
static void kai_reactor_init_filepool(void) {
#if KAI_HOST_REACTOR
    if (kai_filepool_started) return;
    kai_filepool_started = 1;
    sigset_t block_set, prev_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &block_set, &prev_set);
    for (int i = 0; i < KAI_FILEPOOL_WORKERS; i++) {
        kai_rc_go_mt();
        if (pthread_create(&kai_filepool_threads[i], NULL,
                           kai_filepool_worker, NULL) != 0) {
            kai_eprintf("kai: reactor pthread_create failed: %s\n",
                    strerror(errno));
            kai_exit(1);
        }
        pthread_detach(kai_filepool_threads[i]);
    }
    pthread_sigmask(SIG_SETMASK, &prev_set, NULL);
#else
    kai_host_absent("reactor");
#endif
}

/* Submit a unit of work to the file-pool. The caller (a parking
 * file op handler) supplies `work` + `arg`, then parks itself on
 * kai_reactor_filepool_waiters and yields. The worker invokes
 * `work(arg)`, stores the KaiValue * result in `item->result`,
 * and wakes the scheduler. The item itself lives on the caller's
 * fiber stack — no heap allocation. */
static void kai_filepool_submit(KaiFilepoolItem *item) {
    pthread_mutex_lock(&kai_filepool_mu);
    item->queue_next = NULL;
    if (kai_filepool_q_tail) {
        kai_filepool_q_tail->queue_next = item;
    } else {
        kai_filepool_q_head = item;
    }
    kai_filepool_q_tail = item;
    pthread_cond_signal(&kai_filepool_cv);
    pthread_mutex_unlock(&kai_filepool_mu);
}

/* Public entry points used by the Clock / Process / File default
 * handlers. Each is a thin wrapper that links the calling fiber
 * into the appropriate reactor structure, bumps the parked-count,
 * and yields via kai_sched_park (which falls into kai_reactor_wait
 * once the run queue empties). On resume the reactor drain has
 * already cleared the fiber's reactor_* slots and unparked it. */
static void kai_reactor_park_timer_linked(KaiFiber *f, uint64_t deadline_ns) {
    kai_flush_stdout();
    f->reactor_deadline_ns = deadline_ns;
    if (kai_nthreads > 1) {
        /* F2: the root links us into the wheel post-swap (commit_park). */
        f->pending_park = KAI_PARK_TIMER;
        kai_sched_park_linked();
        return;
    }
    kai_reactor_timer_insert(f);
    kai_reactor_parked_count++;
    kai_sched_park_linked();
}

static void kai_reactor_park_timer(KaiFiber *f, uint64_t deadline_ns) {
    kai_reactor_park_timer_linked(f, deadline_ns);
    kai_check_cancel_yield_point();
}

static void kai_reactor_park_pid(KaiFiber *f, int pid) {
    kai_flush_stdout();
    f->reactor_wait_pid    = pid;
    f->reactor_wait_status = 0;
    if (kai_nthreads > 1) {
        f->pending_park = KAI_PARK_PID;
        kai_sched_park();
        return;
    }
    /* Push onto the head; pid lookup walks the list so order is
     * irrelevant. The drain helper splices the matching node out. */
    f->reactor_next = kai_reactor_pid_waiters;
    kai_reactor_pid_waiters = f;
    kai_reactor_parked_count++;
    kai_sched_park();
}

/* Issue #620 — Phase R3: park `f` on the singleton stdin slot.
 * Returns 0 on success and -1 if another fiber already holds the
 * slot (the caller should treat that as "concurrent stdin readers
 * — undefined" and panic). The slot is cleared by `kai_reactor_wait`
 * when POLLIN / POLLHUP / POLLERR fires on STDIN_FILENO; on resume
 * the parking site simply retries its read(). */
static int kai_reactor_park_stdin(KaiFiber *f) {
    kai_flush_stdout();
    if (kai_nthreads > 1) {
        /* Reserve the singleton slot check under the lock; the reactor
         * clears it on readiness. Concurrent stdin readers are undefined
         * (the caller panics on -1), so the check→commit gap is benign. */
        kai_reactor_lock();
        int busy = (kai_reactor_stdin_waiter != NULL);
        kai_reactor_unlock();
        if (busy) return -1;
        f->pending_park = KAI_PARK_STDIN;
        kai_sched_park();
        return 0;
    }
    if (kai_reactor_stdin_waiter != NULL) return -1;
    kai_reactor_stdin_waiter = f;
    kai_reactor_parked_count++;
    kai_sched_park();
    return 0;
}

/* Issue #671 — Phase R4: park `f` on the singleton Signal waiter
 * slot. Returns 0 on success, -1 if another fiber is already
 * parked (the caller panics with a clear diagnostic, same shape
 * as the R3 stdin contract). On resume the delivered signo lives
 * in `f->reactor_wait_status`; the await handler maps it back to
 * the matching variant. */
static int kai_reactor_park_signal(KaiFiber *f) {
    kai_flush_stdout();
    f->reactor_wait_status = 0;
    if (kai_nthreads > 1) {
        kai_reactor_lock();
        int busy = (kai_reactor_signal_waiter != NULL);
        int pend = busy ? 0 : kai_reactor_signal_pending;
        if (pend != 0) kai_reactor_signal_pending = 0;
        kai_reactor_unlock();
        if (busy) return -1;
        if (pend != 0) { f->reactor_wait_status = pend; return 0; }
        f->pending_park = KAI_PARK_SIGNAL;
        kai_sched_park();
        return 0;
    }
    if (kai_reactor_signal_waiter != NULL) return -1;
    if (kai_reactor_signal_pending != 0) {
        f->reactor_wait_status = kai_reactor_signal_pending;
        kai_reactor_signal_pending = 0;
        return 0;
    }
    kai_reactor_signal_waiter = f;
    kai_reactor_parked_count++;
    kai_sched_park();
    return 0;
}

/* Issue #630 — Phase R2: set O_NONBLOCK on a socket fd. Idempotent;
 * safe to call once per fd creation site. We do NOT save/restore the
 * original flags (unlike stdin) because the fd was just opened by us
 * — no caller cares about its pre-flag state. */
static void kai_socket_set_nonblock(int fd) {
    if (fd < 0) return;
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return;
    if (!(fl & O_NONBLOCK)) {
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
}

/* Issue #630 — Phase R2: park `f` waiting for read-readiness on
 * `fd` (accept on a listener, recv on a connection). Pushes the
 * fiber onto the socket_read_waiters list, stashes the fd in
 * reactor_wait_pid (slot is repurposed; pid waiters and socket
 * waiters are mutually exclusive), bumps the parked-count, and
 * yields. On resume the drain helper has spliced the fiber out;
 * the caller retries its non-blocking read(). */
static void kai_reactor_park_socket_read(KaiFiber *f, int fd) {
    kai_flush_stdout();
    f->reactor_wait_pid    = fd;
    f->reactor_wait_status = 0;
    if (kai_nthreads > 1) {
        f->pending_park = KAI_PARK_SOCKET_READ;
        kai_sched_park();
        return;
    }
    f->reactor_next = kai_reactor_socket_read_waiters;
    kai_reactor_socket_read_waiters = f;
    kai_reactor_parked_count++;
    kai_sched_park();
}

/* Issue #630 — Phase R2: park `f` waiting for write-readiness on
 * `fd` (connect handshake completion, send when the kernel buffer is
 * full). Symmetric with kai_reactor_park_socket_read on the
 * socket_write_waiters list. */
static void kai_reactor_park_socket_write(KaiFiber *f, int fd) {
    kai_flush_stdout();
    f->reactor_wait_pid    = fd;
    f->reactor_wait_status = 0;
    if (kai_nthreads > 1) {
        f->pending_park = KAI_PARK_SOCKET_WRITE;
        kai_sched_park();
        return;
    }
    f->reactor_next = kai_reactor_socket_write_waiters;
    kai_reactor_socket_write_waiters = f;
    kai_reactor_parked_count++;
    kai_sched_park();
}

/* Dual-park read-readiness + deadline on a single intrusive slot. The
 * fiber lives only on the read-waiter list; its `reactor_deadline_ns`
 * arms the poll timeout and the deadline-drain pass in kai_reactor_wait.
 * Whichever wins, the drain that unparks us has already spliced us out
 * of the one list — no second structure to disarm, so no UAF window.
 * The deadline-drain stamps `reactor_wait_status = 1` before waking;
 * readiness leaves it 0. Returns 1 (readiness) or 0 (deadline). */
static int kai_reactor_park_socket_read_timeout(KaiFiber *f, int fd, uint64_t deadline_ns) {
    kai_flush_stdout();
    f->reactor_wait_pid    = fd;
    f->reactor_wait_status = 0;
    f->reactor_deadline_ns = deadline_ns;
    if (kai_nthreads > 1) {
        f->pending_park = KAI_PARK_SOCKET_READ;
        kai_sched_park();
    } else {
        f->reactor_next = kai_reactor_socket_read_waiters;
        kai_reactor_socket_read_waiters = f;
        kai_reactor_parked_count++;
        kai_sched_park();
    }
    int timed_out = (f->reactor_wait_status == 1);
    f->reactor_wait_status = 0;
    f->reactor_deadline_ns = 0;
    return timed_out ? 0 : 1;
}

/* Submit `work(arg)` to the file-pool worker queue and park the
 * caller until completion. Returns the worker's KaiValue *result.
 * The KaiFilepoolItem lives on the caller's fiber stack — safe
 * because the parked fiber's stack is preserved until resume. */
static KaiValue *kai_reactor_run_in_pool(KaiValue *(*work)(void *), void *arg) {
    kai_reactor_init_filepool();
    KaiFilepoolItem item;
    item.work       = work;
    item.arg        = arg;
    item.result     = NULL;
    item.waiter     = kai_current_fiber();
    item.queue_next = NULL;

    KaiFiber *me = kai_current_fiber();
    me->reactor_data = &item;
    if (kai_nthreads > 1) {
        /* F2: the root links us into the waiter list post-swap. Submit
         * first so a worker can start; a completion that races the commit
         * is not lost — the drain readies on `item->result`, and commit's
         * self-pipe poke forces a re-drain that observes it. */
        me->pending_park = KAI_PARK_FILEPOOL;
        kai_filepool_submit(&item);
        kai_sched_park();
        /* Re-park on a spurious resume: `item` lives on THIS stack, so
         * returning before the worker publishes `result` would hand the
         * pool a dangling frame. */
        while (!item.result) {
            me->pending_park = KAI_PARK_FILEPOOL;
            kai_sched_park();
        }
    } else {
        /* Queue order is irrelevant for the waiter list; insert at head. */
        me->reactor_next = kai_reactor_filepool_waiters;
        kai_reactor_filepool_waiters = me;
        kai_reactor_parked_count++;
        kai_filepool_submit(&item);
        kai_sched_park();
    }

    /* On resume the drain helper has spliced us out of
     * kai_reactor_filepool_waiters. The result slot is set by the
     * worker prior to the pipe write. */
    KaiValue *r = item.result;
    me->reactor_data = NULL;
    return r ? r : kai_unit();
}

/* Issue #630 — Phase R2: count the live socket-waiter fds in each
 * direction. Two fibers parked on the same fd in the same direction
 * is impossible in v1 (every Conn / Listener belongs to a single
 * fiber under the per-fiber-arena model). Counting unique fds is
 * therefore equivalent to counting waiters. */
static int kai_reactor_count_socket_waiters(KaiFiber *head) {
    int n = 0;
    for (KaiFiber *f = head; f; f = f->reactor_next) n++;
    return n;
}

#if KAI_HOST_REACTOR
/* Issue #630 — Phase R2: drain one socket-direction waiter list.
 * For every fiber whose fd shows up in `pfds[i].revents` with
 * POLLIN/POLLOUT/POLLHUP/POLLERR set, splice it out and unpark.
 * The handler at the park site will retry its non-blocking syscall
 * and either succeed, EOF, or re-park if the kernel buffer drained
 * between wake and retry (spurious wake — POSIX permits it). */
static int kai_reactor_socket_drain(KaiFiber **head_ptr, struct pollfd *pfds,
                                    int nfds, short ready_mask) {
    int woken = 0;
    KaiFiber **link = head_ptr;
    while (*link) {
        int fd = (*link)->reactor_wait_pid;
        /* OR every pfds entry's revents for this fd. The same fd can
         * appear in the poll set multiple times (full-duplex sockets
         * with both read and write waiters, or two readers on a
         * shared listener — accept() distributes connections across
         * them). Aggregating revents avoids missing a wake when the
         * ready entry is not the first match. */
        short revents = 0;
        for (int i = 0; i < nfds; i++) {
            if (pfds[i].fd == fd) revents |= pfds[i].revents;
        }
        if (revents & (ready_mask | POLLHUP | POLLERR | POLLNVAL)) {
            KaiFiber *f = *link;
            *link = f->reactor_next;
            f->reactor_next = NULL;
            f->reactor_wait_pid = 0;
            kai_reactor_parked_count--;
            kai_reactor_mark_ready(f);
            woken++;
        } else {
            link = &(*link)->reactor_next;
        }
    }
    return woken;
}
#endif

/* Wake every deadline-armed socket-read waiter whose deadline has
 * passed, stamping reactor_wait_status = 1 so the park site returns
 * timeout. Runs AFTER the readiness drain: a socket that turned ready
 * in the same poll is already off the list, so data-just-in-time wins
 * over a coincident deadline. */
static int kai_reactor_socket_read_deadline_drain(uint64_t now) {
    int woken = 0;
    KaiFiber **link = &kai_reactor_socket_read_waiters;
    while (*link) {
        KaiFiber *f = *link;
        if (f->reactor_deadline_ns && f->reactor_deadline_ns <= now) {
            *link = f->reactor_next;
            f->reactor_next = NULL;
            f->reactor_wait_pid = 0;
            f->reactor_wait_status = 1;
            kai_reactor_parked_count--;
            kai_reactor_mark_ready(f);
            woken++;
        } else {
            link = &(*link)->reactor_next;
        }
    }
    return woken;
}

