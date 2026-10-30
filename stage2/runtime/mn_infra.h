/* Fragment of runtime.h; include runtime.h, never this file. */
/* ==================================================================
 * M:N work-stealing scheduler — cross-thread infrastructure.
 * docs/mn-scheduler-design.md §2. Off entirely at N=1 (the default):
 * kai_nthreads==1 makes every cross-thread branch below inert, so the
 * single-thread scheduler runs byte-identically to the pre-M:N runtime.
 * ================================================================== */


/* Per-thread scheduler slot: this thread's ready deque, the single
 * source of truth at N>1. The owner pushes/pops both ends and a thief
 * pulls the head, all under `mu` — the mutex is touched only on
 * enqueue/dequeue/steal/remote-unpark, never on the per-op RC path.
 * `live` marks the slot as a steal target once its thread is running; a
 * thief reads it outside `mu` (before deciding to lock), so it is atomic:
 * a worker publishes `live=1` at startup and every thief acquire-loads it. */
typedef struct KaiSchedSlot {
    pthread_mutex_t mu;
    KaiFiber       *steal_head;   /* fibers available to steal (FIFO) */
    KaiFiber       *steal_tail;
    _Atomic int     live;         /* slot participates in stealing */
    /* Idle park: the owner blocks on park_cv until a waker hands it a
     * permit or shutdown begins (kai_worker_park). */
    pthread_mutex_t park_mu;
    pthread_cond_t  park_cv;
    _Atomic int     parked;       /* owner is announced asleep */
    _Atomic int     permit;       /* a wake is pending; consumed by exchange */
} KaiSchedSlot;

#define KAI_MAX_THREADS 256
#if defined(KAI_SEPARATE_COMPILATION)
extern KaiSchedSlot kai_sched_slots[KAI_MAX_THREADS];
#  if defined(KAI_RUNTIME_OWNER)
KaiSchedSlot kai_sched_slots[KAI_MAX_THREADS];
#  endif
#else
static KaiSchedSlot kai_sched_slots[KAI_MAX_THREADS];
#endif

/* `kai_sched_sleepers` counts workers announced asleep in kai_worker_park;
 * a producer that reads 0 after its push skips the wake scan entirely.
 * `kai_sched_spinning` counts workers searching in kai_worker_spin; while
 * one is, a producer leaves its fiber to that search instead of waking a
 * parked worker. `kai_sched_shutting_down` is written once at shutdown and
 * read lock-free by every worker and the reactor. */
#if defined(KAI_SEPARATE_COMPILATION)
extern _Atomic int kai_sched_sleepers;
extern _Atomic int kai_sched_spinning;
extern _Atomic int kai_sched_shutting_down;   /* main returned → workers exit */
#  if defined(KAI_RUNTIME_OWNER)
_Atomic int kai_sched_sleepers = 0;
_Atomic int kai_sched_spinning = 0;
_Atomic int kai_sched_shutting_down = 0;
#  endif
#else
static _Atomic int kai_sched_sleepers = 0;
static _Atomic int kai_sched_spinning = 0;
static _Atomic int kai_sched_shutting_down = 0;
#endif

/* M:N global-quiescence deadlock detection. The single-thread dispatch loop
 * declares deadlock when its run queue drains with fibers still PARKED; no
 * single worker can make that call under M:N (a peer might hold runnable
 * work), so these globals let an idle worker confirm the WHOLE machine is
 * wedged before it does. A deadlock is a stable state, so the check keys on
 * quiescence, never elapsed time — a long-running program is never killed.
 *
 *   kai_sched_idle_count    — workers blocked in the idle park. == kai_nthreads
 *                             means no fiber is RUNNING, so no fiber body can
 *                             produce new runnable work.
 *   kai_blocked_fiber_count — fibers currently in KAI_FIBER_PARKED (mailbox
 *                             recv / await / send-block AND reactor waiters);
 *                             the count the deadlock banner reports.
 *   kai_sched_idle_epoch    — bumped each time a worker leaves the idle pool.
 *                             The check reads its inputs one after another,
 *                             so an unchanged epoch is what makes them one
 *                             snapshot: no worker took a fiber in between.
 *   kai_deadlock_reported   — claimed by CAS so exactly one worker prints the
 *                             banner. Quiescence is observable by every idle
 *                             worker at once, so without it the banner count
 *                             is a scheduling race.
 * The reactor's own idle state is kai_reactor_idle, beside the reactor
 * globals below. Untouched at N=1 (that path keeps the worker's parked_count). */
#if defined(KAI_SEPARATE_COMPILATION)
extern _Atomic int      kai_sched_idle_count;
extern _Atomic unsigned kai_sched_idle_epoch;
extern _Atomic int      kai_blocked_fiber_count;
extern _Atomic int      kai_deadlock_reported;
#  if defined(KAI_RUNTIME_OWNER)
_Atomic int      kai_sched_idle_count = 0;
_Atomic unsigned kai_sched_idle_epoch = 0;
_Atomic int      kai_blocked_fiber_count = 0;
_Atomic int      kai_deadlock_reported = 0;
#  endif
#else
static _Atomic int      kai_sched_idle_count = 0;
static _Atomic unsigned kai_sched_idle_epoch = 0;
static _Atomic int      kai_blocked_fiber_count = 0;
static _Atomic int      kai_deadlock_reported = 0;
#endif

/* Park/wake trace for the deadlock banner. `kai_blocked_fiber_count` has a
 * single decrement site (the PARKED branch of kai_sched_remote_unpark) while
 * kai_reactor_parked_count has eight, so a fiber promoted off a reactor
 * structure but flipped READY before the handback reaches it leaves the pair
 * skewed — parked_count 0 with blocked_fiber_count > 0, which is exactly what
 * the banner reports. The ring records the transitions the banner cannot
 * reconstruct: enabled only when KAI_TRACE_PARK is set, so a normal run pays
 * one predictable load per event and no formatting. */
#define KAI_PARK_TRACE_CAP 64

