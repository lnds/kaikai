/* Fragment of runtime.h; include runtime.h, never this file. */
/* ==================================================================
 * M:N bootstrap — start the worker pool and run `kai_main` as a fiber.
 * ================================================================== */

/* The user's `kai_main` (void→KaiValue*), captured by kai_sched_bootstrap
 * so the root fiber's trampoline can invoke it. Immutable after startup. */
#if defined(KAI_SEPARATE_COMPILATION)
extern KaiValue *(*kai_user_main_fn)(void);
extern KaiValue  *kai_user_main_result;
#  if defined(KAI_RUNTIME_OWNER)
KaiValue *(*kai_user_main_fn)(void) = NULL;
KaiValue  *kai_user_main_result = NULL;
#  endif
#else
static KaiValue *(*kai_user_main_fn)(void) = NULL;
static KaiValue  *kai_user_main_result = NULL;
#endif

/* Under KAI_TRACE_RC, KAI_TRACE_RC_RUNS=k runs main k times in one process,
 * at any thread count, so the RC ledger can tell a leak that grows with the
 * work from state that lives once. Without KAI_TRACE_RC, main runs once. */
static int kai_read_rc_runs(void) {
    if (!getenv("KAI_TRACE_RC")) return 1;
    const char *e = getenv("KAI_TRACE_RC_RUNS");
    long n = e && *e ? strtol(e, NULL, 10) : 1;
    return n < 1 ? 1 : (int) n;
}

static void kai_root_nursery_join(KaiNursery *scope, int mode);
static void kai_root_reap(KaiNursery *root);

/* The scope of the main run in flight: where an actor spawned by a fiber with
 * no explicit scope lives, until main returns. */
static _Atomic(KaiNursery *) kai_root_scope;

/* One run of main inside the implicit root nursery: fibers it spawns outside
 * any scope are joined before the run ends, at every thread count. */
static KaiValue *kai_main_once(KaiValue *(*user_main)(void)) {
    KaiFiber *self = kai_current_fiber();
    KaiNursery root = { .root = 1, .parent = self->nursery_top, .ev_top = self->evidence_top,
                        .reap_at = KAI_ROOT_REAP_MIN };
    self->nursery_top = &root;
    atomic_store(&kai_root_scope, &root);
    KaiValue *r = user_main();
    self->nursery_top = root.parent;
    kai_root_nursery_join(&root, KAI_JOIN_PLAIN);
    atomic_store(&kai_root_scope, NULL);
    return r;
}

/* main, as many times as the ledger asks, on the fiber that is current;
 * what that fiber kept for its constants goes with the last run. */
static KaiValue *kai_main_runs(KaiValue *(*user_main)(void)) {
    KaiFiber *self = kai_current_fiber();
    for (int run = kai_read_rc_runs(); run > 1; run--) {
        kai_decref(kai_main_once(user_main));
        atomic_store(&self->cancel_requested, 0);
        self->cancel_delivered = 0;
    }
    KaiValue *r = kai_main_once(user_main);
    kai_fiber_consts_drop(kai_current_fiber());
    return r;
}

#if defined(__APPLE__) || defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#if KAI_HOST_FIBERS
/* Root-fiber trampoline for the user's main under M:N. Runs kai_main on
 * a spawned fiber's stack (so main can park/steal like any actor),
 * publishes the result, flips the shutdown flag, and returns to thread 0's
 * scheduler loop. */
static void kai_bootstrap_trampoline(void) {
    KaiWorker *w = kai_worker_here();
    KaiFiber *self = w->active;
    kai_drain_pending_free(w);
    self->state = KAI_FIBER_RUNNING;
    kai_user_main_result = kai_main_runs(kai_user_main_fn);
    self->state = KAI_FIBER_DONE;
    /* Program is over: flip the shutdown flag. kai_sched_bootstrap wakes
     * the parked workers and the reactor once thread 0's loop sees it. */
    kai_sched_shutting_down = 1;
    /* Hand back to the worker's scheduler loop, which sees the shutdown
     * flag and returns to kai_sched_bootstrap. */
    w = self->worker;
    w->active = &w->main_fiber;
    KAI_TSAN_SWITCH_TO_ROOT(w);
    setcontext(&w->main_fiber.ctx);
}
#endif

#if defined(KAI_MAIN_STACK_SIZE)
static void kai_sized_main_trampoline(void) {
    kai_user_main_result = kai_main_runs(kai_user_main_fn);
}

/* One thread, main on a stack of KAI_MAIN_STACK_SIZE instead of the OS
 * one, so the budget does not depend on the thread count. Same OS thread:
 * main stays observably main-thread. Recorded as the root fiber's stack
 * so an overflow there gets the guard-page diagnostic. */
static KaiValue *kai_run_main_on_sized_stack(KaiValue *(*user_main)(void)) {
    size_t size = kai_main_fiber_stack_size();
    size_t ps = kai_page_size();
    if (size % ps != 0) size = ((size / ps) + 1) * ps;
    kai_install_fiber_sigsegv_handler();
    KaiFiber *root = &kai_worker_here()->main_fiber;
    kai_user_main_fn = user_main;
    ucontext_t host, ctx;
    void *region = kai_stack_map(size);
    root->stack_base = region;
    root->stack_size = size;
    if (getcontext(&ctx) != 0) {
        kai_eprintf("kai: getcontext failed for main\n");
        kai_exit(1);
    }
    kai_ctx_set_stack(&ctx, (char *) region + ps, size);
    ctx.uc_link = &host;
    makecontext(&ctx, kai_sized_main_trampoline, 0);
    swapcontext(&host, &ctx);
    root->stack_base = NULL;
    root->stack_size = 0;
    kai_stack_unmap(region, size);
    return kai_user_main_result;
}
#endif
#if defined(__APPLE__) || defined(__clang__)
#  pragma clang diagnostic pop
#endif

#if defined(KAI_SEPARATE_COMPILATION)
extern pthread_t kai_worker_threads[KAI_MAX_THREADS];
#  if defined(KAI_RUNTIME_OWNER)
pthread_t kai_worker_threads[KAI_MAX_THREADS];
#  endif
#else
static pthread_t kai_worker_threads[KAI_MAX_THREADS];
#endif

#if KAI_HOST_FIBERS
/* Worker thread entry: adopt an id, anchor this thread's root fiber,
 * mark the slot live, and run the scheduler loop until shutdown. */
