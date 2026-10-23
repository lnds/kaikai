/* Fragment of runtime.h; include runtime.h, never this file. */

#if defined(__wasm__)
/* Fibers parked until the page posts a host event. A wake from the host moves
 * all of them to READY; each re-checks the host queue and parks again if a
 * sibling took the event. A cancel reaches a parked waiter at its next wake. */
#if defined(KAI_SEPARATE_COMPILATION)
extern KaiFiber *kai_web_waiters;
#  if defined(KAI_RUNTIME_OWNER)
KaiFiber *kai_web_waiters = NULL;
#  endif
#else
static KaiFiber *kai_web_waiters = NULL;
#endif

/* Weak: every translation unit carries this text, and the program binds it as `extern "C"`. */
__attribute__((weak)) void kai_web_wait_event(void) {
    KaiFiber *f = kai_current_fiber();
    kai_flush_stdout();
    f->reactor_next = kai_web_waiters;
    kai_web_waiters = f;
    kai_reactor_parked_count++;
    kai_sched_park_linked();
    kai_check_cancel_yield_point();
}

static void kai_web_wake_waiters(void) {
    while (kai_web_waiters) {
        KaiFiber *f = kai_web_waiters;
        kai_web_waiters = f->reactor_next;
        f->reactor_next = NULL;
        kai_reactor_parked_count--;
        kai_reactor_mark_ready(f);
    }
}
#endif
/* Block in poll() until either the SIGCHLD pipe or the file-pool
 * completion pipe fires, a socket fd becomes ready, or the next
 * timer deadline arrives. Promotes every newly-ready fiber to the
 * run queue. Called by kai_sched_park when the ready queue is empty
 * but reactor waiters exist — the dispatch loop's substitute for a
 * dedicated event loop thread. */