typedef struct {
    const KaiFiber *fiber;
    const char     *event;
    int             thread;
    int             state;
    int             wake_pending;
    int             reactor_fired;
    int             blocked_count;
    int             parked_count;
} KaiParkTraceEntry;

#if defined(KAI_SEPARATE_COMPILATION)
extern _Atomic int       kai_park_trace_on;
extern KaiParkTraceEntry kai_park_trace_ring[KAI_PARK_TRACE_CAP];
extern _Atomic unsigned  kai_park_trace_seq;
#  if defined(KAI_RUNTIME_OWNER)
_Atomic int       kai_park_trace_on = -1;
KaiParkTraceEntry kai_park_trace_ring[KAI_PARK_TRACE_CAP];
_Atomic unsigned  kai_park_trace_seq = 0;
#  endif
#else
static _Atomic int       kai_park_trace_on = -1;
static KaiParkTraceEntry kai_park_trace_ring[KAI_PARK_TRACE_CAP];
static _Atomic unsigned  kai_park_trace_seq = 0;
#endif

/* F2 — reactor park reasons stamped into KaiFiber.pending_park. Zero is
 * "not a reactor park" (calloc-cleared), so a spawned or mailbox-parked
 * fiber never looks pending. `kai_sched_commit_park` dispatches on these
 * to link the fiber into the matching reactor waiter structure. */
#define KAI_PARK_NONE          0
#define KAI_PARK_TIMER         1
#define KAI_PARK_PID           2
#define KAI_PARK_SOCKET_READ   3
#define KAI_PARK_SOCKET_WRITE  4
#define KAI_PARK_STDIN         5
#define KAI_PARK_SIGNAL        6
#define KAI_PARK_FILEPOOL      7
/* Non-reactor park (mailbox recv, await, send-block). Committed under the
 * fiber's slot lock — not reactor_mu — by kai_sched_commit_park. Deferred
 * like the reactor reasons so PARKED is set only after the exit swap saved
 * the fiber's ctx, closing the steal-a-half-saved-context race at N>1. */
#define KAI_PARK_SLOT          8



/* F2 forward decls — the dedicated-reactor-thread machinery. Bodies live
 * alongside the reactor implementation (commit_park, mark_ready) and the
 * scheduler primitives (drain_commit_stack). */
static void kai_sched_commit_park(KaiFiber *f);
static void kai_drain_commit_stack(KaiWorker *w);
static inline int  kai_fiber_slot_lock(KaiFiber *f);
static inline void kai_fiber_slot_unlock_at(int home);
static void kai_drain_requeue_stack(KaiWorker *w);
static void kai_reactor_mark_ready(KaiFiber *f);
static void kai_reactor_wake(void);

/* M:N forward decls — bodies live alongside the scheduler primitives.
 * `kai_sched_remote_unpark` promotes a fiber owned by another thread:
 * it enqueues onto that thread's steal deque and wakes it. Used by the
 * cross-thread mailbox send path (which runs on the sender's thread but
 * must resume a receiver parked on the owner's thread). */
static void kai_sched_remote_unpark(KaiFiber *target);
static int  kai_sched_wake_thread(int tid);
static void kai_sched_notify_work(int slot);
static void kai_sched_check_deadlock(void);
/* A fiber struct whose wrapper went to RC=0 while that fiber was still
 * running (the trampoline tail's decref of `self->value`) waits in the
 * worker's `pending_free`. At N>1 the tail's own scheduler ref waits in
 * `pending_sched_drop` instead of being dropped: the tail keeps running on
 * its stack, and a peer holding the last other ref could munmap it. The next
 * context on the same worker drains both, once the switch has left that
 * stack. Single-slot: every switch drains before the next produce. */
static void kai_pending_free_set(KaiFiber *f) { kai_worker_here()->pending_free = f; }

/* Forward decl — defined alongside the m8.x fiber stack allocator
 * later in the file, but used here by the free path (munmap needs the
 * stack_size + page_size total). */
static size_t kai_page_size(void);
static void kai_stack_unmap(void *base, size_t stack_size);

/* Linux's <sys/mman.h> exposes MAP_ANONYMOUS; older BSD-style headers
 * (and the legacy macOS spelling) use MAP_ANON. Define whichever the
 * platform omits in terms of the other so the fiber stack mmap call
 * compiles on both. */
#if !defined(MAP_ANON) && defined(MAP_ANONYMOUS)
#  define MAP_ANON MAP_ANONYMOUS
#endif

static void kai_drain_pending_free(KaiWorker *w) {
    /* The scheduler ref first: its decref may take RC to 0 and munmap the
     * doomed tail's stack, which this context has already left. */
    KaiValue *sv = w->pending_sched_drop;
    if (sv) {
        w->pending_sched_drop = NULL;
        kai_decref(sv);
    }
    KaiFiber *f = w->pending_free;
    if (!f) return;
    w->pending_free = NULL;
    /* thunk / result / linked_head were handled at wrapper-free time;
     * only stack + struct remain. The stack is an mmap region of
     * size stack_size + one guard page; pair the call with munmap
     * (free would corrupt the heap). */
#if KAI_HOST_STACK_GUARD
    if (f->stack_base) {
        kai_stack_unmap(f->stack_base, f->stack_size);
    }
#elif defined(__wasm__)
    free(f->stack_base);
#endif
    kai_tsan_fiber_free(f);
    free(f->free_stack);
    free(f);
}

/* m8 #3: wrap a heap-allocated KaiFiber in an opaque KAI_FIBER
 * value. The KaiFiber struct's lifetime is tied to the value's RC
 * (kai_free_value frees both together). The struct's `value`
 * back-pointer lets the scheduler retain its own incref on the
 * wrapper from spawn-enqueue until the trampoline tail (R4 fix). */
static KAI_RC_NOINLINE KaiValue *kai_fiber_value(KaiFiber *f) {
    KaiValue *v = kai_alloc(KAI_FIBER);
    kai_rc_make_atomic(v);
    v->as.fib = f;
    f->value  = v;
    return v;
}