static void *kai_worker_thread_main(void *arg) {
    KaiWorker *w = kai_worker_here();
    w->id = (int) (intptr_t) arg;
    w->sched = 1;
    /* Per-thread: without it a fiber overflowing here dies undiagnosed. */
    kai_install_thread_sigaltstack(w);
    w->main_fiber.home_thread = w->id;
    kai_sched_slots[w->id].live = 1;
    kai_rc_ledger_register();
    kai_worker_loop(w);
    kai_seg_pool_drain();
    kai_rc_ledger_fold();
    return NULL;
}

/* F2 — the dedicated reactor thread. Owns the poll() loop and every shared
 * reactor structure (under kai_reactor_mu). It never dispatches fibers; it
 * only drains ready ones and hands them back to their home scheduler thread
 * via remote_unpark. `kai_reactor_wait` blocks in poll() on the self-pipes
 * even with no timers armed, so this loop does not spin; a commit or the
 * shutdown poke breaks the wait. Its thread id is kai_nthreads — a valid,
 * never-live slot index it never dispatches from. */
static void *kai_reactor_thread_main(void *arg) {
    (void) arg;
    KaiWorker *w = kai_worker_here();
    w->id = kai_nthreads;
    /* Never dispatches fibers, but keeps the scheduler threads uniform: a
     * fault here reaches the handler instead of compounding on this stack. */
    kai_install_thread_sigaltstack(w);
    kai_rc_ledger_register();
    for (;;) {
        if (kai_sched_shutting_down) break;
        kai_reactor_wait();
    }
    kai_rc_ledger_fold();
    return NULL;
}
#endif

/* Threads start on first demand, so a program that never spawns and never
 * waits on the reactor runs on thread 0 alone. */
#if defined(KAI_SEPARATE_COMPILATION)
extern pthread_mutex_t kai_pool_mu;
extern _Atomic int     kai_workers_up;
extern _Atomic int     kai_reactor_up;
#  if defined(KAI_RUNTIME_OWNER)
pthread_mutex_t kai_pool_mu = PTHREAD_MUTEX_INITIALIZER;
_Atomic int     kai_workers_up = 0;
_Atomic int     kai_reactor_up = 0;
#  endif
#else
static pthread_mutex_t kai_pool_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int     kai_workers_up = 0;
static _Atomic int     kai_reactor_up = 0;
#endif

/* Workers 1..N-1 are counted idle until they exist, so the all-idle tests
 * keep comparing against kai_nthreads. */
static void kai_workers_ensure(void) {
#if KAI_HOST_FIBERS && !defined(__wasm__)
    if (kai_nthreads <= 1 || atomic_load_explicit(&kai_workers_up, memory_order_acquire)) return;
    pthread_mutex_lock(&kai_pool_mu);
    if (!atomic_load_explicit(&kai_workers_up, memory_order_relaxed)) {
        atomic_fetch_sub(&kai_sched_idle_count, kai_nthreads - 1);
        atomic_fetch_add(&kai_sched_idle_epoch, 1);
        for (int i = 1; i < kai_nthreads; i++) {
            kai_rc_go_mt();
            if (pthread_create(&kai_worker_threads[i], NULL,
                               kai_worker_thread_main, (void *) (intptr_t) i) != 0) {
                kai_eprintf("kai: scheduler pthread_create failed: %s\n", strerror(errno));
                kai_exit(1);
            }
        }
        atomic_store_explicit(&kai_workers_up, 1, memory_order_release);
    }
    pthread_mutex_unlock(&kai_pool_mu);
#endif
}

static void kai_reactor_thread_ensure(void) {
#if KAI_HOST_FIBERS && !defined(__wasm__)
    if (kai_nthreads <= 1 || atomic_load_explicit(&kai_reactor_up, memory_order_acquire)) return;
    pthread_mutex_lock(&kai_pool_mu);
    if (!atomic_load_explicit(&kai_reactor_up, memory_order_relaxed)) {
        atomic_store(&kai_reactor_idle, 0);
        kai_rc_go_mt();
        if (pthread_create(&kai_reactor_thread, NULL, kai_reactor_thread_main, NULL) != 0) {
            kai_eprintf("kai: reactor pthread_create failed: %s\n", strerror(errno));
            kai_exit(1);
        }
        atomic_store_explicit(&kai_reactor_up, 1, memory_order_release);
    }
    pthread_mutex_unlock(&kai_pool_mu);
#endif
}

/* CPUs this process may actually run on. sysconf reports the whole
 * machine, which over-subscribes a process pinned by an affinity mask
 * (containers, CI runners), so the mask wins where it exists. */
static int kai_host_ncpu(void) {
#if defined(__linux__)
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        int n = CPU_COUNT(&set);
        if (n > 0) return n;
    }
#endif
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int) n : 1;
}

/* Ceiling on the *implicit* default only. Past this the per-process cost
 * of spawning workers outgrows the scaling they buy, and a short-lived
 * program on a many-core host pays it for nothing. An explicit
 * KAI_THREADS is honoured up to KAI_MAX_THREADS-1. */
#define KAI_DEFAULT_MAX_THREADS 32

/* Parse KAI_THREADS. Unset → the host CPU count (capped); an explicit
 * value wins, with "0"/"1"/garbage → 1, the single-thread escape hatch.
 * Any count is clamped to [1, KAI_MAX_THREADS-1] so the reactor thread's
 * id (kai_nthreads) stays a valid slot index. */
static int kai_read_nthreads(void) {
#if defined(__wasm__)
    return 1;
#elif KAI_HOST_FIBERS
    const char *e = getenv("KAI_THREADS");
    if (!e || !*e) {
        int n = kai_host_ncpu();
        if (n > KAI_DEFAULT_MAX_THREADS) n = KAI_DEFAULT_MAX_THREADS;
        if (n > KAI_MAX_THREADS - 1) n = KAI_MAX_THREADS - 1;
        return n < 1 ? 1 : n;
    }
    long n = strtol(e, NULL, 10);
    if (n <= 1) return 1;
    if (n > KAI_MAX_THREADS - 1) n = KAI_MAX_THREADS - 1;
    return (int) n;
#else
    return 1;
#endif
}


/* Run the program. At N=1 this is exactly `kai_main()` — no threads, no
 * fibers spawned for main, byte-identical. At N>1 it starts the worker
 * pool, runs kai_main as a fiber on thread 0, drives thread 0's
 * scheduler loop until the program quiesces, then joins the workers. */
