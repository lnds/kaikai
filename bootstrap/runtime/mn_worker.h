/* Fragment of runtime.h; include runtime.h, never this file. */
/* ==================================================================
 * M:N work-stealing core — the per-thread scheduler loop.
 * docs/mn-scheduler-design.md §2, §4. Off at N=1.
 * ================================================================== */

/* Find one runnable fiber for this thread: its own queue first (LIFO
 * locality preserved by the FIFO order the owner maintains), then a
 * round-robin steal sweep over the other live threads. Returns NULL when
 * nothing is runnable anywhere this instant. */
static KaiFiber *kai_worker_find_work(KaiWorker *w) {
    KaiFiber *f = kai_sched_dequeue(w);
    if (f) return f;
    for (int i = 1; i < kai_nthreads; i++) {
        int victim = (w->id + i) % kai_nthreads;
        f = kai_sched_steal_from(victim, w->id);
        if (f) return f;
    }
    return NULL;
}

/* Hand worker `tid` a permit if it is parked. Returns 1 only when this call
 * delivered a fresh permit; 0 means `tid` is awake or already holds one, and
 * either way it searches the deques again before it can sleep. */
static int kai_sched_wake_thread(int tid) {
    KaiSchedSlot *s = &kai_sched_slots[tid];
    if (!atomic_load(&s->parked)) return 0;
    if (atomic_exchange(&s->permit, 1)) return 0;
    pthread_mutex_lock(&s->park_mu);
    pthread_cond_signal(&s->park_cv);
    pthread_mutex_unlock(&s->park_mu);
    return 1;
}

/* A fiber just landed on `slot`'s deque (the push's slot lock is already
 * released): wake the slot's owner if it is parked — only the owner can run
 * a main-pinned fiber — else leave it to a spinning worker, else wake one
 * parked peer to steal it; a busy owner would otherwise leave it queued
 * behind its current fiber. Nobody parked costs one load. */
static void kai_sched_notify_work(int slot) {
    if (atomic_load(&kai_sched_sleepers) == 0) return;
    int me = kai_worker_here()->id;
    if (slot != me && kai_sched_wake_thread(slot)) return;
    if (atomic_load(&kai_sched_spinning) > 0) return;
    for (int i = 1; i <= kai_nthreads; i++) {
        int t = (me + i) % kai_nthreads;
        if (t == me || t == slot) continue;
        if (kai_sched_wake_thread(t)) return;
    }
}

/* Shutdown: every parked worker re-reads the flag under its park lock. */
static void kai_sched_wake_all(void) {
    for (int i = 0; i < kai_nthreads; i++) {
        KaiSchedSlot *s = &kai_sched_slots[i];
        pthread_mutex_lock(&s->park_mu);
        pthread_cond_signal(&s->park_cv);
        pthread_mutex_unlock(&s->park_mu);
    }
}

/* Promote a fiber owned by another thread from PARKED to READY and make
 * it runnable on its home thread, then wake a worker for it. Called from a
 * cross-thread mailbox send (the sender runs here, the receiver's home
 * is elsewhere). The state flip is serialized on the target's slot lock
 * so it composes with the target's own park path (which sets PARKED
 * under the same lock via kai_sched_park_remote_safe below). */
static void kai_sched_remote_unpark(KaiFiber *target) {
    if (!target) return;
    /* Lock the target's CURRENT home slot (retry loop — home_thread can
     * move under a work-stealer between a bare read and the acquisition).
     * Locking a stale slot would race the park commit's state/wake_pending
     * transitions, which run under the current slot's lock: the classic
     * outcome is a wake recorded where the commit never looks — a lost
     * wakeup that strands the fiber. */
    int home = kai_fiber_slot_lock(target);
    KaiSchedSlot *s = &kai_sched_slots[home];
    if (target->state == KAI_FIBER_PARKED) {
        target->state = KAI_FIBER_READY;
        target->sched_next = NULL;
        if (s->steal_tail) s->steal_tail->sched_next = target;
        else               s->steal_head = target;
        s->steal_tail = target;
        kai_fiber_slot_unlock_at(home);
        atomic_fetch_sub(&kai_blocked_fiber_count, 1);
        kai_park_trace(target, "unpark-parked");
        kai_sched_notify_work(home);
    } else if (target->state == KAI_FIBER_DONE ||
               target->state == KAI_FIBER_CANCELLED) {
        /* Terminal: nothing to wake, and the struct may be about to be
         * reclaimed — do not write a permit into it. */
        kai_fiber_slot_unlock_at(home);
        kai_park_trace(target, "unpark-terminal");
    } else {
        /* RUNNING (pre-commit window) or READY (already waking): record a
         * permit. The next park commit consumes it and the site re-checks
         * its predicate, so a stale permit costs one spurious resume. */
        target->wake_pending++;
        kai_fiber_slot_unlock_at(home);
        kai_park_trace(target, "unpark-permit");
    }
}