/* m8 #7 + m8.x: mailbox runtime. A KaiMailbox is a singly-linked
 * list of heap-allocated KaiValue messages (head = next-to-pop,
 * tail = next-to-enqueue). Send pushes at the tail; receive pops
 * the head. Receive on an empty mailbox parks the caller on
 * recv_waiter (`kai_mailbox_pop`) and yields to the cooperative
 * scheduler; the next push wakes the head waiter (FIFO). All four
 * overflow policies (Unbounded / DropOldest / DropNewest /
 * BlockSender) reach the runtime: DropOldest / DropNewest mutate
 * the buffer in place; BlockSender parks the sender on
 * send_waiter when full and resumes when a receiver pops a slot. */
typedef struct KaiMboxNode KaiMboxNode;
struct KaiMboxNode {
    KaiValue    *msg;
    KaiMboxNode *next;
};

/* m8 #8 + m8.x: mailbox overflow policy codes (matched in
 * stdlib/actor.kai by the MailboxPolicy enum). 0 = Unbounded,
 * 1 = Bounded+DropOldest, 2 = Bounded+DropNewest,
 * 3 = Bounded+BlockSender. All four policies are implemented:
 * `kai_mailbox_alloc_bounded` accepts every code; `kai_mailbox_push`
 * dispatches on the policy and parks the sender on `send_waiter`
 * for BlockSender via the cooperative scheduler. */
#define KAI_OVERFLOW_UNBOUNDED    0
#define KAI_OVERFLOW_DROP_OLDEST  1
#define KAI_OVERFLOW_DROP_NEWEST  2
#define KAI_OVERFLOW_BLOCK_SENDER 3

typedef struct KaiMailbox KaiMailbox;
struct KaiMailbox {
    KaiMboxNode *head;
    KaiMboxNode *tail;
    int          len;
    int          cap;       /* m8 #8: 0 = unbounded; >0 = bounded */
    int          overflow;  /* m8 #8: KAI_OVERFLOW_* code */
    /* Phase 4 — blocking primitives. Waiter queues for fibers
     * parked on empty receive (head==NULL) or on full BlockSender
     * push (len>=cap). Linked through each fiber's awaiters_next
     * field — a fiber is in exactly one waiter chain at a time
     * (await chain, receiver chain, or sender chain), so the
     * single field is sufficient. */
    KaiFiber    *recv_waiter_head;
    KaiFiber    *recv_waiter_tail;
    KaiFiber    *send_waiter_head;
    KaiFiber    *send_waiter_tail;
    /* Phase 5 — owner fiber for the Link/Monitor runtime.
     * Set to kai_current_fiber() at allocation. Link.link(pid)
     * resolves pid->mb->owner_fiber to find the target fiber to
     * link to. v1 maps each mailbox to exactly one owning fiber
     * (the one that called mailbox_alloc); spawn_actor (when it
     * lands in m8.x #6) will set owner_fiber to the spawned
     * fiber instead. */
    KaiFiber    *owner_fiber;
    /* The owner's mailbox this one shadows while open. */
    KaiMailbox  *outer;
    /* M:N — guards the node list against a cross-thread `Actor.send`.
     * A same-thread send (owner on the sender's thread, the only case
     * at N=1) takes the fast path and never locks. A cross-thread send
     * deep-copies the message into the sender's heap, then appends the
     * node under this lock and wakes the owner's scheduler thread. */
    pthread_mutex_t mu;
    int             mu_inited;
    /* Set under `mu` when the owning scope ends; a push that sees it
     * drops its message instead of enqueueing. */
    int             closed;
    /* The owning scope holds one pin and every in-flight send holds
     * one, so a sender never touches a freed struct: the last unpin
     * frees it. */
    _Atomic int     pins;
};

/* Phase 5 — intrusive linked-peer chain on KaiFiber. A bidirectional
 * link between two fibers consists of one KaiLinkNode in each
 * fiber's linked_head chain pointing at the other peer. On fiber
 * termination (DONE or CANCELLED in the trampoline), the chain is
 * walked and each peer's cancel_requested flag is set. The doc
 * spec distinguishes Normal vs Crashed termination for link
 * propagation; v1 propagates on both DONE and CANCELLED (BEAM
 * trap-exit semantics is queued for post-MVP).
 *
 * The forward typedef is at the KaiFiber declaration above so
 * KaiFiber can hold a `KaiLinkNode *linked_head`. */
struct KaiLinkNode {
    KaiFiber    *peer;
    KaiLinkNode *next;
};

/* Tier 2 Monitor — intrusive node sitting on the *target* fiber's
 * monitor_head chain. observer = the fiber that called
 * Monitor.monitor(target_pid); target_pid = the same KaiValue *
 * the user passed to monitor(...) (the runtime owns one ref via
 * kai_incref so the value survives until the target terminates).
 * On termination, the propagate walker pushes target_pid into
 * observer->mailbox and frees the node.
 *
 * The forward typedef is at the KaiFiber declaration above so
 * KaiFiber can hold a `KaiMonitorNode *monitor_head`. */
struct KaiMonitorNode {
    KaiFiber       *observer;
    KaiValue       *target_pid;
    KaiMonitorNode *next;
};

/* How a fiber looks through the pids of the mailboxes it owns: alive, or
 * ended and why. The fiber and every such Pid box hold a reference, so a
 * link or monitor placed through a pid reaches the fiber for as long as it
 * runs, even after the mailbox scope closed, and learns its real exit once
 * it is gone. `lock` orders the fiber's terminate detach against link and
 * monitor placement on it. */
/* Normal (DONE), Crashed (CANCELLED) or Trapped (a runtime trap), as
 * trap-exit delivery reports it. */
typedef enum {
    KAI_EXIT_NORMAL  = 0,
    KAI_EXIT_CRASHED = 1,
    KAI_EXIT_TRAPPED = 2
} KaiExitReason;