KAI_SCHED_FN KaiValue *kai_sched_bootstrap(KaiValue *(*user_main)(void))
#if KAI_SCHED_DECL_ONLY
;
#else
{
    kai_nthreads = kai_read_nthreads();
    kai_worker_here()->sched = 1;
    if (kai_nthreads <= 1) {
        kai_nthreads = 1;
        KAI_TSAN_BIND_ROOT(kai_worker_here());
#if defined(KAI_MAIN_STACK_SIZE)
        return kai_run_main_on_sized_stack(user_main);
#endif
        return kai_main_runs(user_main);
    }

#if KAI_HOST_FIBERS && !defined(__wasm__)
    kai_user_main_fn = user_main;
    /* The Char cache is classified immortal — shared by every thread, never
     * mutated after init. Lazy warming breaks the second half of that: the
     * first `kai_char(c)` on a worker writes 128 cells while other workers
     * are already reading them. Warm it here instead, before any worker
     * exists, so pthread_create publishes it and it really is read-only for
     * the rest of the process. At N=1 the lazy path still applies. */
    kai_char_cache_warm();
    for (int i = 0; i < kai_nthreads; i++) {
        pthread_mutex_init(&kai_sched_slots[i].mu, NULL);
        kai_sched_slots[i].steal_head = NULL;
        kai_sched_slots[i].steal_tail = NULL;
        kai_sched_slots[i].live = 0;
        pthread_mutex_init(&kai_sched_slots[i].park_mu, NULL);
        pthread_cond_init(&kai_sched_slots[i].park_cv, NULL);
        kai_sched_slots[i].parked = 0;
        kai_sched_slots[i].permit = 0;
    }
    /* F2 — the reactor mutex guards the wheel + waiter lists + parked_count,
     * and the reactor thread must have its self-pipes open before it polls
     * (kai_reactor_init is otherwise lazy, triggered by the first park). */
    pthread_mutex_init(&kai_reactor_mu, NULL);
    kai_reactor_init();
    KaiWorker *w = kai_worker_here();
    w->id = 0;
    w->main_fiber.home_thread = 0;
    kai_sched_slots[0].live = 1;

    /* Spawn kai_main as a fiber on thread 0's deque. It gets a main-thread
     * stack, not a fiber one: raising the thread count must not shrink the
     * stack budget a working program already had. */
    KaiFiber *root = (KaiFiber *) calloc(1, sizeof(KaiFiber));
    if (!root) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
    root->home_thread = 0;
    /* Thread 0 IS the OS thread the process entered `main` on, so pinning
     * the entry fiber here is what makes `main` observably main-thread —
     * the guarantee a thread-affine C library (AppKit/GLFW/SDL/GTK) needs
     * from its caller. Every fiber `main` spawns stays freely stealable. */
    root->pinned_main = 1;
    root->evidence_top = NULL;
    kai_fiber_init_ctx_sized(root, kai_main_fiber_stack_size());
    /* Re-point its makecontext entry from the generic trampoline to the
     * bootstrap trampoline (kai_fiber_init_ctx installed the former). */
#if defined(__APPLE__) || defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    makecontext(&root->ctx, kai_bootstrap_trampoline, 0);
#if defined(__APPLE__) || defined(__clang__)
#  pragma clang diagnostic pop
#endif
    root->state = KAI_FIBER_READY;
    kai_sched_enqueue(root);

    atomic_store(&kai_sched_idle_count, kai_nthreads - 1);
    atomic_store(&kai_reactor_idle, 1);

    /* Thread 0 runs the scheduler loop (its root fiber is the loop
     * context) until the bootstrap trampoline flips the shutdown flag. */
    kai_worker_loop(w);

    /* Shutdown: the bootstrap trampoline set kai_sched_shutting_down; wake
     * the parked workers and poke the reactor out of poll() so every thread
     * observes the flag and exits. */
    kai_sched_wake_all();
    kai_reactor_wake();
    if (atomic_load(&kai_workers_up))
        for (int i = 1; i < kai_nthreads; i++) pthread_join(kai_worker_threads[i], NULL);
    if (atomic_load(&kai_reactor_up)) pthread_join(kai_reactor_thread, NULL);
    free(root);
    return kai_user_main_result;
#else
    kai_host_absent("fibers");
#endif
}
#endif

/* m8.x: default Spawn handlers backed by the cooperative scheduler.
 * spawn(thunk)        — alloc fiber + enqueue; thunk runs later.
 * await(fiber)        — park on the fiber's awaiter chain until DONE.
 * yield()             — rotate run queue (no-op if alone).
 * select([fiber])     — race: park on every candidate, return the first
 *                       to terminate, request cancel on the losers.
 * cancel(fiber)       — set cancel_requested; delivery is Phase 3.
 *
 * Op-argument ABI (all effects, not just Spawn): a call site hands each
 * op argument over as an OWNED reference, and the handler entry point
 * consumes it. For a compiler-emitted user clause that is the ordinary
 * function-parameter discipline; for a `$extern_handler` bridge the
 * release lives in the generated shim (`_kai_default_<eff>_<op>_shim`
 * in the C backend, `kaix_default_<eff>_<op>` in the native one). The
 * bodies below therefore BORROW their args and `kai_incref` whatever
 * they retain — same pattern `kai_core_map` documents. A body that
 * forwards to a consuming `kai_core_*` helper increfs into that call,
 * since the shim still owes one release.
 *
 * Spec: docs/fibers-impl.md §*Yield-point list* and §*Trampoline*. */
KAI_SCHED_FN KaiValue *kai_default_spawn_yield(void *self, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
    kai_sched_yield();
    return kai_cont_resume(k, kai_unit());
}
#endif

/* Shared spawn body for Spawn.spawn and spawn_actor. Builds the fiber,
 * optionally stamps `stamp_mb` as its mailbox BEFORE enqueue (so the
 * owner is wired before the child can be stolen and before this returns),
 * and returns the RC=2 Fiber[T] wrapper. */