static void kai_reactor_wait(void) {
#if KAI_HOST_REACTOR
    /* F2: read the shared wheel + waiter lists to compute the timeout and
     * build the poll set under `kai_reactor_mu` (no-op at N=1), then drop
     * the lock before poll() — poll blocks the reactor thread and must not
     * hold the mutex a committing scheduler needs. A waiter linked between
     * this unlock and poll() is not missed: its commit poked the self-pipe,
     * so poll() (which always watches that pipe) returns at once. */
    kai_reactor_lock();
    /* Compute the timeout in ms (poll's resolution). A negative
     * timeout is "wait forever"; a zero timeout polls. The wake
     * deadline is the earliest of the timer wheel head and any
     * deadline-armed socket-read waiter (recv_timeout), so a socket
     * deadline that precedes every timer still bounds the sleep. */
    uint64_t wake_dl = 0;
    if (kai_reactor_timer_head) {
        wake_dl = kai_reactor_timer_head->reactor_deadline_ns;
    }
    for (KaiFiber *f = kai_reactor_socket_read_waiters; f; f = f->reactor_next) {
        if (f->reactor_deadline_ns &&
            (wake_dl == 0 || f->reactor_deadline_ns < wake_dl)) {
            wake_dl = f->reactor_deadline_ns;
        }
    }
    int timeout_ms = -1;
    if (wake_dl) {
        uint64_t now = kai_reactor_now_ns();
        if (wake_dl <= now) {
            timeout_ms = 0;
        } else {
            uint64_t diff_ns = wake_dl - now;
            uint64_t ms = (diff_ns + 999999ULL) / 1000000ULL;  /* ceil */
            if (ms > (uint64_t) INT_MAX) ms = (uint64_t) INT_MAX;
            timeout_ms = (int) ms;
        }
    }

    /* Size the pfds array: 2 self-pipes + optional stdin + N
     * socket-read + M socket-write. Use a small stack buffer for the
     * common case (no sockets) and fall back to a heap alloc when
     * more than ~16 fds are live. */
    int nread  = kai_reactor_count_socket_waiters(kai_reactor_socket_read_waiters);
    int nwrite = kai_reactor_count_socket_waiters(kai_reactor_socket_write_waiters);
    /* 3 self-pipes (sigchld + filepool + signal) + optional stdin
     * (R3) + optional signal-waiter pipe (R4 — same fd as the
     * self-pipe, but kept in the count for clarity). socket waiters
     * grow the set per (fd, direction). */
    int max_fds = 3 + 1 + nread + nwrite;
    struct pollfd  stack_pfds[16];
    struct pollfd *pfds = stack_pfds;
    int heap_alloced = 0;
    if (max_fds > (int) (sizeof(stack_pfds) / sizeof(stack_pfds[0]))) {
        pfds = (struct pollfd *) malloc((size_t) max_fds * sizeof(*pfds));
        if (!pfds) {
            kai_eprintf("kai: reactor pfds malloc failed\n");
            kai_exit(1);
        }
        heap_alloced = 1;
    }

    int nfds = 0;
    if (kai_reactor_sigchld_pipe[0] >= 0) {
        pfds[nfds].fd = kai_reactor_sigchld_pipe[0];
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        nfds++;
    }
    if (kai_reactor_filepool_pipe[0] >= 0) {
        pfds[nfds].fd = kai_reactor_filepool_pipe[0];
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        nfds++;
    }
    /* Issue #671 — Phase R4: signal self-pipe stays in the poll
     * set for the lifetime of the process. A signal that arrives
     * with no parked waiter is dropped by the drain — that races
     * the v1 sigwait body exactly the same way (an unblocked signal
     * arriving before sigwait() was lost too); the new path adds
     * no new strand-against-handler hazard. */
    if (kai_reactor_signal_pipe[0] >= 0) {
        pfds[nfds].fd = kai_reactor_signal_pipe[0];
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        nfds++;
    }
    /* Issue #620 — Phase R3: only register stdin in the poll set
     * while a fiber is parked on it. Otherwise we would wake on
     * every keystroke even when no one is reading, burn CPU, and
     * have no waiter to promote. */
    if (kai_reactor_stdin_waiter != NULL) {
        pfds[nfds].fd = STDIN_FILENO;
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        nfds++;
    }
    /* Issue #630 — Phase R2: register every socket waiter's fd. If
     * the same fd shows up in both directions (a fiber reading on a
     * fd while another writes to it — rare in v1 but legal), the fd
     * appears twice in the poll set with disjoint event masks; POSIX
     * permits this and reports revents per-entry. */
    for (KaiFiber *f = kai_reactor_socket_read_waiters; f; f = f->reactor_next) {
        pfds[nfds].fd = f->reactor_wait_pid;
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        nfds++;
    }
    for (KaiFiber *f = kai_reactor_socket_write_waiters; f; f = f->reactor_next) {
        pfds[nfds].fd = f->reactor_wait_pid;
        pfds[nfds].events = POLLOUT;
        pfds[nfds].revents = 0;
        nfds++;
    }
    /* Publish "reactor asleep with nothing armed" under the lock, so the
     * deadlock check (which reads it under kai_reactor_mu) sees a coherent
     * value. Cleared the instant poll() returns, before any drain/flush,
     * so a promotion in flight is never mistaken for a quiescent reactor. */
    if (kai_nthreads > 1)
        atomic_store(&kai_reactor_idle, kai_reactor_parked_count == 0 ? 1 : 0);
    kai_reactor_unlock();
    /* Parked workers never retry the deadlock check, so a reactor going
     * quiescent after the last worker parked must run it itself. */
    if (kai_nthreads > 1) kai_sched_check_deadlock();
    if (timeout_ms != 0 && kai_nthreads <= 1) kai_flush_stdout();
    int rc = poll(pfds, (nfds_t) nfds, timeout_ms);
    if (kai_nthreads > 1) atomic_store(&kai_reactor_idle, 0);
    if (rc < 0 && errno != EINTR) {
        kai_eprintf("kai: reactor poll() failed: %s\n", strerror(errno));
        kai_exit(1);
    }

    /* Drain in a fixed order under the lock again — the drains unlink
     * from the shared lists and decrement parked_count, and batch the
     * readied fibers (kai_reactor_mark_ready). Even on EINTR (rc < 0) the
     * timer wheel must be drained because a stray signal could have
     * coincided with a deadline expiry. */
    kai_reactor_lock();
    uint64_t now = kai_reactor_now_ns();
    kai_reactor_timer_drain(now);
    if (rc > 0) {
        /* Self-pipe and stdin paths first (these compare against the
         * fixed fds we know up front). */
        for (int i = 0; i < nfds; i++) {
            if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            if (pfds[i].fd == kai_reactor_sigchld_pipe[0]) {
                kai_reactor_sigchld_drain();
            } else if (pfds[i].fd == kai_reactor_filepool_pipe[0]) {
                kai_reactor_filepool_drain();
            } else if (pfds[i].fd == kai_reactor_signal_pipe[0]) {
                kai_reactor_signal_drain();
            } else if (pfds[i].fd == STDIN_FILENO &&
                       kai_reactor_stdin_waiter != NULL) {
                /* Issue #620 — readiness event on stdin: promote
                 * the parked fiber. POLLHUP / POLLERR also count
                 * as wakeups so a closed pipe (EOF) does not
                 * strand the waiter forever. */
                KaiFiber *f = kai_reactor_stdin_waiter;
                kai_reactor_stdin_waiter = NULL;
                kai_reactor_parked_count--;
                kai_reactor_mark_ready(f);
            }
        }
        /* Socket waiters: separate drain pass because the same fd
         * may appear in both directions (and the read/write masks
         * are disjoint). Each drain handles only the matching list. */
        kai_reactor_socket_drain(&kai_reactor_socket_read_waiters,  pfds, nfds, POLLIN);
        kai_reactor_socket_drain(&kai_reactor_socket_write_waiters, pfds, nfds, POLLOUT);
    }
    /* Timeout-armed recv waiters whose deadline elapsed. Unconditional
     * (like the timer drain): a bare poll timeout with no revents still
     * has to expire them. Runs after the readiness drain above. */
    kai_reactor_socket_read_deadline_drain(now);
    /* A SIGCHLD delivered before we entered poll() may not appear
     * in revents (the signal handler ran but the byte arrived
     * after the kernel snapshot). Always attempt a non-blocking
     * waitpid drain too so children that exited during the
     * micro-window before poll() do not strand their fibers. */
    if (kai_reactor_pid_waiters) {
        kai_reactor_sigchld_drain();
    }
    kai_reactor_unlock();

    if (heap_alloced) free(pfds);

    /* Unpark the batch outside `kai_reactor_mu` (each remote_unpark takes a
     * slot lock — never nested under reactor_mu). No-op at N=1: the drains
     * unparked inline and the batch is empty. */
    kai_reactor_flush_ready();
#elif defined(__wasm__)
    uint64_t wake_dl = kai_reactor_timer_head ? kai_reactor_timer_head->reactor_deadline_ns : 0;
    kai_flush_stdout();
    kai_wctx_mark();
    double slack_ms = kai_wctx_host_wait(wake_dl ? (double) wake_dl / 1e6 : -1.0);
    if (slack_ms < 0) {
        kai_web_wake_waiters();
        slack_ms = 0;
    }
    kai_reactor_timer_drain(kai_reactor_now_ns() + (uint64_t) (slack_ms * 1e6));
#else
    kai_host_absent("reactor");
#endif
}