struct KaiFiberExit {
    _Atomic int  refs;
    _Atomic int  lock;
    KaiFiber    *fiber;   /* NULL once the fiber has ended */
    int          reason;  /* KaiExitReason, valid once `fiber` is NULL */
    const char  *msg;     /* static trap message, or NULL */
};

static void kai_fiber_exit_lock(KaiFiberExit *x) {
    if (kai_nthreads <= 1) return;
    while (atomic_exchange_explicit(&x->lock, 1, memory_order_acquire)) { }
}

static void kai_fiber_exit_unlock(KaiFiberExit *x) {
    if (kai_nthreads > 1) atomic_store_explicit(&x->lock, 0, memory_order_release);
}

static void kai_fiber_exit_unref(KaiFiberExit *x) {
    if (x && atomic_fetch_sub_explicit(&x->refs, 1, memory_order_acq_rel) == 1) free(x);
}

/* `f`'s exit record with one more reference. Must run before `f` can
 * terminate: on `f` itself, or before it is enqueued. */
static KaiFiberExit *kai_fiber_exit_ref(KaiFiber *f) {
    KaiFiberExit *x = atomic_load_explicit(&f->exit_rec, memory_order_acquire);
    if (!x) {
        KaiFiberExit *fresh = (KaiFiberExit *) calloc(1, sizeof(KaiFiberExit));
        if (!fresh) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
        atomic_init(&fresh->refs, 1);
        fresh->fiber = f;
        if (atomic_compare_exchange_strong(&f->exit_rec, &x, fresh)) x = fresh;
        else free(fresh);
    }
    atomic_fetch_add_explicit(&x->refs, 1, memory_order_relaxed);
    return x;
}

/* Phase 4 forward decls: mailbox push/pop park/wake the calling
 * fiber via the scheduler primitives, which are defined further
 * down (after the handler-stack runtime). The decls here let the
 * mailbox ops compile in their natural file location. */
static void kai_sched_park(void);
static void kai_sched_unpark(KaiFiber *target);
/* Issue #679: park sites in the reactor call this after wake to
 * observe a sibling-triggered cancel before retrying their syscall.
 * Body lives near the op-call lookup prologue at line 8868+. */
static void kai_check_cancel_yield_point(void);
/* Dispatch a pending cancellation through the innermost user `with
 * Cancel` handler on this fiber. Does not return when one is in scope;
 * returns 0 when there is none. Body sits beside the yield-point hook. */
static int kai_cancel_dispatch_user_handler(void);
/* kai_sched_park without the cancel check on resume, for a site linked
 * into a chain it must leave before it unwinds. */
static void kai_sched_park_linked(void);
/* The program's main fiber has no pad: its cancel runs the user handler or
 * ends the process. */
static inline int kai_cancel_can_land(KaiFiber *f) {
    return f->cancel_pad_set || f->pinned_main ||
           (f->worker && f == &f->worker->main_fiber);
}
/* True when a requested cancellation can be delivered to `f` now. */
static inline int kai_cancel_due(KaiFiber *f) {
    return f->cancel_requested && !f->cancel_delivered && kai_cancel_can_land(f);
}
static void kai_cancel_deliver(KaiFiber *f);
static void kai_fiber_request_cancel(KaiFiber *f);
/* Splice a fiber off whatever reactor waiter list holds it; defined with
 * the reactor below, needed above by the select loser-cancel walk. */
static int kai_reactor_detach_fiber(KaiFiber *target);

/* Issue #611 — Phase R1 reactor forward decls. The default Clock /
 * File / Process handlers live above the reactor implementation
 * but call into it through the small API below to park their
 * fibers. Bodies sit alongside the scheduler primitives a few
 * thousand lines further down. */
typedef struct KaiFilepoolItem KaiFilepoolItem;
static void     kai_reactor_init(void);
static void     kai_reactor_park_timer(KaiFiber *f, uint64_t deadline_ns);
static void     kai_reactor_park_timer_linked(KaiFiber *f, uint64_t deadline_ns);
static void     kai_reactor_park_pid(KaiFiber *f, int pid);
static int      kai_reactor_take_child_exit(int pid, int *status);
static KaiValue *kai_reactor_run_in_pool(KaiValue *(*work)(void *), void *arg);
static uint64_t kai_reactor_now_ns(void);
/* Timeout-receive dual-park: arm a deadline alongside the mailbox park
 * and disarm it if a message wins. Bodies sit with the timer wheel. */
static void     kai_reactor_timer_insert(KaiFiber *f);
static int      kai_reactor_timer_remove(KaiFiber *f);
static int      kai_reactor_detach_fiber(KaiFiber *target);
/* Every writer of the wheel takes these, the disarm above included. */
static inline void kai_reactor_lock(void);
static inline void kai_reactor_unlock(void);

/* Issue #620 — Phase R3 reactor forward decls. The default Stdin
 * handlers live ~1700 lines above the reactor implementation. They
 * call into the small park-stdin helper which encapsulates the
 * single-fiber slot, the parked-count bump, and the kai_sched_park
 * yield. The nonblock-once helper is also forward-declared. */
static int       kai_reactor_park_stdin(KaiFiber *f);
static void      kai_reactor_stdin_set_nonblocking(void);

/* Issue #630 — Phase R2 reactor forward decls. The six NetTcp
 * default handlers live ~1000 lines above the reactor; they call
 * these helpers to park on read/write readiness of a socket fd.
 * A fiber sits on at most one socket-direction list at a time
 * (a single op is either reading or writing, never both), so the
 * existing `reactor_next` intrusive slot is sufficient. The fd is
 * stashed in `reactor_wait_pid` (the slot is otherwise unused while
 * a fiber is parked on a socket — pids and sockets are mutually
 * exclusive park reasons). */
static void      kai_reactor_park_socket_read(KaiFiber *f, int fd);
static void      kai_reactor_park_socket_write(KaiFiber *f, int fd);
/* Read-readiness park with a deadline: parks on the read-waiter list
 * carrying `deadline_ns` in the fiber's own slot (single-list dual-park,
 * no second intrusive link). Returns 1 if readiness woke us, 0 if the
 * deadline fired first. */