static KAI_RC_NOINLINE KaiValue *kai_spawn_fiber_stamped(KaiValue *thunk, KaiValue *stamp_pid) {
    KaiMailbox *stamp_mb = stamp_pid ? stamp_pid->as.mb : NULL;
    kai_workers_ensure();
    if (!thunk || thunk->tag != KAI_CLOSURE) {
        kai_eprintf("kai: Spawn.spawn called with non-closure value\n");
        kai_exit(1);
    }
    KaiFiber *f = (KaiFiber *) calloc(1, sizeof(KaiFiber));
    if (!f) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
    f->parent       = kai_current_fiber();
    /* The fiber owns its thunk for its whole lifetime. At N=1 an incref
     * suffices (one thread). At N>1 the fiber may run on another
     * scheduler thread, so sharing the closure would race its non-atomic
     * rc (and its captures') between the spawner and the fiber body —
     * exactly the cross-thread-share the message copy path forbids. Copy
     * the thunk into a single-owner tree that migrates with the fiber. */
    if (kai_nthreads > 1) {
        f->thunk = kai_deep_copy_cross(thunk);
    } else {
        /* incref to retain a ref the fiber owns; the caller-side ref is
         * still the caller's to manage (Perceus decrefs at call-site exit). */
        f->thunk = kai_incref(thunk);
    }
    /* Capabilities do not cross a spawn: a value-transportable effect
     * rides the child thunk's own evidence frame, and a fiber-local
     * effect resolves against this fiber's own disposition (cancel pad,
     * link set, mailbox, nursery), never the parent's. The child starts
     * with an empty evidence stack rather than inheriting the parent's —
     * inheriting it would carry a parent handler whose *Ev struct lives
     * on a stack frame the parent overwrites once it returns. */
    f->evidence_top          = NULL;
    memset(f->fl_top, 0, sizeof f->fl_top);
    kai_fiber_init_ctx(f);
    /* Stamp the actor mailbox onto the fiber before it becomes runnable:
     * owner_fiber must be set here, not after enqueue, or a Monitor/Link
     * in the spawner would resolve a NULL owner and a stolen child could
     * run (and free the mailbox) before the write landed. */
    if (stamp_mb) {
        stamp_mb->owner_fiber = f;
        f->mailbox            = stamp_mb;
        if (!stamp_pid->as.pid.owner) stamp_pid->as.pid.owner = kai_fiber_exit_ref(f);
    }
    /* R4 fix — allocate the wrapper before enqueue so the scheduler
     * can hold its own incref on the value. Without this second ref a
     * `let _ = fiber_spawn(…)` discard would drop the wrapper to
     * RC=0 while the struct is still in the ready queue, and the
     * trampoline would later run on freed memory. The wrapper RC
     * therefore starts at 2: one for the caller (the user-visible
     * Fiber[T] handle) and one for the scheduler (released in the
     * trampoline's DONE/CANCELLED tail via `kai_decref(self->value)`). */
    KaiValue *v = kai_fiber_value(f);  /* RC=1, sets f->value */
    kai_incref(v);                     /* RC=2, scheduler's own ref */
    /* Issue #959 — register on the spawning fiber's innermost open
     * nursery. The scope takes its own ref (RC=3 here) so a discarded
     * `Fiber[T]` handle cannot free the wrapper before `nursery_exit`
     * joins this child; the scope releases it during the join walk. */
    KaiNursery *scope = kai_current_fiber()->nursery_top;
    if (stamp_mb && (!scope || scope->body)) scope = atomic_load(&kai_root_scope);
    if (scope) {
        f->root_actor    = scope->root && stamp_mb;
        kai_incref(v);
        f->scope_nursery = scope;
        KaiFiber *head   = atomic_load(&scope->children_head);
        do {
            f->scope_sibling_next = head;
        } while (!atomic_compare_exchange_weak(&scope->children_head, &head, f));
        if (scope->root && atomic_fetch_add(&scope->registered, 1) + 1 >= atomic_load(&scope->reap_at)) {
            kai_root_reap(scope);
        }
    }
    f->state = KAI_FIBER_READY;
    kai_sched_enqueue(f);
    return v;
}

KAI_SCHED_FN KaiValue *kai_default_spawn_spawn(void *self, KaiValue *thunk, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
    KaiValue *v = kai_spawn_fiber_stamped(thunk, NULL);
    return kai_cont_resume(k, v);
}
#endif

/* Hand a terminated fiber's result to an awaiter.
 *
 * `f->result` is produced on `f`'s own thread and the Fiber[T] wrapper goes
 * on owning it, so an awaiter that just increfs puts one non-atomic rc under
 * two threads — and two awaiters woken on different threads race each other
 * outright, since the terminate walk unparks the whole chain. The result is a
 * value crossing a thread boundary exactly like a message, so it gets the
 * same treatment: at N>1 the awaiter takes a private copy and the producer's
 * reference is left alone. The wrapper is kept alive by the awaiter's own
 * Fiber[T] handle, so reading it here is safe.
 *
 * A CANCELLED target may have unwound before assigning, leaving `result`
 * NULL; the caller's continuation still needs a well-typed value, so that
 * yields unit. At N=1 the incref is unchanged. */
static KaiValue *kai_fiber_result_for_awaiter(KaiFiber *f) {
    if (!f->result) return kai_unit();
    if (kai_nthreads > 1) return kai_deep_copy_cross(f->result);
    return kai_incref(f->result);
}

KAI_SCHED_FN KaiValue *kai_default_spawn_await(void *self, KaiValue *fib_v, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
    if (!fib_v || fib_v->tag != KAI_FIBER || !fib_v->as.fib) {
        kai_eprintf("kai: Spawn.await called on non-fiber value\n");
        kai_exit(1);
    }
    KaiFiber *f = fib_v->as.fib;
    /* Issue #679: a fiber cancelled mid-flight (e.g. cancelled
     * while parked in NetTcp.accept) terminates in state
     * CANCELLED, not DONE. Treat both as "terminated" for the
     * purposes of awaiting. The fiber's `result` is set to
     * kai_unit() on cancel-driven unwind (see the cancel pad
     * setup). */
    /* Check-and-link under f's slot lock so the terminate walk cannot
     * snapshot the chain between our state read and our link (the lost
     * wakeup). Loop: a resume is a hint, not a proof — re-check the
     * terminal state and re-park on a spurious wake. */
    for (;;) {
        int f_hl = 0;
        if (kai_nthreads > 1) f_hl = kai_fiber_slot_lock(f);
        int terminal = (f->state == KAI_FIBER_DONE || f->state == KAI_FIBER_CANCELLED);
        KaiFiber *me = kai_current_fiber();
        if (terminal) kai_awaiter_unlink(f, me);
        else          kai_awaiter_link_if_absent(f, me);
        if (kai_nthreads > 1) kai_fiber_slot_unlock_at(f_hl);
        if (terminal) break;
        atomic_store(&me->cancel_wake, 1);
        if (!kai_cancel_due(me)) kai_sched_park_linked();
        atomic_store(&me->cancel_wake, 0);
        if (kai_cancel_due(me)) {
            /* Off the chain before the unwind: the terminate walk must not
             * wake a fiber that has left. */
            if (kai_nthreads > 1) f_hl = kai_fiber_slot_lock(f);
            kai_awaiter_unlink(f, me);
            if (kai_nthreads > 1) kai_fiber_slot_unlock_at(f_hl);
            kai_decref(fib_v);
            kai_check_cancel_yield_point();
        }
    }
    return kai_cont_resume(k, kai_fiber_result_for_awaiter(f));
}
#endif