/* F2 — link a reactor-parking fiber into the waiter structure its park site
 * stamped, and mark it PARKED, both under `kai_reactor_mu`. Runs on the
 * scheduler root via `kai_drain_commit_stack` AFTER the
 * fiber's exit swap finished writing its ctx — never on the fiber's own
 * stack — so the reactor drain can never resume a half-saved context. The
 * self-pipe poke re-arms a reactor already asleep in poll() with a stale
 * timeout (the residual-deadlock fix: a timer linked while poll slept was
 * never drained). Only reached at N>1 (the N=1 park sites link inline). */
static void kai_sched_commit_park(KaiFiber *f) {
    /* Non-reactor park: commit under f's slot lock, not reactor_mu. Its ctx
     * is now saved (the exit swap completed), so publishing it PARKED is
     * safe. Re-check wake_pending: a send that raced the pre-commit window
     * saw us RUNNING and bumped it instead of enqueuing — honour that here
     * by going straight to READY on the steal deque rather than parking. */
    if (f->pending_park == KAI_PARK_SLOT) {
        int home = kai_fiber_slot_lock(f);
        KaiSchedSlot *s = &kai_sched_slots[home];
        /* Clear pending_park before the unlock publishes the new state: a
         * woken fiber can re-park and stamp a fresh reason the instant the
         * state is visible, and a clear landing after that stomps it — the
         * next commit would then park the fiber on no structure at all. */
        f->pending_park = KAI_PARK_NONE;
        if (f->wake_pending > 0) {
            f->wake_pending--;
            f->state = KAI_FIBER_READY;
            f->sched_next = NULL;
            if (s->steal_tail) s->steal_tail->sched_next = f;
            else               s->steal_head = f;
            s->steal_tail = f;
        } else {
            f->state = KAI_FIBER_PARKED;
            atomic_fetch_add(&kai_blocked_fiber_count, 1);
            kai_park_trace(f, "park-slot");
        }
        kai_fiber_slot_unlock_at(home);
        return;
    }
    kai_reactor_lock();
    int reason = f->pending_park;
    int home = kai_fiber_slot_lock(f);
    f->pending_park = KAI_PARK_NONE;
    /* Cancel-vs-park race: the canceller sets cancel_requested and then
     * detaches+unparks under kai_reactor_mu. A cancel that lands in the
     * pre-commit window finds no waiter to detach, so committing this park
     * would sleep through the flag until the reactor event fires. Re-check
     * here and abort the park; the resume path runs the cancel yield-point
     * check. The gate mirrors kai_check_cancel_yield_point so a fiber whose
     * cancel cannot be delivered (already delivered, no pad) still parks. */
    int cancel_abort = kai_cancel_due(f);
    if (f->wake_pending > 0 || cancel_abort) {
        /* A wake raced the pre-commit window (or a stale permit survived
         * an earlier park), or a pending cancel must not be slept through:
         * go READY instead of parking on the reactor. The site observes
         * reactor_fired == 0 and re-parks, or unwinds via the cancel pad. */
        if (f->wake_pending > 0) f->wake_pending--;
        f->state = KAI_FIBER_READY;
        f->sched_next = NULL;
        KaiSchedSlot *s = &kai_sched_slots[home];
        if (s->steal_tail) s->steal_tail->sched_next = f;
        else               s->steal_head = f;
        s->steal_tail = f;
        kai_park_trace(f, cancel_abort ? "commit-abort-cancel" : "commit-abort-wake");
        kai_fiber_slot_unlock_at(home);
        kai_reactor_unlock();
        return;
    }
    f->state = KAI_FIBER_PARKED;
    atomic_fetch_add(&kai_blocked_fiber_count, 1);
    kai_park_trace(f, "park-reactor");
    /* Publish PARKED under the slot lock so wakers (which classify state
     * under that lock) never read a torn transition; the reactor insert
     * below still happens under kai_reactor_mu, and a wake that flips us
     * READY right after the unlock serializes behind it for the disarm. */
    kai_fiber_slot_unlock_at(home);
    switch (reason) {
        case KAI_PARK_TIMER:
            kai_reactor_timer_insert(f);
            break;
        case KAI_PARK_PID:
            f->reactor_next = kai_reactor_pid_waiters;
            kai_reactor_pid_waiters = f;
            break;
        case KAI_PARK_SOCKET_READ:
            f->reactor_next = kai_reactor_socket_read_waiters;
            kai_reactor_socket_read_waiters = f;
            break;
        case KAI_PARK_SOCKET_WRITE:
            f->reactor_next = kai_reactor_socket_write_waiters;
            kai_reactor_socket_write_waiters = f;
            break;
        case KAI_PARK_STDIN:
            kai_reactor_stdin_waiter = f;
            break;
        case KAI_PARK_SIGNAL:
            kai_reactor_signal_waiter = f;
            break;
        case KAI_PARK_FILEPOOL:
            f->reactor_next = kai_reactor_filepool_waiters;
            kai_reactor_filepool_waiters = f;
            break;
        default:
            /* KAI_PARK_NONE should never reach the commit stack. */
            break;
    }
    kai_reactor_parked_count++;
    /* A signo delivered (and drained to `pending`) between the park
     * request and this commit must wake the waiter now — the byte is
     * gone from the self-pipe, so no future poll will re-deliver it. */
    if (reason == KAI_PARK_SIGNAL) kai_reactor_signal_try_wake();
    kai_reactor_unlock();
    kai_reactor_wake();
}