static int       kai_reactor_park_socket_read_timeout(KaiFiber *f, int fd, uint64_t deadline_ns);
static void      kai_socket_set_nonblock(int fd);

/* Issue #671 — Phase R4: park `f` on the singleton signal waiter
 * slot and yield. Returns 0 on success, -1 if a second fiber is
 * already parked (concurrent `Signal.await()` is undefined — v1
 * panics at the call site with a clear diagnostic, mirroring the
 * R3 stdin-multiplex contract). The signo arrives on resume via
 * `f->reactor_wait_status`. */
static int       kai_reactor_park_signal(KaiFiber *f);

/* Issue #671 — Phase R4: async-signal-safe handler installed for
 * every subscribed signal. Forward-declared because it is used by
 * `kai_default_signal_on` (~line 6300) but its body lives down in
 * the reactor block (~line 7030, alongside kai_reactor_sigchld_handler). */
static void      kai_reactor_signal_handler(int sig);

/* M:N — arm the per-mailbox cross-thread lock. The mailbox is created
 * on one thread (its allocating fiber), so this init never races. At
 * N=1 the lock is armed but never taken (every send is same-thread). */
static void kai_mailbox_init_mu(KaiMailbox *mb) {
    if (kai_nthreads > 1 && !mb->mu_inited) {
        pthread_mutex_init(&mb->mu, NULL);
        mb->mu_inited = 1;
    }
}

/* Serialize a mailbox op against cross-thread senders. A no-op unless
 * the lock was armed (N>1), so the single-thread pop/push paths stay
 * lock-free and byte-identical. The owner's pop and same-thread pushes
 * run on one thread cooperatively — they need the lock only to exclude
 * a concurrent cross-thread send, never each other. */
static inline void kai_mbox_lock(KaiMailbox *mb)   { if (mb->mu_inited) pthread_mutex_lock(&mb->mu); }
static inline void kai_mbox_unlock(KaiMailbox *mb) { if (mb->mu_inited) pthread_mutex_unlock(&mb->mu); }

/* An owned mailbox is stamped onto the allocating fiber (with_mailbox:
 * that fiber IS the actor) so link/monitor/trap-exit find it without a
 * registry. An unowned one leaves the caller's `mailbox` slot untouched —
 * spawn_actor wires it to the spawned fiber instead, and stamping the
 * parent would corrupt its own lookups whenever it already owns one. */
static KaiMailbox *kai_mailbox_new(int cap, int overflow, int owned) {
    KaiMailbox *mb = (KaiMailbox *) calloc(1, sizeof(KaiMailbox));
    if (!mb) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
    kai_mbox_count_alloc();
    kai_mailbox_init_mu(mb);
    mb->cap      = cap;
    mb->overflow = overflow;
    atomic_init(&mb->pins, 1);
    if (owned) {
        mb->owner_fiber = kai_current_fiber();
        if (mb->owner_fiber) {
            mb->outer = mb->owner_fiber->mailbox;
            mb->owner_fiber->mailbox = mb;
        }
    }
    return mb;
}

static KaiMailbox *kai_mailbox_alloc(void) {
    return kai_mailbox_new(0, KAI_OVERFLOW_UNBOUNDED, 1);
}

static KaiMailbox *kai_mailbox_alloc_unowned(void) {
    return kai_mailbox_new(0, KAI_OVERFLOW_UNBOUNDED, 0);
}

static KaiMailbox *kai_mailbox_alloc_bounded(int cap, int overflow) {
    return kai_mailbox_new(cap, overflow, 1);
}

static KaiMailbox *kai_mailbox_alloc_bounded_unowned(int cap, int overflow) {
    return kai_mailbox_new(cap, overflow, 0);
}

/* Phase 4 helper: link a fiber into a waiter chain at the tail. */
static void kai_mailbox_waiter_enqueue(KaiFiber **head, KaiFiber **tail, KaiFiber *f) {
    f->awaiters_next = NULL;
    if (*tail) {
        (*tail)->awaiters_next = f;
    } else {
        *head = f;
    }
    *tail = f;
}

/* Phase 4 helper: pop the head waiter from a chain (FIFO wakeup). */
static KaiFiber *kai_mailbox_waiter_dequeue(KaiFiber **head, KaiFiber **tail) {
    KaiFiber *f = *head;
    if (!f) return NULL;
    *head = f->awaiters_next;
    if (!*head) *tail = NULL;
    f->awaiters_next = NULL;
    return f;
}

/* Awaiter-chain hygiene under the owner's slot lock. A spurious resume can
 * leave the waiter linked, so a re-wait must not double-link and a terminal
 * observation must not walk away leaving a stale link (the waiter's
 * awaiters_next is re-used by its next wait). */
static void kai_awaiter_link_if_absent(KaiFiber *owner, KaiFiber *me) {
    for (KaiFiber *c = owner->awaiters_head; c; c = c->awaiters_next) {
        if (c == me) return;
    }
    me->awaiters_next = owner->awaiters_head;
    owner->awaiters_head = me;
}
static void kai_awaiter_unlink(KaiFiber *owner, KaiFiber *me) {
    KaiFiber **link = &owner->awaiters_head;
    while (*link) {
        if (*link == me) {
            *link = me->awaiters_next;
            me->awaiters_next = NULL;
            return;
        }
        link = &(*link)->awaiters_next;
    }
}

/* Select-chain membership. Both sides run under `owner`'s slot lock, so a
 * candidate's terminate walk cannot snapshot the chain between a selector's
 * state read and its link. `node` belongs to the selector's stack frame and
 * must be unlinked from every candidate before that frame returns. */
static void kai_select_link(KaiFiber *owner, KaiSelectWaiter *node) {
    node->next = owner->select_waiters_head;
    owner->select_waiters_head = node;
}
static void kai_select_unlink(KaiFiber *owner, KaiSelectWaiter *node) {
    KaiSelectWaiter **link = &owner->select_waiters_head;
    while (*link) {
        if (*link == node) {
            *link = node->next;
            node->next = NULL;
            return;
        }
        link = &(*link)->next;
    }
}