/* Every deque empty? Read each slot's ready head under its own lock so a
 * fiber a reactor flush is mid-enqueue is seen rather than raced past.
 * Takes one slot lock at a time, so it respects the reactor_mu → slot
 * order whether or not the caller holds kai_reactor_mu. */
static int kai_all_deques_empty(void) {
    for (int i = 0; i < kai_nthreads; i++) {
        KaiSchedSlot *s = &kai_sched_slots[i];
        pthread_mutex_lock(&s->mu);
        KaiFiber *head = s->steal_head;
        pthread_mutex_unlock(&s->mu);
        if (head) return 0;
    }
    return 1;
}

/* Global-quiescence deadlock check — the M:N analogue of the single-thread
 * dispatch loop's "run queue empty with fibers parked". A worker runs it as
 * it parks and the reactor as it goes quiescent, so whichever of them
 * completes the wedge sees it. It fires only when the whole machine is
 * wedged: every worker idle, the reactor asleep with nothing armed, every
 * deque empty, and at least one fiber PARKED. That state is terminal — no fiber or reactor event can leave
 * it — so it is a true deadlock, not a transient lull, and the report needs
 * no timeout.
 *
 * The race the check must survive — "I saw everyone idle; a peer just made a
 * fiber runnable" — is closed by construction. The only producers of new
 * runnable work are a RUNNING fiber (spawn / unpark / awaiter wake) and a
 * reactor promotion. All workers idle ⇒ no fiber is RUNNING; kai_reactor_idle
 * ⇒ the reactor is blocked in poll() with nothing to fire. With both true no
 * producer can act, so the observed quiescence cannot dissolve under us. The
 * confirmation runs under kai_reactor_mu (which serializes reactor drains and
 * the reactor branch of commit_park) and scans the deques under their slot
 * locks, so a fiber a flush is mid-handback is counted, never missed. */
static void kai_sched_check_deadlock(void) {
    /* Read before the idle count: a worker leaves the pool by dropping the
     * count and THEN bumping the epoch, so a worker this check still counts
     * idle cannot have bumped it yet. */
    unsigned epoch = atomic_load(&kai_sched_idle_epoch);
    /* Cheap lock-free gate so the reactor_mu acquisition below stays rare —
     * only an already-terminal-looking observation pays for the confirm. */
    if (atomic_load(&kai_sched_idle_count) != kai_nthreads) return;
    if (!atomic_load(&kai_reactor_idle)) return;
    if (atomic_load(&kai_blocked_fiber_count) <= 0) return;
    if (kai_sched_shutting_down) return;

    pthread_mutex_lock(&kai_reactor_mu);
    int wedged = atomic_load(&kai_sched_idle_count) == kai_nthreads
              && atomic_load(&kai_reactor_idle)
              && kai_reactor_parked_count == 0
              && atomic_load(&kai_blocked_fiber_count) > 0
              && !kai_sched_shutting_down
              && kai_all_deques_empty()
              /* All idle does not stop a woken worker from leaving the pool
               * and dequeuing between the reads above, which would make a
               * queued fiber look like an empty deque. */
              && atomic_load(&kai_sched_idle_epoch) == epoch;
    pthread_mutex_unlock(&kai_reactor_mu);
    if (!wedged) return;

    /* Wedged is a whole-scheduler state, so every idle worker observes it at
     * once. Claim the report with a CAS: the winner prints and exits, the
     * losers nap until that exit() lands. No mutex — the reporter must not be
     * able to block on a peer in the very state it is reporting, and a loser
     * spinning here holds nothing a peer could need. */
    int unreported = 0;
    if (!atomic_compare_exchange_strong(&kai_deadlock_reported, &unreported, 1)) {
        for (;;) {
            struct timespec nap = { 0, 200 * 1000 };
            nanosleep(&nap, NULL);
        }
    }

    kai_eprintf(
        "kai: all workers idle with fibers parked (%d parked) — deadlock\n",
        atomic_load(&kai_blocked_fiber_count));
    kai_park_trace_dump();
    kai_exit(1);
}

#ifdef KAI_CELL_POOL_ACTIVE
/* Wait out one grace period on the park lock; true when nothing woke the
 * worker meanwhile. */