/* Drain the worker's pending-commit stack on its root. Called right after
 * each dispatch swap returns to the root, so every fiber that parked this
 * pass is linked before the loop looks for more work. LIFO order is
 * irrelevant — each fiber lands on its own waiter. */
static void kai_drain_commit_stack(KaiWorker *w) {
    while (w->commit_stack_head) {
        KaiFiber *f = w->commit_stack_head;
        w->commit_stack_head = f->commit_next;
        f->commit_next = NULL;
        kai_sched_commit_park(f);
    }
}

/* Lock the slot that owns fiber `f` and return that slot's index. This
 * guards `f`'s `state` + `awaiters_head` against a concurrent terminate:
 * both the awaiter (check state, link onto the chain) and the target's own
 * trampoline (set state, snapshot the chain) take THIS lock, closing the
 * check-then-link lost-wakeup — an awaiter either observes DONE and skips
 * parking, or is on the chain the terminate walk unparks.
 *
 * `f->home_thread` can change under a work-stealer, so we cannot just lock
 * `slots[f->home_thread]` and trust it: re-read after acquiring and retry
 * if it moved. Once we hold the lock of `f`'s current home, `f` cannot
 * migrate — a thief must take that same slot lock to steal it — so the
 * index stays valid until we release. The trampoline runs on `f`'s own
 * (RUNNING, non-stealable) thread, so it locks the same slot. N=1 has one
 * thread; callers gate these on nthreads>1 (single-thread path unchanged). */