KAI_SCHED_FN KaiValue *kai_default_spawn_select(void *self, KaiValue *fibs_v, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
    if (!fibs_v || (fibs_v->tag != KAI_CONS && fibs_v->tag != KAI_NIL)) {
        kai_eprintf("kai: Spawn.select called on non-list value\n");
        kai_exit(1);
    }
    if (fibs_v->tag == KAI_NIL) {
        kai_eprintf("kai: Spawn.select called on empty list\n");
        kai_exit(1);
    }
    /* Collect the candidates once: the list is walked repeatedly below and
     * a cons cell read is not cheaper than an array index. */
    int n = 0;
    for (KaiValue *c = fibs_v; kai_is_ptr(c) && c->tag == KAI_CONS; c = c->as.cons.tail) n++;
    KaiFiber  *fibs_stack[8];
    KaiFiber **fibs = fibs_stack;
    if (n > 8) {
        fibs = (KaiFiber **) malloc((size_t) n * sizeof(KaiFiber *));
        if (!fibs) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
    }
    {
        int i = 0;
        for (KaiValue *c = fibs_v; kai_is_ptr(c) && c->tag == KAI_CONS; c = c->as.cons.tail) {
            KaiValue *elem = c->as.cons.head;
            if (!elem || elem->tag != KAI_FIBER || !elem->as.fib) {
                kai_eprintf("kai: Spawn.select: list element is not a fiber\n");
                kai_exit(1);
            }
            fibs[i++] = elem->as.fib;
        }
    }

    /* Race. The selector links a node into EVERY candidate's select chain,
     * so whichever terminates first wakes it; on resume it re-scans and
     * takes the first candidate found terminated. Order within one wake is
     * list order, which only decides ties — two fibers that finished before
     * the selector ran are both winners by any honest reading.
     *
     * Link-then-recheck is the same lost-wakeup discipline await uses, one
     * candidate at a time: a fiber that terminates while we are still
     * linking later candidates has already put us on its chain (so the
     * unpark is recorded, not lost) or set its terminal state before we
     * read it (so the rescan sees it and we never park). */
    KaiSelectWaiter  nodes_stack[8];
    KaiSelectWaiter *nodes = nodes_stack;
    if (n > 8) {
        nodes = (KaiSelectWaiter *) malloc((size_t) n * sizeof(KaiSelectWaiter));
        if (!nodes) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
    }
    KaiFiber *me     = kai_current_fiber();
    KaiFiber *winner = NULL;
    for (int i = 0; i < n; i++) { nodes[i].waiter = me; nodes[i].next = NULL; }

    for (;;) {
        int linked_upto = 0;
        for (int i = 0; i < n && !winner; i++) {
            KaiFiber *f = fibs[i];
            int hl = 0;
            if (kai_nthreads > 1) hl = kai_fiber_slot_lock(f);
            if (f->state == KAI_FIBER_DONE || f->state == KAI_FIBER_CANCELLED) {
                winner = f;
            } else {
                kai_select_link(f, &nodes[i]);
                linked_upto = i + 1;
            }
            if (kai_nthreads > 1) kai_fiber_slot_unlock_at(hl);
        }
        /* Park only while still linked: a candidate that terminates now
         * finds us on its chain and the unpark is recorded rather than
         * lost. Unlinking before the park would leave nobody able to wake
         * us — the whole race would hang on the first sleeper. */
        if (!winner) {
            atomic_store(&me->cancel_wake, 1);
            if (!kai_cancel_due(me)) kai_sched_park_linked();
            atomic_store(&me->cancel_wake, 0);
        }
        /* Unlink every node we linked this round, whether we won, lost the
         * tie, or woke spuriously: the nodes live in this frame, so no
         * candidate may hold a pointer into it past the return, and the
         * next round must be able to re-link the same node cleanly. */
        for (int i = 0; i < linked_upto; i++) {
            KaiFiber *f = fibs[i];
            int hl = 0;
            if (kai_nthreads > 1) hl = kai_fiber_slot_lock(f);
            kai_select_unlink(f, &nodes[i]);
            if (kai_nthreads > 1) kai_fiber_slot_unlock_at(hl);
        }
        if (winner) break;
        if (kai_cancel_due(me)) {
            if (nodes != nodes_stack) free(nodes);
            if (fibs  != fibs_stack)  free(fibs);
            kai_decref(fibs_v);
            kai_cancel_deliver(me);
        }
    }

    /* Cancel the losers. `select` returns one result and the nursery joins
     * every child at scope exit, so a loser left running would stall the
     * scope on work whose result nobody can observe. Delivery is the
     * ordinary cooperative path: the flag lands and the fiber unwinds at
     * its next yield point; a parked loser is woken to reach that point
     * instead of sleeping on an event it no longer needs. */
    for (int i = 0; i < n; i++) {
        KaiFiber *f = fibs[i];
        if (f == winner) continue;
        if (f->state == KAI_FIBER_DONE || f->state == KAI_FIBER_CANCELLED) continue;
        kai_fiber_request_cancel(f);
    }

    KaiValue *res = kai_fiber_result_for_awaiter(winner);
    if (nodes != nodes_stack) free(nodes);
    if (fibs  != fibs_stack)  free(fibs);
    return kai_cont_resume(k, res);
}
#endif

/* Issue #679: detach `target` from whatever reactor waiter list it
 * sits on (if any). Returns 1 if the fiber was found and removed
 * from some list, 0 if it was already runnable / DONE / not parked
 * on the reactor. The caller is responsible for calling
 * `kai_sched_unpark(target)` afterwards so the scheduler reschedules
 * it; we keep detach and unpark separate so other call sites
 * (timer expiry, socket-ready drain) that already manage their own
 * resume sequencing can reuse the detach without double-unparking.
 *
 * The walk is O(N) per list; the lists are short in practice
 * (per-fiber-arena means typically one waiter per fd). All seven
 * lists are checked because a fiber lives on exactly one — the
 * waiter discipline never enqueues the same fiber twice. */