static int kai_worker_idle_grace(KaiSchedSlot *s) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += KAI_CELL_POOL_IDLE_NS;
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    int rc = 0;
    while (!atomic_load(&s->permit) && !kai_sched_shutting_down && rc == 0)
        rc = pthread_cond_timedwait(&s->park_cv, &s->park_mu, &until);
    return rc != 0 && !atomic_load(&s->permit);
}

/* Called with the park lock held. A worker idle for a whole grace period
 * gives its cell pool back (lock dropped meanwhile so wakers never wait on
 * the free); a short park keeps the pool hot. A second grace period lets the
 * allocator release pages once every peer has freed its own pool. */
static void kai_worker_idle_trim(KaiSchedSlot *s) {
    if (!kai_cell_pool_trimmable() || !kai_worker_idle_grace(s)) return;
    pthread_mutex_unlock(&s->park_mu);
    kai_cell_pool_trim();
    pthread_mutex_lock(&s->park_mu);
    if (kai_worker_idle_grace(s)) kai_cell_pool_relief();
}
#endif

/* Park this worker until a waker hands it a permit or shutdown begins.
 * Returns a fiber the final recheck found, else NULL once woken.
 *
 * No lost wakeup: `parked` and the sleeper count are raised BEFORE the
 * recheck locks each slot, and every producer reads them AFTER pushing under
 * a slot lock. That lock orders each push against the recheck, so a push is
 * either seen by the recheck or its producer sees this worker parked and
 * delivers a permit. A permit is consumed by exchange, which acquires the
 * push of every waker that found it already pending. */
static KaiFiber *kai_worker_park(KaiWorker *w) {
    KaiSchedSlot *s = &kai_sched_slots[w->id];
    atomic_store(&s->parked, 1);
    atomic_fetch_add(&kai_sched_sleepers, 1);
    KaiFiber *f = kai_worker_find_work(w);
    if (!f) {
        /* Idle only once the recheck came up empty: a worker holding an
         * undispatched fiber must never count as idle to the deadlock check. */
        /* The last worker to go idle flushes: bytes printed on another
         * worker after the reactor armed would otherwise wait for the next
         * wake. */
        if (atomic_fetch_add(&kai_sched_idle_count, 1) + 1 == kai_nthreads) kai_flush_stdout();
        kai_sched_check_deadlock();
        pthread_mutex_lock(&s->park_mu);
#ifdef KAI_CELL_POOL_ACTIVE
        kai_worker_idle_trim(s);
#endif
        while (!atomic_load(&s->permit) && !kai_sched_shutting_down)
            pthread_cond_wait(&s->park_cv, &s->park_mu);
        pthread_mutex_unlock(&s->park_mu);
        atomic_fetch_sub(&kai_sched_idle_count, 1);
        atomic_fetch_add(&kai_sched_idle_epoch, 1);
    }
    int woken = atomic_exchange(&s->permit, 0);
    atomic_fetch_sub(&kai_sched_sleepers, 1);
    atomic_store(&s->parked, 0);
    /* The recheck already found work, so a permit that raced it was meant
     * for a fiber still queued: pass it on rather than leave that fiber
     * waiting behind this one. */
    if (f && woken) kai_sched_notify_work(w->id);
    return f;
}

/* Search rounds a worker spends before parking. A worker that just ran out
 * of work often gets the next fiber within microseconds (a reply, a wake),
 * and catching it here spares the producer a futex wake and this worker a
 * sleep/wake round trip. Bounded, so an idle program still parks at once. */
#define KAI_SPIN_ROUNDS 32

/* Search for work for a bounded spell, yielding the CPU between rounds so an
 * oversubscribed host still runs the thread that would produce the work.
 * Producers skip their wake while anyone spins (kai_sched_notify_work), so
 * the last spinner out owes them: leaving empty-handed, it parks and its
 * recheck sees any skipped push; leaving with a fiber, it wakes a parked
 * peer if work is still queued. A push the scan below misses landed after
 * this spinner left the count, so its producer did not count on it. */
static KaiFiber *kai_worker_spin(KaiWorker *w) {
    atomic_fetch_add(&kai_sched_spinning, 1);
    KaiFiber *f = NULL;
    for (int i = 0; i < KAI_SPIN_ROUNDS && !f && !kai_sched_shutting_down; i++) {
        sched_yield();
        f = kai_worker_find_work(w);
    }
    int last = atomic_fetch_sub(&kai_sched_spinning, 1) == 1;
    if (f && last && !kai_all_deques_empty()) kai_sched_notify_work(w->id);
    return f;
}