static inline int kai_fiber_slot_lock(KaiFiber *f) {
    for (;;) {
        int home = f->home_thread;
        pthread_mutex_lock(&kai_sched_slots[home].mu);
        if (f->home_thread == home) return home;
        pthread_mutex_unlock(&kai_sched_slots[home].mu);
    }
}
static inline void kai_fiber_slot_unlock_at(int home) {
    pthread_mutex_unlock(&kai_sched_slots[home].mu);
}

/* At N>1 the ready queue lives in the slot (`steal_head`/`steal_tail`),
 * one source of truth shared between the owner (both ends) and thieves
 * (head only), serialized by the slot lock. At N=1 it is the plain TLS
 * FIFO the pre-M:N runtime used — lock-free and byte-identical. */
/* The thread a fiber must stay on, or -1 when it may migrate. */
static int kai_fiber_pinned_to(KaiFiber *f) {
    if (f->pinned_main) return 0;
    return f->seg_pin ? f->home_thread : -1;
}

static void kai_sched_enqueue(KaiFiber *f) {
    KaiWorker *w = kai_worker_here();
    if (kai_nthreads > 1) {
        /* A pinned fiber goes on its thread's deque wherever the enqueue
         * runs from — routing it to the caller's slot would hand it to that
         * thread, which is exactly the migration the pin forbids. */
        int pin = kai_fiber_pinned_to(f);
        int owner = pin >= 0 ? pin : w->id;
        KaiSchedSlot *s = &kai_sched_slots[owner];
        pthread_mutex_lock(&s->mu);
        /* Publish ownership under the same lock that publishes the fiber
         * onto this slot's steal list: a reader that finds `f` here must
         * see home_thread pointing at this slot, or a cross-thread unpark
         * would lock a slot the fiber is not queued on. */
        f->home_thread = owner;
        f->sched_next = NULL;
        int was_empty = s->steal_tail == NULL;
        if (s->steal_tail) s->steal_tail->sched_next = f;
        else               s->steal_head = f;
        s->steal_tail = f;
        pthread_mutex_unlock(&s->mu);
        /* An owner at its root searches next and takes a lone fiber itself;
         * waking a peer to race it for that fiber is a wasted futex trip. */
        if (!(was_empty && owner == w->id && w->sched && w->active == &w->main_fiber))
            kai_sched_notify_work(owner);
        return;
    }
    f->sched_next = NULL;
    if (w->ready_tail) {
        w->ready_tail->sched_next = f;
    } else {
        w->ready_head = f;
    }
    w->ready_tail = f;
}