static int kai_reactor_detach_fiber(KaiFiber *target) {
    if (!target) return 0;
    /* F2: walk + unlink under kai_reactor_mu — the reactor thread and other
     * schedulers touch these same lists. A target that stamped a park but
     * has not been committed yet is not on any list here (returns 0); the
     * commit path re-checks the cancel flag under this same lock and aborts
     * the park, so the flag set before this walk is never slept through.
     * reactor_mu is released before the caller's unpark takes a slot lock,
     * so the two locks are never nested. */
    kai_reactor_lock();
    KaiFiber **heads[] = {
        &kai_reactor_socket_read_waiters,
        &kai_reactor_socket_write_waiters,
        &kai_reactor_pid_waiters,
        &kai_reactor_filepool_waiters,
        &kai_reactor_timer_head,
        &kai_reactor_stdin_waiter,
        &kai_reactor_signal_waiter,
    };
    const int n_heads = (int) (sizeof(heads) / sizeof(heads[0]));
    int found = 0;
    for (int h = 0; h < n_heads && !found; ++h) {
        KaiFiber **link = heads[h];
        while (*link) {
            if (*link == target) {
                *link = target->reactor_next;
                target->reactor_next = NULL;
                target->reactor_wait_pid = 0;
                target->reactor_fired = 1;
                kai_reactor_parked_count--;
                found = 1;
                break;
            }
            link = &(*link)->reactor_next;
        }
    }
    kai_reactor_unlock();
    return found;
}

/* Request `f`'s cancellation and get it to a yield point. The flag alone
 * reaches a runnable fiber at its next op. A parked one never reaches an
 * op: on a reactor waiter list no event may ever come, and on a mailbox
 * chain no message may. Splice it off the reactor, or let its mailbox park
 * unlink itself, and unpark it; the resume observes the flag and unwinds.
 * A fiber parked on an await or a nursery join keeps waiting: what it
 * waits for must end before it unwinds. */
static void kai_fiber_request_cancel(KaiFiber *f) {
    atomic_store(&f->cancel_requested, 1);
    if (kai_reactor_detach_fiber(f) || atomic_load(&f->cancel_wake)) {
        kai_sched_unpark(f);
    }
}

KAI_SCHED_FN KaiValue *kai_default_spawn_cancel(void *self, KaiValue *fib_v, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
    if (fib_v && fib_v->tag == KAI_FIBER && fib_v->as.fib) {
        kai_fiber_request_cancel(fib_v->as.fib);
    }
    return kai_cont_resume(k, kai_unit());
}
#endif

/* Tier 2 trap-exit: set the current fiber's trap_exit flag from a
 * Bool argument. With trap_exit=1, a linked peer's termination
 * delivers a "Normal"/"Crashed" string to the fiber's mailbox
 * instead of setting cancel_requested. The fiber must already
 * have a mailbox (typically via with_mailbox) for the delivery to
 * land; without one, the propagation falls back to cancel_requested.
 * Spec: docs/actors.md §*Trap-exit semantics*. */
KAI_SCHED_FN KaiValue *kai_default_spawn_set_trap_exit(void *self, KaiValue *on, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
    KaiFiber *f = kai_current_fiber();
    int v = (on && on->tag == KAI_BOOL && on->as.b) ? 1 : 0;
    f->trap_exit = v;
    return kai_cont_resume(k, kai_unit());
}
#endif

/* Issue #959 — open a structured-concurrency scope on the current
 * fiber. Subsequent `Spawn.spawn` calls register their child on this
 * scope's children list; `nursery_exit` joins them all. Scopes nest:
 * the new scope's `parent` is the fiber's previous `nursery_top`. */
KAI_SCHED_FN KaiValue *kai_default_spawn_scope_enter(void *self, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
    KaiFiber *f = kai_current_fiber();
    KaiNursery *n = (KaiNursery *) calloc(1, sizeof(KaiNursery));
    if (!n) { kai_eprintf("kai: out of memory (nursery scope)\n"); kai_exit(1); }
    n->children_head = NULL;
    n->parent        = f->nursery_top;
    n->ev_top        = f->evidence_top;
    f->nursery_top   = n;
    return kai_cont_resume(k, kai_unit());
}
#endif

static void kai_nursery_cancel_siblings(KaiFiber *head, KaiFiber *skip);

/* Park the current fiber on `child`'s awaiter chain until it reaches
 * DONE or CANCELLED. Mirrors the await park; no result is read — the
 * scope joins for completion, not for the value. An `interruptible` join
 * woken by a cancel aimed at the joiner cancels the scope's children
 * (`head`) and keeps waiting: they must end before the scope exits. Returns
 * 1 when that happened, for the caller to re-raise once the drain is done. */
static int kai_nursery_join_child(KaiFiber *child, KaiFiber *head, int interruptible) {
    /* Check-and-link under child's slot lock so its terminate walk cannot
     * snapshot the chain between our state read and our link (the lost
     * wakeup that stranded a nursery join at N>1). Loop: only the child's
     * terminal state ends the join; a spurious resume re-parks. */
    int cancelled = 0;
    for (;;) {
        int child_hl = 0;
        if (kai_nthreads > 1) child_hl = kai_fiber_slot_lock(child);
        int terminal = (child->state == KAI_FIBER_DONE || child->state == KAI_FIBER_CANCELLED);
        KaiFiber *me = kai_current_fiber();
        if (terminal) kai_awaiter_unlink(child, me);
        else          kai_awaiter_link_if_absent(child, me);
        if (kai_nthreads > 1) kai_fiber_slot_unlock_at(child_hl);
        if (terminal) return cancelled;
        if (!interruptible) { kai_sched_park_linked(); continue; }
        atomic_store(&me->cancel_wake, 1);
        if (!kai_cancel_due(me)) kai_sched_park_linked();
        atomic_store(&me->cancel_wake, 0);
        if (kai_cancel_due(me)) {
            kai_nursery_cancel_siblings(head, NULL);
            interruptible = 0;
            cancelled = 1;
        }
    }
}