/* The per-thread scheduler loop, run ON the worker's root fiber
 * (the OS-thread context). A root never changes thread, so it holds `w`
 * across every switch. A worker thread enters here after startup; the
 * main thread enters it only implicitly (its main_fiber IS the program's
 * initial context, and park/trampoline swap back here when the local
 * queue drains). The loop: run everything runnable locally or stolen; when
 * nothing is runnable anywhere, spin briefly, then block in kai_worker_park
 * until a producer or shutdown wakes it. Exits when the program has terminated (shutdown
 * flag set) — only the workers exit here; the main thread's loop returns
 * control to `main` via the root fiber finishing. */
static void kai_worker_loop(KaiWorker *w) {
    KaiFiber *root = &w->main_fiber;
    KAI_TSAN_BIND_ROOT(w);
    while (!kai_sched_shutting_down) {
        KaiFiber *next = kai_worker_find_work(w);
        if (!next) next = kai_worker_spin(w);
        if (!next) next = kai_worker_park(w);
        if (!next) continue;
        next->state = KAI_FIBER_RUNNING;
        kai_worker_run(w, next);
        KAI_TSAN_SWITCH_TO(next);
#if KAI_HOST_FIBERS
        swapcontext(&root->ctx, &next->ctx);
#else
        kai_host_absent("fibers");
#endif
        w->active = root;
        kai_drain_pending_free(w);
        /* F2 — from the root context, after the exit
         * swap above saved the fiber's ctx: link any fiber that
         * reactor-parked into the wheel/waiter list, and requeue any
         * fiber that yielded onto the steal list. Both are unsafe to do
         * on the fiber's own stack (a thief/reactor could resume a
         * half-saved ctx), so they are deferred to here. */
        kai_drain_commit_stack(w);
        kai_drain_requeue_stack(w);
    }
}

/* Yield: caller stays READY and goes back on the run queue; control
 * swaps to the head of the queue. No-op if the queue is empty (caller
 * is the only ready fiber, nothing to switch to). */
static void kai_sched_yield(void) {
    KaiWorker *w = kai_worker_here();
    KaiFiber *current = w->active;

    /* M:N: never enqueue `current` on the steal list before its ctx is
     * saved — a thief could resume a half-written context. Defer the
     * requeue to the root (post-swap) via the requeue stack, and hand
     * control to this thread's scheduler loop, which drains the stack and
     * dispatches the next runnable fiber. No fiber→fiber shortcut: the
     * publish-after-swap invariant is worth the root trip. If nothing else
     * is runnable this instant, the loop simply redispatches `current`. */
    if (kai_nthreads > 1) {
        current->state = KAI_FIBER_READY;
        current->commit_next  = w->requeue_stack_head;
        w->requeue_stack_head = current;
        w->active = &w->main_fiber;
        KAI_TSAN_SWITCH_TO_ROOT(w);
#if KAI_HOST_FIBERS
        swapcontext(&current->ctx, &w->main_fiber.ctx);
#else
        kai_host_absent("fibers");
#endif
        kai_drain_pending_free(current->worker);
        return;
    }

    KaiFiber *next = kai_sched_dequeue(w);
    if (!next) return;  /* alone — nothing to yield to */
    current->state = KAI_FIBER_READY;
    kai_sched_enqueue(current);
    next->state = KAI_FIBER_RUNNING;
    kai_worker_run(w, next);
    KAI_TSAN_SWITCH_TO(next);
#if KAI_HOST_FIBERS
    swapcontext(&current->ctx, &next->ctx);
#else
    kai_host_absent("fibers");
#endif
    /* If the fiber that switched to us was the tail of a now-discarded
     * fiber, its struct and stack wait in pending_free. */
    kai_drain_pending_free(current->worker);
}

/* Drain the worker's pending-requeue stack on its root: each yielded
 * fiber's exit swap has completed, so it is now safe to publish it to the
 * steal list. */
static void kai_drain_requeue_stack(KaiWorker *w) {
    while (w->requeue_stack_head) {
        KaiFiber *f = w->requeue_stack_head;
        w->requeue_stack_head = f->commit_next;
        f->commit_next = NULL;
        kai_sched_enqueue(f);
    }
}

/* Park: caller goes PARKED, control swaps to the head of the run
 * queue. The caller must have already linked itself into a wakeup
 * list (awaiter chain, mailbox waiter list, or a reactor structure)
 * before calling park — otherwise no fiber can ever unpark it.
 *
 * Issue #611 — Phase R1 reactor integration: when the ready queue
 * is empty but at least one fiber is parked on the reactor (timer
 * wheel, pid waiter, file-pool waiter), block in `kai_reactor_wait`
 * until an event promotes someone, then redequeue. Deadlock is now
 * `ready queue empty AND reactor empty` — only that combination
 * means no path to forward progress. */