static KaiFiber *kai_sched_dequeue(KaiWorker *w) {
    if (kai_nthreads > 1) {
        KaiSchedSlot *s = &kai_sched_slots[w->id];
        pthread_mutex_lock(&s->mu);
        KaiFiber *f = s->steal_head;
        if (f) {
            s->steal_head = f->sched_next;
            if (!s->steal_head) s->steal_tail = NULL;
            f->sched_next = NULL;
        }
        pthread_mutex_unlock(&s->mu);
        return f;
    }
    KaiFiber *f = w->ready_head;
    if (!f) return NULL;
    w->ready_head = f->sched_next;
    if (!w->ready_head) w->ready_tail = NULL;
    f->sched_next = NULL;
    return f;
}

/* Steal one fiber from the head of thread `victim`'s ready queue,
 * serialized against that thread's owner by the slot lock. Steal
 * granularity is one fiber (a pointer move). The stolen fiber's
 * home_thread is retargeted to the thief: it now runs, parks, and frees
 * on the thief's thread, so its non-atomic-RC heap stays single-threaded.
 * A fiber only migrates while READY — never mid-run — so nothing on its
 * suspended stack references the victim thread's TLS. */
static KaiFiber *kai_sched_steal_from(int victim, int thief) {
    KaiSchedSlot *s = &kai_sched_slots[victim];
    if (!s->live) return NULL;
    pthread_mutex_lock(&s->mu);
    KaiFiber *f = s->steal_head;
    /* A pinned fiber is not stealable: taking it would run it here. Only
     * the head is a steal candidate, so a pinned head just makes this
     * victim unstealable for now — its owner dequeues it next. A thief is
     * never its own victim, so no thread-id test is needed. */
    if (f && kai_fiber_pinned_to(f) >= 0) f = NULL;
    if (f) {
        s->steal_head = f->sched_next;
        if (!s->steal_head) s->steal_tail = NULL;
        f->sched_next = NULL;
        /* Stamp the new owner under the victim's slot lock so a concurrent
         * remote_unpark that reads home_thread sees a coherent value. */
        f->home_thread = thief;
    }
    pthread_mutex_unlock(&s->mu);
    return f;
}

/* Forward decl: the trampoline drives a fiber's body and walks its
 * awaiter chain on completion. Defined below so it can call the
 * scheduler primitives + kai_apply. */
#if KAI_HOST_FIBERS
static void kai_fiber_trampoline(void);
#endif

/* Fall-off-the-end landing for a fiber context. `uc_link` cannot name a
 * scheduler root directly: the address baked at spawn belongs to the
 * spawning worker, and a stolen fiber would resume a root another thread is
 * running on. Resolve the executing thread's root here instead. */
#if KAI_HOST_FIBERS
static void kai_fiber_uc_link_landing(void);
static ucontext_t *kai_uc_link_target(void);
#endif

/* Phase 5 forward decl: link propagation runs in the trampoline's
 * termination tail; the helper itself is defined alongside the
 * Link default handler further down. */

static void kai_fiber_detach_watchers(KaiFiber *self, KaiExitReason reason,
                                      KaiLinkNode **links, KaiMonitorNode **monitors);
static void kai_link_propagate_terminate(KaiFiber *self, KaiLinkNode *ln,
                                         KaiExitReason reason);
/* Tier 2 Monitor — observers learn about target's termination via a
 * single push of `target_pid` into observer->mailbox. Defined
 * alongside the Monitor default handler further down. */
static void kai_monitor_propagate_terminate(KaiMonitorNode *mn);
/* Eager nursery cancel-on-fail — defined alongside the nursery ops
 * further down. */