/* Cancel every not-yet-terminated child still on the scope list: set
 * the flag and wake a parked child, so the cancel is delivered at its
 * next op boundary. Idempotent, so
 * the eager walk and the scope_exit walk may both visit a sibling.
 * `skip` excludes the failing child itself, which is mid-trampoline
 * and must not be flagged as a requested cancellation. */
static void kai_nursery_cancel_siblings(KaiFiber *head, KaiFiber *skip) {
    for (KaiFiber *c = head; c; c = c->scope_sibling_next) {
        if (c == skip
            || c->state == KAI_FIBER_DONE || c->state == KAI_FIBER_CANCELLED) {
            continue;
        }
        kai_fiber_request_cancel(c);
    }
}

/* Eager cancel-on-fail: a child terminating CANCELLED without a
 * requested cancellation cancels its siblings here, at its own
 * termination, instead of when the owner's join loop reaches it.
 * Runs before the terminal state store — see `scope_nursery` for why
 * that keeps the scope pointer alive across threads. The latch makes
 * one failing child the sole walker; a loser's failure is already
 * recorded, so its skipped walk changes nothing. */
static void kai_nursery_propagate_failure(KaiFiber *self) {
    KaiNursery *scope = self->scope_nursery;
    if (!scope || scope->root || self->cancel_requested) return;
    KaiFiber *expected = NULL;
    if (!atomic_compare_exchange_strong(&scope->failed_child, &expected, self)) {
        return;
    }
    kai_nursery_cancel_siblings(scope->children_head, self);
}

/* Drop the scope's refs on its terminated children and free it. The
 * children's wrappers may still be held by the user's `Fiber[T]` handles,
 * so decref (not free). */
static void kai_nursery_release(KaiFiber *head, KaiNursery *scope) {
    KaiFiber *c = head;
    while (c) {
        KaiFiber *nx = c->scope_sibling_next;
        c->scope_sibling_next = NULL;
        c->scope_nursery      = NULL;
        if (c->value) kai_decref(c->value);
        c = nx;
    }
    if (!scope->root) free(scope);
}

/* Join every child of `scope`, cancelling the rest on the first failure
 * (a child cancelled without a request: it raised Cancel or trapped), then
 * release the scope's refs. Returns 1 if a child failed. */
static int kai_nursery_join_all(KaiFiber *head, KaiNursery *scope) {
    int failed = 0;
    for (KaiFiber *c = head; c; c = c->scope_sibling_next) {
        if (kai_nursery_join_child(c, head, !failed)) failed = 1;
        if (!failed && c->state == KAI_FIBER_CANCELLED && !c->cancel_requested) {
            failed = 1;
            kai_nursery_cancel_siblings(head, NULL);
        }
    }
    /* A child that failed concurrently with a peer's cancel walk terminates
     * flagged; only the latch records it. */
    if (!failed && scope->failed_child != NULL) failed = 1;
    kai_nursery_release(head, scope);
    return failed;
}

/* Drop the terminated children behind the list head and release the scope's
 * own ref on each; a handle the program keeps holds its own. Pushers only CAS
 * `children_head`, so unlinking strictly after the node read as head races
 * with no one; the head itself stays. Skipped when the final join holds the
 * lock. The next pass waits until the list has doubled. */
static void kai_root_reap(KaiNursery *root) {
    if (atomic_exchange(&root->reap_lock, 1)) return;
    long live = 0;
    KaiFiber *head = atomic_load(&root->children_head);
    if (head) {
        live = 1;
        KaiFiber *prev = head;
        for (KaiFiber *c = head->scope_sibling_next, *next; c; c = next) {
            next = c->scope_sibling_next;
            int hl = 0, terminal;
            if (kai_nthreads > 1) hl = kai_fiber_slot_lock(c);
            terminal = c->state == KAI_FIBER_DONE || c->state == KAI_FIBER_CANCELLED;
            if (kai_nthreads > 1) kai_fiber_slot_unlock_at(hl);
            if (!terminal) { prev = c; live++; continue; }
            prev->scope_sibling_next = next;
            c->scope_sibling_next    = NULL;
            c->scope_nursery         = NULL;
            if (c->value) kai_decref(c->value);
        }
    }
    atomic_store(&root->reap_at, live * 2 > KAI_ROOT_REAP_MIN ? live * 2 : KAI_ROOT_REAP_MIN);
    atomic_store(&root->registered, live);
    atomic_store(&root->reap_lock, 0);
}

/* A fiber's trap or unrequested cancel is contained at an implicit scope: the
 * join waits for every child but neither cancels its siblings nor fails the
 * owner. Plain children are joined first; the actors still alive then get a
 * cancel (a server parked in `receive` never ends on its own) and are joined
 * too. Children registered while a batch is joined (an actor spawning from
 * its cleanup, from any thread) form the next batch. An interruptible join
 * (a fiber body) turns a cancel aimed at the owner into a cancel of every
 * child, still waiting for them to end; a cancelling join starts from there. */
static void kai_root_nursery_join(KaiNursery *scope, int mode) {
    int cancelled = mode == KAI_JOIN_CANCEL;
    for (;;) {
        while (atomic_exchange(&scope->reap_lock, 1)) { }
        KaiFiber *head = atomic_exchange(&scope->children_head, NULL);
        atomic_store(&scope->reap_lock, 0);
        if (!head) break;
        if (cancelled) kai_nursery_cancel_siblings(head, NULL);
        for (KaiFiber *c = head; c; c = c->scope_sibling_next) {
            if (c->root_actor) continue;
            if (kai_nursery_join_child(c, head, mode == KAI_JOIN_INTERRUPTIBLE && !cancelled)) cancelled = 1;
        }
        for (KaiFiber *c = head; c; c = c->scope_sibling_next) {
            if (!c->root_actor) continue;
            kai_fiber_request_cancel(c);
            kai_nursery_join_child(c, head, 0);
        }
        kai_nursery_release(head, scope);
    }
}

/* A fiber unwinding by cancel never reaches its `scope_exit`: cancel and
 * join the children of every scope it left open, so none outlives it. */
static void kai_nursery_abandon_top(KaiFiber *f) {
    KaiNursery *scope = f->nursery_top;
    f->nursery_top = scope->parent;
    KaiFiber *head = scope->children_head;
    kai_nursery_cancel_siblings(head, NULL);
    for (KaiFiber *c = head; c; c = c->scope_sibling_next) {
        kai_nursery_join_child(c, head, 0);
    }
    kai_nursery_release(head, scope);
}

static void kai_nursery_abandon(KaiFiber *f) {
    while (f->nursery_top) kai_nursery_abandon_top(f);
}