static void kai_sched_park_linked(void) {
    KaiWorker *w = kai_worker_here();
    KaiFiber *current = w->active;
    /* Any prior park's fired stamp is history; the sites read it only for
     * THIS park's resume. Safe here: the fiber is not linked into any
     * reactor structure a drain could fire from (N>1 links at commit;
     * N=1 links inline but is single-threaded). */
    current->reactor_fired = 0;

    /* M:N path: a parking fiber returns to this thread's scheduler loop
     * — never runs poll() or a condvar wait on its own
     * stack (asu invariant: nothing that blocks the OS thread runs on a
     * user fiber's stack).
     *
     * Both reactor parks (pending_park already stamped by the site) and
     * non-reactor parks (mailbox recv, await, send-block) yield
     * UNCONDITIONALLY to the root and let it mark PARKED — from the root's
     * stack, after this swap has finished saving our ctx. Setting PARKED
     * inline, before the save, would let a cross-thread remote_unpark flip
     * us READY onto the steal deque while our ctx is still being written,
     * and a thief could then resume a half-saved context (garbage RIP).
     * State stays RUNNING across the swap; a wake that races the commit
     * takes remote_unpark's wake_pending path, which kai_sched_commit_park
     * re-checks (reactor reasons under reactor_mu, KAI_PARK_SLOT under the
     * slot lock) before it commits. */
    if (kai_nthreads > 1) {
        int was_reactor = (current->pending_park != KAI_PARK_NONE &&
                           current->pending_park != KAI_PARK_SLOT);
        if (!current->pending_park) current->pending_park = KAI_PARK_SLOT;
        current->commit_next = w->commit_stack_head;
        w->commit_stack_head = current;
        w->active = &w->main_fiber;
        KAI_TSAN_SWITCH_TO_ROOT(w);
#if KAI_HOST_FIBERS
        swapcontext(&current->ctx, &w->main_fiber.ctx);
#else
        kai_host_absent("fibers");
#endif
        kai_drain_pending_free(current->worker);
        /* A reactor park that resumes without the reactor's own splice
         * (a message wake off the recv chain, a consumed permit, or a
         * stale unpark) may still be linked into the wheel or a waiter
         * list. Splice it out now, so a re-park or a return can never
         * leave a dangling reactor link behind — the double-insert that
         * corrupts the wheel's list structure. */
        if (was_reactor && !current->reactor_fired) {
            if (kai_reactor_detach_fiber(current))
                kai_park_trace(current, "self-detach");
        }
        return;
    }

    /* Mark the caller PARKED up front so the reactor drain helpers
     * (which may run inside kai_reactor_wait below) can promote us
     * back to READY via kai_sched_unpark. The unpark path bails out
     * when state != PARKED, so a fiber whose state is still RUNNING
     * when its deadline fires would be lost. */
    current->state = KAI_FIBER_PARKED;
    w->parked_count++;

    KaiFiber *next = kai_sched_dequeue(w);
    while (!next) {
        if (kai_reactor_parked_count > 0) {
            kai_reactor_wait();
            next = kai_sched_dequeue(w);
            continue;
        }
        kai_eprintf(
            "kai: deadlock — fiber parked with empty run queue (%d parked total)\n",
            w->parked_count);
        kai_exit(1);
    }
    if (next == current) {
        /* The reactor wake promoted us before we picked anyone else.
         * Skip the swapcontext (we are still on our own stack) and
         * unwind the parked accounting we just bumped. */
        current->state = KAI_FIBER_RUNNING;
        w->parked_count--;
        return;
    }
    next->state = KAI_FIBER_RUNNING;
    kai_worker_run(w, next);
    KAI_TSAN_SWITCH_TO(next);
#if KAI_HOST_FIBERS
    swapcontext(&current->ctx, &next->ctx);
#else
    kai_host_absent("fibers");
#endif
    kai_drain_pending_free(current->worker);
}

/* Every resume is a yield point: a fiber cancelled while parked was woken
 * to observe it here, before the site retries what the park wrapped. The
 * check longjmps to the cancel pad when the cancel is due. */
static void kai_sched_park(void) {
    kai_sched_park_linked();
    kai_check_cancel_yield_point();
}