/* Splice `f` out of a waiter chain regardless of position. Returns 1
 * if found. The timeout-receive uses this to drop itself from the
 * recv-waiter chain when its deadline fires first, so a later send
 * does not unpark a fiber that already returned `None`. */
static int kai_mailbox_waiter_remove(KaiFiber **head, KaiFiber **tail, KaiFiber *f) {
    KaiFiber **link = head;
    KaiFiber  *prev = NULL;
    while (*link) {
        if (*link == f) {
            *link = f->awaiters_next;
            if (!*link) *tail = prev;
            f->awaiters_next = NULL;
            return 1;
        }
        prev = *link;
        link = &(*link)->awaiters_next;
    }
    return 0;
}

/* BlockSender back-pressure: park the calling fiber until a receiver frees a
 * slot. Shared by both push paths, because which one a send takes depends
 * only on where the receiver happens to be scheduled — a policy that applied
 * on one and not the other would make delivery a function of placement.
 *
 * The full-check and the waiter-enqueue happen together under the mailbox
 * lock, mirroring kai_mailbox_pop's recv side: a pop that frees a slot either
 * runs before our check (we see room) or after our enqueue (it dequeues and
 * wakes us). Dropping the lock before the park is what keeps that safe —
 * kai_sched_park leaves the fiber RUNNING until the scheduler root commits it,
 * and a wake landing in that window is recorded as wake_pending rather than
 * lost. The loop re-checks because another sender can take the slot between
 * our wake and our resume.
 *
 * Caller must NOT hold the mailbox lock. Returns holding it: 1 with room in
 * the mailbox — the caller enqueues under that same acquisition, so two
 * senders racing on the last slot cannot both pass the check and overshoot
 * `cap` — or 0 once the mailbox is closed or a cancel is due; the sender
 * then drops its message and the cancel is delivered by the send entry
 * point after it releases its references. */
static int kai_mailbox_park(KaiMailbox *mb, KaiFiber **head, KaiFiber **tail, KaiFiber *me);
static int kai_mailbox_await_slot(KaiMailbox *mb) {
    for (;;) {
        KaiFiber *me = kai_current_fiber();
        kai_mbox_lock(mb);
        /* A spurious resume leaves us linked; unlink before deciding, so a
         * re-enqueue below never double-links the chain. */
        kai_mailbox_waiter_remove(&mb->send_waiter_head,
                                  &mb->send_waiter_tail, me);
        if (mb->closed) return 0;
        if (mb->len < mb->cap) return 1;
        kai_mailbox_waiter_enqueue(&mb->send_waiter_head,
                                   &mb->send_waiter_tail, me);
        kai_mbox_unlock(mb);
        if (kai_mailbox_park(mb, &mb->send_waiter_head,
                             &mb->send_waiter_tail, me)) {
            kai_mbox_lock(mb);
            return 0;
        }
    }
}

/* Apply a drop policy to a full mailbox, under its lock. Returns 0 when the
 * incoming message is the one to drop. */
static int kai_mailbox_make_room(KaiMailbox *mb) {
    if (mb->cap == 0 || mb->len < mb->cap) return 1;
    if (mb->overflow == KAI_OVERFLOW_DROP_NEWEST) return 0;
    if (mb->overflow == KAI_OVERFLOW_DROP_OLDEST) {
        KaiMboxNode *old = mb->head;
        mb->head = old->next;
        if (!mb->head) mb->tail = NULL;
        kai_decref(old->msg);
        free(old);
        mb->len--;
    }
    return 1;
}

/* Enforce the overflow policy and return holding the lock, or 0 if the
 * message was dropped — by policy or because the mailbox is closed (lock
 * released, msg consumed). Shared by both push paths so a policy cannot mean
 * two different things depending on which thread the receiver landed on. */
static int kai_mailbox_reserve_slot(KaiMailbox *mb, KaiValue *msg) {
    int room;
    if (mb->cap > 0 && mb->overflow == KAI_OVERFLOW_BLOCK_SENDER) {
        room = kai_mailbox_await_slot(mb);
    } else {
        kai_mbox_lock(mb);
        room = !mb->closed && kai_mailbox_make_room(mb);
    }
    if (!room) {
        kai_mbox_unlock(mb);
        kai_decref(msg);
    }
    return room;
}

static void kai_mailbox_push(KaiMailbox *mb, KaiValue *msg) {
    /* m8 #8 + Phase 4: enforce policy on full. */
    if (!kai_mailbox_reserve_slot(mb, msg)) return;
    KaiMboxNode *node = (KaiMboxNode *) calloc(1, sizeof(KaiMboxNode));
    if (!node) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
    node->msg  = msg;
    node->next = NULL;
    if (mb->tail) { mb->tail->next = node; }
    else          { mb->head       = node; }
    mb->tail = node;
    mb->len++;
    /* Phase 4: wake one parked receiver if any. */
    KaiFiber *waiter = kai_mailbox_waiter_dequeue(&mb->recv_waiter_head,
                                                    &mb->recv_waiter_tail);
    kai_mbox_unlock(mb);
    if (waiter) kai_sched_unpark(waiter);
}

/* M:N cross-thread send. `msg` has already been deep-copied into the
 * SENDER's heap by the caller — the copied tree is single-owner (rc=1)
 * and migrates to the receiver like a fiber does (freed on the
 * receiver's thread, into its own TLS pools). We take the mailbox lock,
 * append the node, and — if the owner is parked waiting to receive —
 * promote it on ITS scheduler thread and wake that thread.
 *
 * Bounded policies mean the same thing here as on the same-thread path: a
 * BlockSender mailbox parks the sender, it does not discard. The wake crosses
 * threads on its own — kai_mailbox_pop already routes send-waiter wakes
 * through kai_sched_unpark, which promotes on the target's home thread. */