static void kai_nursery_propagate_failure(KaiFiber *self);

/* Initialise a freshly-allocated KaiFiber's ucontext + private stack.
 * Sets f->ctx so it can be a swapcontext target; fills uc_link as a
 * fallback (the trampoline always exits via setcontext, so uc_link
 * is reachable only on a runtime bug). The thunk slot must already
 * be filled by the caller before the fiber runs. */
#if defined(__APPLE__) || defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#if KAI_HOST_STACK_GUARD
/* [guard page | stack_size bytes], low to high; the caller munmaps
 * stack_size + one page. MAP_NORESERVE keeps a large stack a pure address
 * reservation: pages are committed only as the stack reaches them. */
static void *kai_stack_map(size_t stack_size) {
    size_t page = kai_page_size();
    size_t total = stack_size + page;
    void *region = mmap(NULL, total, PROT_READ | PROT_WRITE, kai_stack_map_flags(), -1, 0);
    if (region == MAP_FAILED) {
        kai_eprintf("kai: mmap failed allocating fiber stack (%zu bytes)\n",
                total);
        kai_exit(1);
    }
    if (mprotect(region, page, PROT_NONE) != 0) {
        kai_eprintf("kai: mprotect guard page failed for fiber stack\n");
        munmap(region, total);
        kai_exit(1);
    }
    return region;
}
#elif defined(__wasm__)
/* The "guard" is the page-size prefix, which nothing protects here. */
static void *kai_stack_map(size_t stack_size) {
    void *region = malloc(stack_size + kai_page_size());
    if (region == NULL) {
        kai_eprintf("kai: out of memory allocating fiber stack (%zu bytes)\n", stack_size);
        kai_exit(1);
    }
    return region;
}
#endif

#if KAI_HOST_FIBERS
/* Point `c` at the usable stack [lo, lo + size). TRAP (macOS): makecontext
 * bzeroes the whole uc_stack, which commits every page of a large reserved
 * stack up front. Hand it only the top window instead; the frames below grow
 * on into the rest of the mapping, down to the guard page. */
static void kai_ctx_set_stack(ucontext_t *c, void *lo, size_t size) {
#if defined(__APPLE__)
    size_t window = 256 * 1024;
    if (size > window) {
        lo = (char *) lo + (size - window);
        size = window;
    }
#endif
    c->uc_stack.ss_sp   = lo;
    c->uc_stack.ss_size = size;
}
#endif

static void kai_fiber_init_ctx_sized(KaiFiber *f, size_t stack_size) {
#if KAI_HOST_FIBERS
#if defined(__wasm__)
    kai_wctx_mark();
#endif
    kai_install_fiber_sigsegv_handler();
    if (getcontext(&f->ctx) != 0) {
        kai_eprintf("kai: getcontext failed for new fiber\n");
        kai_exit(1);
    }
    {
        size_t ps = kai_page_size();
        if (stack_size % ps != 0) stack_size = ((stack_size / ps) + 1) * ps;
    }
    f->stack_size = stack_size;
    /* Allocate stack + one guard page below it. Stack grows down on
     * x86_64 / arm64, so the lowest address is the overflow target.
     * stack_base points at the guard; the usable region starts one
     * page above. Layout (low → high):
     *   [ guard page (PROT_NONE) | stack_size bytes (RW) ]
     * We store stack_base as the mmap base so munmap covers both;
     * stack_size remains the usable size, and total = stack_size +
     * page_size whenever we need to release the region. */
    void *region = kai_stack_map(f->stack_size);
    f->stack_base = region;
    kai_ctx_set_stack(&f->ctx, (char *) region + kai_page_size(), f->stack_size);
    f->ctx.uc_link          = kai_uc_link_target();
    makecontext(&f->ctx, kai_fiber_trampoline, 0);
#else
    (void) f; (void) stack_size;
    kai_host_absent("fibers");
#endif
}

static void kai_fiber_init_ctx(KaiFiber *f) {
    kai_fiber_init_ctx_sized(f, kai_fiber_stack_size());
}