/* Unpark: target PARKED → READY, enqueue at run queue tail. Caller
 * stays RUNNING (does not yield); the unparked fiber runs whenever
 * the scheduler reaches it. No-op if target is not currently
 * PARKED (defensive against double-unpark).
 *
 * Under M:N the target may be owned by another thread (it was stolen, or
 * this is a same-thread wake of a fiber whose home is elsewhere): route
 * through the slot-locked remote path, which flips its state on its own
 * slot and wakes its home thread. A target on this very thread takes the
 * remote path too — same slot, same lock — so there is one unpark path
 * at N>1 and the lost-wakeup guard (wake_pending) applies uniformly. */
static void kai_sched_unpark(KaiFiber *target) {
    if (!target) return;
    if (kai_nthreads > 1) { kai_sched_remote_unpark(target); return; }
    if (target->state != KAI_FIBER_PARKED) return;
    target->state = KAI_FIBER_READY;
    kai_worker_here()->parked_count--;
    kai_sched_enqueue(target);
}

/* At exit a queued fiber that never started holds only its spawn's
 * references, so the ledger releases them and counts it apart; one that did
 * start has live frames and is only counted. `cancel_pad_set` is clear
 * exactly until the trampoline starts the body. */
static KaiFiber *kai_exit_queue_take(KaiFiber **head, KaiFiber **tail) {
    KaiFiber *f = *head;
    *head = NULL;
    *tail = NULL;
    return f;
}

static KaiExitFibers kai_rc_settle_exit_fibers(void) {
    static KaiExitFibers settled;
    static int done;
    if (done) return settled;
    done = 1;
    KaiFiber *queues[KAI_MAX_THREADS];
    int nq = 0;
    if (kai_nthreads > 1) {
        for (int i = 0; i < kai_nthreads; i++) {
            KaiSchedSlot *sl = &kai_sched_slots[i];
            pthread_mutex_lock(&sl->mu);
            queues[nq++] = kai_exit_queue_take(&sl->steal_head, &sl->steal_tail);
            pthread_mutex_unlock(&sl->mu);
        }
        settled.started = atomic_load(&kai_blocked_fiber_count);
    } else {
        KaiWorker *w = kai_worker_here();
        queues[nq++] = kai_exit_queue_take(&w->ready_head, &w->ready_tail);
        settled.started = w->parked_count;
    }
    for (int q = 0; q < nq; q++) {
        for (KaiFiber *f = queues[q], *next; f; f = next) {
            next = f->sched_next;
            f->sched_next = NULL;
            if (f->cancel_pad_set) { settled.started++; continue; }
            settled.unstarted++;
            f->state = KAI_FIBER_CANCELLED;
            kai_decref(f->value);
        }
    }
    return settled;
}

/* Trampoline: the entry point makecontext installs on every spawned
 * fiber's stack. Reads its worker's active fiber to find itself (set by
 * the dispatcher who swapped in), runs the thunk, walks awaiters, and
 * hands control to the next ready fiber via setcontext.
 *
 * Phase 3 — Cancel landing: setjmp(cancel_pad) before the body runs.
 * The yield-point hook in kai_evidence_lookup* longjmps here when
 * cancel_requested fires; the second-return path skips the body and
 * marks the fiber CANCELLED instead of DONE. cancel_pad_set is the
 * gate the hook reads before attempting the longjmp.
 *
 * RC contract: f->thunk is owned by f for f's entire lifetime and the
 * trampoline only borrows it. kai_free_value's KAI_FIBER branch decrefs
 * both thunk and result when f's RC drops. */
#if KAI_HOST_FIBERS
static void kai_fiber_uc_link_landing(void) {
    KaiWorker *w = kai_worker_here();
    w->active = &w->main_fiber;
    KAI_TSAN_SWITCH_TO_ROOT(w);
    setcontext(&w->main_fiber.ctx);
    kai_eprintf("kai: fiber uc_link landing failed to reach the scheduler root\n");
    kai_exit(1);
}

/* Per-worker context whose entry point is the landing above. Every fiber
 * spawned on this worker links here, so a fall-off-the-end resolves the
 * root of whichever thread is executing rather than the spawner's. */
static ucontext_t *kai_uc_link_target(void) {
    KaiWorker *w = kai_worker_here();
    if (!w->uc_link_ready) {
        if (getcontext(&w->uc_link_ctx) != 0) return &w->main_fiber.ctx;
        w->uc_link_ctx.uc_stack.ss_sp   = w->uc_link_stack;
        w->uc_link_ctx.uc_stack.ss_size = sizeof(w->uc_link_stack);
        w->uc_link_ctx.uc_link          = NULL;
        makecontext(&w->uc_link_ctx, kai_fiber_uc_link_landing, 0);
        w->uc_link_ready = 1;
    }
    return &w->uc_link_ctx;
}