/* A fiber about to unwind to its pad ends its children first, so they are
 * gone before any handler's cleanup runs. The body scope stays open: cleanup
 * may still spawn into it. */
static void kai_nursery_end_children(KaiFiber *f) {
    while (f->nursery_top && f->nursery_top->parent) kai_nursery_abandon_top(f);
    if (f->nursery_top) kai_root_nursery_join(f->nursery_top, KAI_JOIN_CANCEL);
}

/* Issue #959 — close the innermost scope: join every child, then on
 * the first child that terminated CANCELLED cancel the remaining
 * siblings, finish the drain (waiting their unwind), release the
 * scope's refs, and re-raise via the current fiber's cancel_pad so
 * the failure propagates out of the nursery body. On a clean drain
 * just release the refs and return unit. */
KAI_SCHED_FN KaiValue *kai_default_spawn_scope_exit(void *self, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
    KaiFiber   *f     = kai_current_fiber();
    KaiNursery *scope = f->nursery_top;
    if (!scope) {
        /* No open scope — `nursery_exit` without a matching enter is a
         * runtime invariant violation, but degrade to a no-op rather
         * than crash. */
        return kai_cont_resume(k, kai_unit());
    }
    /* Pop the scope first so children joined below (which run on the
     * scheduler) see the enclosing scope, not this closing one. */
    f->nursery_top = scope->parent;

    KaiFiber *head   = scope->children_head;
    int       failed = kai_nursery_join_all(head, scope);

    if (failed) {
        /* The re-raise is a `Cancel.raise()` like any other: it walks
         * this fiber's handler stack first, so a `with Cancel`
         * enclosing the nursery runs its clause. Only with no user
         * handler in scope does it take a terminal path — the
         * trampoline's cancel pad, or, at the program root where no
         * pad exists, banner + exit. */
        f->cancel_delivered = 1;
        (void) kai_cancel_dispatch_user_handler();
        if (f->cancel_pad_set) {
            kai_evidence_unwind_all();
            kai_fiber_pad_jump(f, 0);
            /* Unreachable. */
        }
        kai_test_cancel_escape();
        kai_eputs("kai: nursery child cancelled; no survivors\n");
        kai_exit(1);
    }
    return kai_cont_resume(k, kai_unit());
}
#endif

/* m8 #4 + Phase 3: default Cancel.raise handler. Doc B §`Cancel`/
 * Default handler: an unhandled Cancel.raise() unwinds the fiber
 * cleanly (the runtime delivers "no silent survivors"). For a
 * spawned fiber whose trampoline cancel_pad is live, longjmp to it
 * — same path as the yield-point hook, marks state CANCELLED and
 * resumes the scheduler. For main_fiber (no pad — main never runs
 * through the trampoline), fall back to the m8 v1 behaviour: banner
 * + exit(0). Exit code 0 because cancellation at the program root
 * is an expected termination, not a programmer error. */
static KaiValue *kai_default_cancel_raise(void *self, KaiCont *k) {
    (void) self;
    (void) k;
    KaiFiber *f = kai_current_fiber();
    if (f->cancel_pad_set) {
        f->cancel_delivered = 1;
        kai_evidence_unwind_all();
        kai_fiber_pad_jump(f, 0);
        /* Unreachable. */
    }
    kai_test_cancel_escape();
    kai_eputs("kai: Cancel.raise: unhandled (fiber cancelled)\n");
    kai_exit(0);
}

/* Phase 5 — Link runtime registry.
 *
 * `Link.link(peer)` registers a bidirectional link between the
 * current fiber and the fiber that owns `peer`'s mailbox. On
 * either fiber's termination, the trampoline walks the linked
 * chain and sets cancel_requested on each peer (delivered at
 * that peer's next yield-point hook).
 *
 * v1 simplifications:
 *  - `Pid[Nothing]` is the type-erased existential pid the typer
 *    uses for link/monitor ops; we resolve via the pid's exit record.
 *  - Self-links (a == b) are dropped.
 *  - A pid with no owner (mailbox alloc'd outside any fiber context)
 *    links nothing.
 *  - Duplicate links between the same pair are not de-dup'd; the
 *    propagation walk sets cancel_requested idempotently, so the
 *    duplicates are harmless beyond the wasted KaiLinkNode.
 *  - Spec specifies "crash → propagate cancel"; v1 propagates on
 *    both DONE and CANCELLED termination (BEAM-style trap-exit
 *    semantics queued for post-MVP). */
static void kai_link_add_bidirectional(KaiFiber *a, KaiFiber *b) {
    if (!a || !b || a == b) return;
    KaiLinkNode *na = (KaiLinkNode *) calloc(1, sizeof(KaiLinkNode));
    KaiLinkNode *nb = (KaiLinkNode *) calloc(1, sizeof(KaiLinkNode));
    if (!na || !nb) {
        kai_eprintf("kai: out of memory (link)\n");
        kai_exit(1);
    }
    na->peer = b; na->next = a->linked_head; a->linked_head = na;
    nb->peer = a; nb->next = b->linked_head; b->linked_head = nb;
}

/* Walk a fiber's linked chain at termination. For each peer:
 *   - if peer has trap_exit=1 AND a current mailbox, push a
 *     "Normal" or "Crashed" KAI_STR (per `reason`) into the
 *     mailbox. The peer learns of the termination in user code
 *     and is NOT cancelled.
 *   - otherwise, set cancel_requested (the v1 default — delivered
 *     at the peer's next yield-point hook).
 * In either branch, remove our back-link from the peer's chain so
 * the peer's own future termination doesn't re-enter our (now-
 * freed) chain. Each KaiLinkNode is freed as the walk passes it. */
/* Deliver a system message (link trap-exit reason, monitor pid) into a
 * mailbox owned by an arbitrary fiber, consuming `msg`. Same copy rule as
 * kai_core_mailbox_send: at N=1 a plain push (byte-identical), at N>1 an
 * unconditional deep copy, because the peer's `home_thread` is not stable
 * across the send and the peer's heap must stay single-threaded. */
static void kai_deliver_to_mailbox(KaiMailbox *mb, KaiValue *msg) {
    if (kai_nthreads > 1) {
        KaiValue *copy = kai_deep_copy_cross(msg);
        kai_decref(msg);
        kai_mailbox_push_cross_thread(mb, copy);
    } else {
        kai_mailbox_push(mb, msg);
    }
}