static void kai_mailbox_push_cross_thread(KaiMailbox *mb, KaiValue *msg) {
    if (!kai_mailbox_reserve_slot(mb, msg)) return;
    KaiMboxNode *node = (KaiMboxNode *) calloc(1, sizeof(KaiMboxNode));
    if (!node) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
    node->msg  = msg;
    node->next = NULL;
    if (mb->tail) mb->tail->next = node;
    else          mb->head       = node;
    mb->tail = node;
    mb->len++;
    KaiFiber *waiter = kai_mailbox_waiter_dequeue(&mb->recv_waiter_head,
                                                  &mb->recv_waiter_tail);
    kai_mbox_unlock(mb);
    if (waiter) kai_sched_remote_unpark(waiter);
}

/* Park `me` on the waiter chain it is already linked into. Returns 0 on an
 * ordinary resume, which is a hint the caller re-checks. Returns 1 when a
 * cancel is due: `me` is off the chain, so no later send can wake a fiber
 * that has unwound, and the caller delivers the cancel once it has released
 * what it holds. */
static int kai_mailbox_park(KaiMailbox *mb, KaiFiber **head, KaiFiber **tail, KaiFiber *me) {
    atomic_store(&me->cancel_wake, 1);
    if (!kai_cancel_due(me)) kai_sched_park_linked();
    atomic_store(&me->cancel_wake, 0);
    if (!kai_cancel_due(me)) return 0;
    kai_mbox_lock(mb);
    kai_mailbox_waiter_remove(head, tail, me);
    kai_mbox_unlock(mb);
    return 1;
}

/* Unlink the head message under the mailbox lock; NULL when the mailbox is
 * empty. `*sender` receives the blocked sender the freed slot admits, for
 * the caller to unpark once it has released the lock. */
static KaiValue *kai_mailbox_take(KaiMailbox *mb, KaiFiber **sender) {
    KaiMboxNode *node = mb->head;
    if (!node) return NULL;
    mb->head = node->next;
    if (!mb->head) { mb->tail = NULL; }
    KaiValue *msg = node->msg;
    free(node);
    mb->len--;
    *sender = kai_mailbox_waiter_dequeue(&mb->send_waiter_head,
                                         &mb->send_waiter_tail);
    return msg;
}

/* Returns NULL when a cancel is due instead of a message: the caller
 * releases its references and delivers it. */
static KaiValue *kai_mailbox_pop(KaiMailbox *mb) {
    /* Phase 4: park the calling fiber until a sender enqueues. The
     * loop handles the case where another receiver took the slot
     * between our unpark and resume (rare but possible if multiple
     * fibers race on the same mailbox). Under M:N the empty-check and
     * the waiter-enqueue happen under the lock so a concurrent
     * cross-thread send cannot slip a message in and signal a wake
     * between our check and our park (lost-wakeup); the lock is
     * released before the park itself. */
    for (;;) {
        KaiFiber *me = kai_current_fiber();
        KaiFiber *waiter = NULL;
        kai_mbox_lock(mb);
        /* A spurious resume leaves us linked; unlink before deciding, so the
         * re-enqueue below never double-links the chain. */
        kai_mailbox_waiter_remove(&mb->recv_waiter_head,
                                  &mb->recv_waiter_tail, me);
        KaiValue *msg = kai_mailbox_take(mb, &waiter);
        if (msg) {
            kai_mbox_unlock(mb);
            if (waiter) kai_sched_unpark(waiter);
            return msg;
        }
        kai_mailbox_waiter_enqueue(&mb->recv_waiter_head,
                                    &mb->recv_waiter_tail, me);
        kai_mbox_unlock(mb);
        if (kai_mailbox_park(mb, &mb->recv_waiter_head,
                             &mb->recv_waiter_tail, me)) return NULL;
    }
}

/* Receive with a deadline: park the caller on BOTH the recv-waiter
 * chain and the timer wheel, woken by whichever fires first. Returns
 * the popped message, or NULL if `timeout_nanos` elapsed first.
 *
 * The discriminant on wake is recv-waiter membership: a send unparks
 * us by dequeuing us from the recv-waiter chain (we stay on the timer
 * wheel), while the timer drain unparks us off the wheel (we stay on
 * the recv-waiter chain). Whichever wakeup ran, we splice ourselves
 * out of the OTHER structure so a later event cannot touch a fiber
 * that already returned — the use-after-free trap this guards.
 *
 * A spurious message-wake (another receiver drained the slot first)
 * re-parks for the remaining time; an elapsed deadline returns NULL.
 * A due cancel also returns NULL, with `me` off both structures: the caller
 * releases what it holds and delivers it. */
static KaiValue *kai_mailbox_pop_timeout(KaiMailbox *mb, uint64_t timeout_nanos) {
    KaiFiber *me = kai_current_fiber();
    KaiFiber *sw = NULL;
    kai_mbox_lock(mb);
    KaiValue *msg = kai_mailbox_take(mb, &sw);
    kai_mbox_unlock(mb);
    uint64_t deadline = msg ? 0 : kai_reactor_now_ns() + timeout_nanos;
    while (!msg) {
        int expired = kai_reactor_now_ns() >= deadline;
        /* The empty-check and the link share one acquisition, as in
         * kai_mailbox_pop: a send that lands between them finds no waiter
         * to wake, and only the deadline would end the park. */
        kai_mbox_lock(mb);
        msg = kai_mailbox_take(mb, &sw);
        if (!msg && !expired) {
            kai_mailbox_waiter_enqueue(&mb->recv_waiter_head,
                                       &mb->recv_waiter_tail, me);
        }
        kai_mbox_unlock(mb);
        if (msg || expired) break;
        if (!kai_cancel_due(me)) kai_reactor_park_timer_linked(me, deadline);

        /* park_timer's drain already spliced us off the wheel on a
         * deadline wake. Disarm it on a message wake; in both cases
         * remove ourselves from the chain we are still linked into. */
        kai_mbox_lock(mb);
        int still_waiting = kai_mailbox_waiter_remove(&mb->recv_waiter_head,
                                                      &mb->recv_waiter_tail, me);
        if (!still_waiting) msg = kai_mailbox_take(mb, &sw);
        kai_mbox_unlock(mb);
        if (still_waiting) {
            /* Never dequeued by a send. Only the reactor's own splice
             * (reactor_fired) means the deadline really fired; a resume
             * without it is spurious — loop, which re-checks the clock
             * and re-parks for whatever time remains. */
            if (me->reactor_fired || kai_cancel_due(me)) break;
            continue;
        }
        /* A send dequeued us; disarm the still-armed deadline timer. The
         * wheel belongs to the reactor, so splice under its lock — and hold
         * that lock alone: the mailbox lock is released above so the two
         * never nest, keeping the runtime's one-lock-at-a-time discipline.
         * The take above can find the slot already drained by another
         * receiver, which is the spurious case the loop re-parks on. */
        kai_reactor_lock();
        kai_reactor_timer_remove(me);
        kai_reactor_unlock();
    }
    if (sw) kai_sched_unpark(sw);
    return msg;
}