static void kai_nursery_abandon(KaiFiber *f);
static void kai_root_nursery_join(KaiNursery *scope, int mode);

static void kai_fiber_trampoline(void) {
    KaiWorker *w = kai_worker_here();
    KaiFiber *self = w->active;
    /* First entry follows a setcontext from another fiber's
     * trampoline tail or a swap from yield/park. Drain any pending
     * struct free left behind by the previous fiber's
     * `kai_decref(self->value)` before we touch our own state. */
    kai_drain_pending_free(w);
    self->unwind_frame = (uintptr_t) __builtin_frame_address(0);
    /* Lives in this frame: the cancel landing below reads it after the body's
     * own frames are gone. */
    KaiNursery body = { .root = 1, .body = 1, .reap_at = KAI_ROOT_REAP_MIN };
    if (setjmp(self->cancel_pad) == 0) {
        self->cancel_pad_set = 1;
        self->nursery_top = &body;
        /* Borrowed: a cancel longjmps past any release after the call. */
        self->result = kai_apply_borrow(self->thunk, 0, NULL);
        self->nursery_top = NULL;
        kai_root_nursery_join(&body, KAI_JOIN_INTERRUPTIBLE);
        self->cancel_pad_set = 0;
        /* Before the terminal state: once it is stored another thread
         * may read the result, which can share cells with a constant. */
        kai_fiber_consts_drop(self);
        self->state  = KAI_FIBER_DONE;
    } else {
        /* Cancel or trap landing: the yield-point hook, the default
         * Cancel handler, or kai_trap_abort longjmped here. The body
         * did not finish; result stays NULL. `trapped` distinguishes
         * a runtime trap (a bug) from cooperative cancellation.
         * Sibling cancellation must precede the terminal state store:
         * the store is what lets the nursery owner join this fiber,
         * finish its drain, and free the scope. */
        self->cancel_pad_set = 0;
        kai_fiber_consts_drop(self);
        kai_nursery_abandon(self);
        kai_nursery_propagate_failure(self);
        self->state = KAI_FIBER_CANCELLED;
    }

    /* Phase 5 — Link propagation. Walk the linked chain. For each
     * peer, behaviour depends on the peer's trap_exit flag (Tier 2):
     *   - trap_exit=0 (default): set cancel_requested (current
     *     behaviour, delivered at the peer's next yield-point hook).
     *   - trap_exit=1: push a "Normal"/"Crashed"/"Trapped" string
     *     into the peer's mailbox so the peer can react in user code
     *     instead of being cancelled.
     * The exit reason: DONE → Normal, trapped → Trapped, otherwise
     * CANCELLED → Crashed. */
    KaiExitReason   exit_reason = self->state == KAI_FIBER_DONE ? KAI_EXIT_NORMAL
                                : self->trapped ? KAI_EXIT_TRAPPED : KAI_EXIT_CRASHED;
    KaiLinkNode    *links;
    KaiMonitorNode *monitors;
    kai_fiber_detach_watchers(self, exit_reason, &links, &monitors);
    kai_link_propagate_terminate(self, links, exit_reason);

    /* Tier 2 Monitor — push our pid into each observer's mailbox.
     * Observers do not get cancel_requested set; monitors are
     * unidirectional and fault-isolated. */
    kai_monitor_propagate_terminate(monitors);

    /* Wake awaiters. Each was parked in Spawn.await / Spawn.select /
     * nursery_join. Snapshot the chain under this fiber's slot lock so it
     * is atomic against a concurrent awaiter's check-then-link (both sides
     * take this lock): `state` was set DONE/CANCELLED above, so an awaiter
     * that acquires the lock after this snapshot sees the terminal state
     * and never parks, while one that linked before the snapshot is on the
     * chain we walk here. Clear each awaiter's next-link before unparking
     * so a re-park does not see stale links. */
    KaiFiber  *wake_stack[8];
    KaiFiber **wake = wake_stack;
    int        wake_n = 0;
    int self_hl = 0;
    if (kai_nthreads > 1) self_hl = kai_fiber_slot_lock(self);
    /* Snapshot AND clear every awaiter's next-link under the lock: an
     * awaiter that observes our terminal state and returns re-uses its
     * awaiters_next for its next wait, so the link must not be read or
     * written outside this critical section. */
    {
        int n = 0;
        for (KaiFiber *c = self->awaiters_head; c; c = c->awaiters_next) n++;
        if (n > 8) {
            wake = (KaiFiber **) malloc((size_t) n * sizeof(KaiFiber *));
            if (!wake) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
        }
        KaiFiber *a = self->awaiters_head;
        while (a) {
            KaiFiber *nx = a->awaiters_next;
            a->awaiters_next = NULL;
            wake[wake_n++] = a;
            a = nx;
        }
        self->awaiters_head = NULL;
    }
    /* Selectors parked on us. Each node lives on its selector's stack
     * frame, so the chain must be walked to fibers HERE, under the lock:
     * once a selector is unparked it can win, return, and reclaim its
     * frame, and a `sel->next` read after that is a use-after-free.
     * Collecting the fibers first means the unpark loop below touches no
     * node memory. Each selector re-scans every candidate on resume, so
     * detaching the chain is all we owe it. */
    KaiFiber  *swake_stack[8];
    KaiFiber **swake = swake_stack;
    int        swake_n = 0;
    {
        int n = 0;
        for (KaiSelectWaiter *c = self->select_waiters_head; c; c = c->next) n++;
        if (n > 8) {
            swake = (KaiFiber **) malloc((size_t) n * sizeof(KaiFiber *));
            if (!swake) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
        }
        KaiSelectWaiter *s = self->select_waiters_head;
        while (s) {
            KaiSelectWaiter *nx = s->next;
            s->next = NULL;
            swake[swake_n++] = s->waiter;
            s = nx;
        }
        self->select_waiters_head = NULL;
    }
    if (kai_nthreads > 1) kai_fiber_slot_unlock_at(self_hl);
    for (int i = 0; i < swake_n; i++) kai_sched_unpark(swake[i]);
    if (swake != swake_stack) free(swake);
    for (int i = 0; i < wake_n; i++) kai_sched_unpark(wake[i]);
    if (wake != wake_stack) free(wake);

    /* R4 fix — drop the scheduler-side incref on our wrapper, taken
     * by `kai_default_spawn_spawn` before enqueue. Pairs `enqueue`
     * with this single decref.
     *
     * At N>1 we must NOT drop it inline: the tail keeps running on this
     * fiber's private stack below (dequeue + setcontext), and a peer thread
     * holding the last other ref (a discarded Fiber[T] handle) could take RC
     * to 0 and munmap this stack the instant we drop ours. Stash the ref so
     * RC stays >= 1 across the tail; the next context drops it once the
     * setcontext has left this stack (folded into the pending-free drain).
     * At N=1 there is no peer thread, so decref inline — if RC hits 0,
     * kai_free_value defers the struct/stack free into kai_pending_free (we
     * are still on this stack) and the next drain reaps it; byte-identical to
     * the pre-M:N path. */
    w = self->worker;
    if (self->value) {
        if (kai_nthreads > 1) w->pending_sched_drop = self->value;
        else                  kai_decref(self->value);
    }

    /* Hand control to the next ready fiber. Awaiters we just woke
     * are now in the queue; the FIFO discipline picks the oldest
     * ready (which may be one of them, or main, or a sibling).
     *
     * Issue #611 — if the queue is empty here but reactor waiters
     * remain (timer wheel, pid map, file-pool list), block in
     * kai_reactor_wait until a wake event promotes someone before
     * declaring deadlock. */
    /* M:N — a finished fiber with no local successor returns to its
     * worker's scheduler loop, which steals or idles. We setcontext
     * explicitly so the loop resumes at its swap point rather than falling
     * off the end into the uc_link landing. This frame spans the fiber's
     * whole body, so `w` was re-read from `self` above, not kept from the
     * prologue: the fiber may have been stolen since. */
    if (kai_nthreads > 1) {
        KaiFiber *next = kai_sched_dequeue(w);
        if (next) {
            next->state = KAI_FIBER_RUNNING;
            kai_worker_run(w, next);
            KAI_TSAN_SWITCH_TO(next);
            setcontext(&next->ctx);
        }
        w->active = &w->main_fiber;
        KAI_TSAN_SWITCH_TO_ROOT(w);
        setcontext(&w->main_fiber.ctx);
        /* setcontext does not return. */
    }

    KaiFiber *next = kai_sched_dequeue(w);
    while (!next) {
        if (kai_reactor_parked_count > 0) {
            kai_reactor_wait();
            next = kai_sched_dequeue(w);
            continue;
        }
        kai_eprintf(
            "kai: fiber finished with empty run queue (%d parked) — deadlock\n",
            w->parked_count);
        kai_exit(1);
    }
    next->state = KAI_FIBER_RUNNING;
    kai_worker_run(w, next);
    KAI_TSAN_SWITCH_TO(next);
    setcontext(&next->ctx);
    /* setcontext does not return. */
}
#endif
#if defined(__APPLE__) || defined(__clang__)
#  pragma clang diagnostic pop
#endif