static void kai_mailbox_unpin(KaiMailbox *mb) {
    if (atomic_fetch_sub_explicit(&mb->pins, 1, memory_order_acq_rel) != 1) return;
    if (mb->mu_inited) pthread_mutex_destroy(&mb->mu);
    free(mb);
    kai_mbox_count_free();
}

/* End the owning scope: queued messages are released on the owner's thread,
 * later pushes drop theirs, and senders parked on a full BlockSender mailbox
 * are woken to observe the close instead of waiting forever. */
static void kai_mailbox_close(KaiMailbox *mb) {
    /* Only the owner closes, so its chain is not read concurrently here.
     * An aborted inner scope never closes, so `mb` need not be innermost. */
    if (mb->owner_fiber) {
        KaiMailbox **slot = &mb->owner_fiber->mailbox;
        while (*slot && *slot != mb) slot = &(*slot)->outer;
        if (*slot) *slot = mb->outer;
    }
    kai_mbox_lock(mb);
    mb->closed = 1;
    mb->owner_fiber = NULL;
    KaiMboxNode *node = mb->head;
    mb->head = mb->tail = NULL;
    mb->len = 0;
    kai_mbox_unlock(mb);
    while (node) {
        KaiMboxNode *next = node->next;
        kai_decref(node->msg);
        free(node);
        node = next;
    }
    for (;;) {
        kai_mbox_lock(mb);
        KaiFiber *sw = kai_mailbox_waiter_dequeue(&mb->send_waiter_head,
                                                  &mb->send_waiter_tail);
        kai_mbox_unlock(mb);
        if (!sw) break;
        kai_sched_unpark(sw);
    }
    kai_mailbox_unpin(mb);
}

/* A mailbox has exactly one Pid box, shared by every handle (copies across
 * threads incref it atomically), and closing nulls its `as.mb`. So a
 * NULL there means "ended" and no handle can point at a freed mailbox. Under
 * M:N the stripe lock orders a sender's read-and-pin against the owner's
 * null, closing the window where the mailbox could be released between the
 * two. */
#define KAI_PID_STRIPES 64
#if defined(KAI_SEPARATE_COMPILATION)
extern _Atomic int kai_pid_stripe[KAI_PID_STRIPES];
#  if defined(KAI_RUNTIME_OWNER)
_Atomic int kai_pid_stripe[KAI_PID_STRIPES];
#  endif
#else
static _Atomic int kai_pid_stripe[KAI_PID_STRIPES];
#endif

static _Atomic int *kai_pid_stripe_lock(KaiValue *pid) {
    if (kai_nthreads <= 1) return NULL;
    _Atomic int *s = &kai_pid_stripe[((uintptr_t) pid >> 4) % KAI_PID_STRIPES];
    while (atomic_exchange_explicit(s, 1, memory_order_acquire)) { }
    return s;
}

static void kai_pid_stripe_unlock(_Atomic int *s) {
    if (s) atomic_store_explicit(s, 0, memory_order_release);
}

/* Returns the Pid's mailbox pinned for one send, or NULL once it has ended. */
static KaiMailbox *kai_mailbox_pin(KaiValue *pid) {
    _Atomic int *s = kai_pid_stripe_lock(pid);
    KaiMailbox *mb = pid->as.mb;
    if (mb) atomic_fetch_add_explicit(&mb->pins, 1, memory_order_relaxed);
    kai_pid_stripe_unlock(s);
    return mb;
}

/* m8 #7: wrap a borrowed mailbox pointer as a KAI_PID value. The
 * mailbox itself is owned by the with_mailbox / spawn_actor scope
 * that allocated it; Pid values are non-owning handles that just
 * carry the address. */
static KAI_RC_NOINLINE KaiValue *kai_pid_value(KaiMailbox *mb) {
    KaiValue *v = kai_alloc(KAI_PID);
    v->as.mb = mb;
    if (mb->owner_fiber) v->as.pid.owner = kai_fiber_exit_ref(mb->owner_fiber);
    /* Senders on any scheduler thread dup and drop this box, so its count is
     * atomic. Freeing it never touches the mailbox, which its scope owns. */
    kai_rc_make_atomic(v);
    return v;
}

/* The recycle-this-cell tail of kai_free_one. Runs the
 * trace/poison bookkeeping (#812 counters, #296 alloc-site credit,
 * history log, poison stamp) and returns the chunk to the pool (or
 * libc), then bumps the free counters. Must run exactly once per
 * reclaimed cell — double invocation is a double-free + double counter.
 * Reads `_kc->tag` / `_kc->var_n_args` BEFORE the poison stamp clobbers
 * them, so callers must not have poisoned the cell. */
#define KAI_RECYCLE_CELL(cell) do {                                          \
    KaiValue *_kc = (cell);                                                  \
    KAI_RC_RECYCLE_TRACE(_kc);                                               \
    KAI_RC_RECYCLE_POOL(_kc);                                                \
    kai_rc_count_free();                                                     \
} while (0)

