/* Fragment of runtime.h; include runtime.h, never this file. */
static inline int kai_rc_is_immortal(const KaiValue *v) {
    return kai_rc_load(v) == INT32_MAX;
}

/* Put a fresh cell (rc 1) under the atomic count. */
static inline void kai_rc_make_atomic(KaiValue *v) {
    atomic_store_explicit((_Atomic int32_t *) &v->rc, KAI_RC_SPECIAL + 1,
                          memory_order_relaxed);
}

KAI_RT_SHARED __attribute__((noinline, cold)) void kai_rc_overflow(const KaiValue *v) {
    kai_eprintf("kai: reference count overflow on a %s cell\n",
            v->tag == KAI_FIBER ? "fiber" : "heap");
    abort();
}

/* An atomic cell's increment, and the trap for a plain count about to
 * reach KAI_RC_SPECIAL. */
KAI_RT_SHARED __attribute__((noinline)) KaiValue *kai_incref_special(KaiValue *v, int32_t r) {
    if (r < KAI_RC_SPECIAL) kai_rc_overflow(v);
    if (atomic_fetch_add_explicit((_Atomic int32_t *) &v->rc, 1,
                                  memory_order_relaxed) >= INT32_MAX - 1)
        kai_rc_overflow(v);
    KAI_RC_TRAFFIC_INC(kai_rc_incref_total);
#ifdef KAI_TRACE_RC
    kai_rc_history_log(v, /* op=incref */ 1, v->tag);
#endif
    return v;
}

/* Increment a reference. Pure fast path — no free, no out-of-line case
 * — so the whole body is inlined into the caller. Previously a non-inline
 * `static KaiValue *` (a real call at every dup, the symmetric cost to
 * the old non-inline kai_decref). Timing hooks would have
 * blocked the inline. Under tracing the
 * counters still fire. Koka's kk_block_dup is likewise inline. */
KAI_RT_SHARED_INLINE KaiValue *kai_incref(KaiValue *v) {
    if (kai_is_value(v) || !v) return v;
    int32_t r = kai_rc_load(v);
    if (r >= KAI_RC_SPECIAL - 1) {
        if (r != INT32_MAX) return kai_incref_special(v, r);
#ifdef KAI_TRACE_RC
        kai_rc_history_log(v, /* op=incref */ 1, v->tag);
#endif
        return v;
    }
    v->rc = r + 1;
    KAI_RC_TRAFFIC_INC(kai_rc_incref_total);
#ifdef KAI_TRACE_RC
    kai_rc_history_log(v, /* op=incref */ 1, v->tag);
#endif
    return v;
}
KAI_RT_SHARED void       kai_decref(KaiValue *v);

/* m8 #1/#3: KaiFiber definitions sit here (before kai_free_value)
 * because KAI_FIBER values own their KaiFiber struct and the free
 * path needs the full layout. The handler-stack runtime
 * (KaiEvidence + push/pop/lookup) lives further down; KaiFiber only
 * holds a `KaiEvidence *`, so a forward declaration is enough. The
 * Spawn default handlers (kai_default_spawn_*) come later in the
 * file too — they reach the struct through this declaration and use
 * kai_apply (defined further down) to invoke spawned thunks. */
typedef struct KaiEvidence KaiEvidence;

/* m8 #3 + m8.x: fiber lifecycle states. The pre-v0.4.0 runtime ran
 * `spawn` synchronously and only ever needed NEW/DONE; the m8.x
 * cooperative scheduler (landed v0.4.0) adds READY (enqueued in the
 * run queue), RUNNING (currently dispatched), and PARKED (blocked
 * on await / receive / send). Spec: docs/fibers-impl.md §*Fiber
 * state machine*. The numeric values are not stable across versions
 * — no ABI commitment yet (CLAUDE.md "Backward compatibility — not
 * promised until post-MVP"). */
typedef enum {
    KAI_FIBER_NEW       = 0,
    KAI_FIBER_READY     = 1,  /* m8.x */
    KAI_FIBER_RUNNING   = 2,  /* m8.x */
    KAI_FIBER_PARKED    = 3,  /* m8.x */
    KAI_FIBER_DONE      = 4,
    KAI_FIBER_CANCELLED = 5
} KaiFiberState;

typedef struct KaiFiber   KaiFiber;
/* `n` consecutive slots a frame owns across a call that can unwind; a
 * non-local exit that skips the frame releases each non-NULL slot. A
 * negative `n` names what `base` is instead: a handle's node, a lazy
 * cell whose thunk is running, or the initialiser of a constant being
 * evaluated. */
typedef struct { KaiValue **base; intptr_t n; } KaiUnwEntry;
#define KAI_UNW_HANDLE (-1)
#define KAI_UNW_SEQ    (-2)
#define KAI_UNW_CONST  (-3)
/* A constant this fiber evaluated, keyed by its initialiser; not `ready`
 * while the initialiser runs. */
typedef struct { KaiFn init; KaiValue *value; int ready; } KaiConstCell;
/* What an op clause runs after a non-tail `resume`, waiting for the exit
 * of the handle whose node it names. */
typedef struct { KaiEvidence *node; KaiValue *clo; } KaiResumeFrame;
typedef struct KaiNursery KaiNursery;     /* issue #959 — defined below */

/* One selector's membership in one candidate's select chain. A fiber in
 * Spawn.select allocates an array of these on its own stack and links one
 * into every candidate, so N candidates can each reach the selector. */
typedef struct KaiSelectWaiter KaiSelectWaiter;
struct KaiSelectWaiter {
    KaiFiber        *waiter;   /* the fiber parked in select      */
    KaiSelectWaiter *next;     /* next node in the candidate chain */
};
typedef struct KaiLinkNode KaiLinkNode;  /* Phase 5 — defined below */
typedef struct KaiMonitorNode KaiMonitorNode;  /* Tier 2 Monitor — defined below */
typedef struct KaiFiberExit KaiFiberExit;

struct KaiFiber {
    KaiEvidence    *evidence_top;
    /* M:N — the ONE per-fiber field that goes atomic. A cross-thread
     * Spawn.cancel writes it (relaxed) while the target reads it at its
     * own yield points (acquire); making it _Atomic keeps that the sole
     * synchronized field and leaves the object-RC hot path atomic-free.
     * At N=1 an _Atomic int on this platform is a plain aligned int. */
    _Atomic int     cancel_requested;  /* Spawn.cancel(target) sets this (#4) */
    int             cancel_delivered;  /* Cancel.raise() injected once (#4)   */
    KaiFiber       *sched_next;        /* intrusive ready-queue link          */
    KaiFiber       *parent;            /* spawning fiber, NULL for the root   */
    /* M:N — the fiber's lifecycle state transitions under several different
     * locks depending on the path (the parker's slot in kai_sched_park, the
     * target's slot in remote_unpark, kai_reactor_mu in commit_park, the
     * child's slot in the await/terminate handshake) and is read locklessly
     * by peers, so it is _Atomic: that is the single field with no one
     * covering lock, and making it atomic keeps every transition race-free
     * without forcing one global scheduler lock. A plain aligned int at N=1. */
    _Atomic KaiFiberState   state;
    KaiValue       *thunk;             /* held alive while the fiber runs     */
    KaiValue       *result;            /* set on DONE; what await returns     */
    /* m8.x cooperative scheduler additions. Spec: docs/fibers-impl.md
     * §*Scheduler*. main_fiber leaves ctx zero-initialised (filled in
     * by getcontext on first dispatch); spawned fibers fill ctx via
     * makecontext + a heap-allocated stack. */
#if KAI_HOST_FIBERS
    ucontext_t      ctx;               /* swapcontext target              */
#else
    char            ctx[1];
#endif
    void           *stack_base;        /* heap-allocated; freed on RC=0   */
    size_t          stack_size;        /* set per fiber (env-configurable) */
    KaiFiber       *awaiters_head;     /* per-fiber awaiter chain head    */
    KaiFiber       *awaiters_next;     /* link into another fiber's chain */
    /* Spawn.select waiters. Separate from the awaiter chain because
     * `awaiters_next` is a single link: a selector waits on every
     * candidate at once, so its membership cannot be one pointer in
     * its own struct. Each node is owned by the selector's stack frame
     * and lives only while it is parked. Guarded by the candidate's
     * slot lock, exactly like `awaiters_head`. */
    KaiSelectWaiter *select_waiters_head;
    /* m8.x Phase 3 — Cancel delivery at yield points. The trampoline
     * sets up cancel_pad with setjmp before running the body; the
     * yield-point hook in kai_evidence_lookup* longjmps here when the
     * fiber's cancel_requested flag fires, unwinding the body to the
     * trampoline's cancel branch (state=CANCELLED). cancel_pad_set
     * gates the longjmp so it's only attempted while the pad is live
     * — main_fiber never runs through the trampoline, so its
     * cancel_pad stays unset and Cancel.raise() in main falls back to
     * exit(0) (the m8 v1 behaviour for unhandled root cancellation). */
    jmp_buf         cancel_pad;
    int             cancel_pad_set;
    /* Fiber-level trap isolation. A recoverable trap (index out of
     * range, divide by zero, non-exhaustive match) unwinds to
     * cancel_pad like Cancel does, but sets `trapped` so the
     * trampoline reports KAI_EXIT_TRAPPED instead of CRASHED — a
     * program bug, distinguishable from cooperative cancellation.
     * trap_msg is a static C string (never freed). With no pad
     * installed (main_fiber) a trap terminates the process. */
    int             trapped;
    const char     *trap_msg;
    /* Phase 5 — intrusive list of linked peer fibers. Walked at
     * trampoline termination (DONE or CANCELLED branches) to set
     * cancel_requested on each peer. Owned by the fiber; nodes are
     * freed during the propagation walk and as a safety net in
     * kai_free_value's KAI_FIBER branch. */
    KaiLinkNode    *linked_head;
    /* Per-fiber dispatch state. Points at the evidence node whose clause
     * body is currently on this fiber's stack, or NULL otherwise.
     * `kai_evidence_lookup_node` skips it so a `Eff.op(...)` invoked from
     * inside the clause resolves to the outer handler instead of recursing.
     * The op-call site saves the previous value, sets this to its own node,
     * runs the clause, then restores.
     *
     * Trap: the skip rule makes a stale value here MASK a live handler —
     * the walk then reports the effect as unhandled even though the node is
     * on the chain with a valid handler. A non-local exit out of a clause
     * (a longjmp past the restore) leaves exactly that state. */
    KaiEvidence    *in_dispatch_node;
    /* R4 fix — back-pointer to the KaiValue wrapper that owns this
     * fiber struct. Set in kai_fiber_value when the wrapper is
     * allocated; the scheduler holds an incref on the wrapper from
     * spawn-enqueue until the trampoline's DONE/CANCELLED tail, which
     * `kai_decref`s `value`. Pairing the scheduler-side ref with the
     * caller-side ref makes `let _ = fiber_spawn(…)` (discarding the
     * Fiber value) safe: the wrapper stays alive while the struct is
     * still referenced from the run queue. NULL on a worker's root fiber,
     * which has no wrapper (it represents the OS thread). */
    KaiValue       *value;
    /* Tier 2 — trap-exit semantics. When 0 (default), a linked peer's
     * termination sets cancel_requested on this fiber (the v1 uniform
     * propagation). When 1, the propagation walk pushes a String into
     * this fiber's mailbox instead — "Normal" if the peer terminated
     * via DONE, "Crashed" if via CANCELLED — and leaves
     * cancel_requested untouched. Toggled by Spawn.set_trap_exit;
     * spec: docs/actors.md §*Supervision: links and monitors* /
     * *Trap-exit semantics*. The flag is per-fiber, not per-link, so
     * it must be set before the link that should respect it; later
     * toggles affect future propagations only. */
    int             trap_exit;
    /* Innermost open mailbox this fiber owns; each one links to the
     * mailbox it shadows through `outer`, and closing one unlinks it.
     * Link/monitor deliveries to this fiber land here. */
    struct KaiMailbox *mailbox;
    /* Tier 2 — intrusive list of fibers monitoring this one. Each
     * Monitor.monitor(target_pid) call from an observer fiber
     * appends a node here on the *target* fiber. At trampoline
     * termination (DONE or CANCELLED) the walker pops every entry
     * and pushes the original target_pid value into the observer's
     * mailbox, leaving the observer's cancel_requested untouched
     * (monitors do not propagate faults — `docs/actors.md` §*Fault
     * propagation*). Owned by the target fiber; nodes are freed in
     * the propagation walk and as a safety net in
     * kai_free_value's KAI_FIBER branch. */
    KaiMonitorNode *monitor_head;
    /* Issue #611 — Phase R1 reactor metadata. A parked fiber lives
     * on exactly one reactor list at a time (timer wheel, pid waiter
     * map, or file-pool waiter list), so a single intrusive link slot
     * is sufficient. The companion data members carry the wakeup
     * payload that the parking op needs to read on resume:
     *   reactor_deadline_ns — monotonic deadline for timer-wheel parks.
     *   reactor_wait_pid    — pid the fiber is waiting on (Process.wait).
     *   reactor_wait_status — waitpid status filled by the SIGCHLD drain.
     *   reactor_data        — generic pointer slot used by the file
     *                         thread-pool offload to publish the
     *                         completed `KaiValue *` result before
     *                         waking the parked fiber.
     * The slots are read once on resume; nothing else in the runtime
     * touches them. Zero is a valid "not parked here" sentinel for
     * all four. */
    KaiFiber       *reactor_next;
    uint64_t        reactor_deadline_ns;
    int             reactor_wait_pid;
    int             reactor_wait_status;
    void           *reactor_data;
    /* Issue #959 — structured-concurrency scope. `nursery_top` is the
     * innermost open nursery on THIS fiber (a per-fiber stack so a
     * spawned child opening its own nursery does not collide with the
     * parent's; nesting composes Trio-style). `Spawn.spawn` registers
     * the new child on the spawner's `nursery_top` children list
     * via `scope_sibling_next`. `nursery_exit` joins every child on
     * that list before returning, cancelling the rest and re-raising
     * on the first child that terminated CANCELLED. NULL when no
     * nursery is open (bare spawns then have no scope to join). */
    struct KaiNursery *nursery_top;
    KaiFiber          *scope_sibling_next;
    /* The nursery this fiber was spawned into (NULL for bare spawns).
     * Read by the trampoline's cancel branch to cancel siblings
     * eagerly; valid there because the owner cannot free the scope
     * until it has joined this fiber, which requires the terminal
     * state store that follows the walk. */
    struct KaiNursery *scope_nursery;
    /* M:N scheduler — the OS scheduler thread that owns this fiber's
     * heap and runs it. A fiber runs on exactly one thread at a time;
     * `home_thread` is where it was spawned and where it parks. A
     * cross-thread `Actor.send` reads it (lock-free) to decide copy vs
     * pointer transfer, and a thief writes it (lock-free) when it steals
     * the fiber — so it is atomic: the store publishes the new owner and
     * every reader acquire-loads it, matching the `live`-slot discipline.
     * Under N=1 every fiber's home_thread is 0, the copy branch is never
     * taken, and an _Atomic int is a plain aligned int on our targets. */
    _Atomic int home_thread;
    /* M:N — when set, this fiber may only ever run on thread 0, the OS
     * thread `main` entered on. Thread-affine C libraries (AppKit, and
     * through it GLFW/SDL/GTK) trap if initialised or pumped off the
     * process main thread, so the entry fiber carries the pin and every
     * other fiber keeps migrating freely. Read by the thief (skip) and by
     * enqueue (route to thread 0 instead of the caller's slot); never
     * written after spawn. */
    int pinned_main;
    /* M:N — a cross-thread wake that arrives while this fiber is still
     * RUNNING (it enqueued its recv-waiter but has not yet reached the
     * park swap) is recorded here instead of lost. The park path checks
     * it after marking PARKED and re-readies itself if non-zero, closing
     * the enqueue-waiter / park lost-wakeup window. Touched only under
     * the owner slot lock. */
    int wake_pending;
    /* F2 — dedicated reactor thread. A reactor park (sleep, socket, pid,
     * stdin, signal, file-pool) stamps the reason here on the fiber's own
     * stack, then yields to the scheduler root WITHOUT touching any shared
     * reactor structure. The root, running on the worker's root fiber (never the
     * parked fiber's stack), links the fiber into the wheel/waiter list
     * from `kai_sched_commit_park` — so no thief or reactor drain can
     * observe the fiber on a reactor list until its exit swap has finished
     * writing its ctx. Zero (KAI_PARK_NONE, calloc-cleared) means "not a
     * reactor park"; the mailbox/await park path leaves it zero and takes
     * the wake_pending discipline instead. Reactor-only; unused at N=1. */
    int pending_park;
    /* Set (under kai_reactor_mu) by whichever reactor drain or detach
     * actually spliced this fiber out of a reactor structure; cleared by
     * kai_sched_park before every park. On resume it distinguishes a wake
     * the reactor delivered from a spurious one (a stale unpark aimed at an
     * earlier park, or a consumed wake_pending permit): a reactor park site
     * must re-park when it resumes with this still 0 and its own predicate
     * unmet, never conclude its deadline or event fired. */
    int reactor_fired;
    /* Nonzero while the fiber waits on a mailbox waiter chain, where a
     * cancel must wake it: no reactor event ever will. The fiber sets it
     * and then reads cancel_requested; the canceller sets cancel_requested
     * and then reads it. One of the two always sees the other, so the
     * cancel is never slept through. */
    _Atomic int cancel_wake;
    /* F2 — per-thread pending-commit stack link. `kai_sched_park` pushes a
     * reactor-parking fiber here before swapping to the root; the root
     * drains the stack post-swap and commits each. A stack (not a single
     * slot) because two parks can interpose before the root drains. */
    KaiFiber *commit_next;
    /* ThreadSanitizer's per-fiber bookkeeping handle, or NULL outside a TSAN
     * build. TSAN tracks a shadow stack and a sync clock per OS thread and
     * cannot see a ucontext switch, so without this every fiber's function
     * entries and exits land on whichever thread happens to run it; a fiber
     * that migrates pushes on one thread and pops on another, and the
     * imbalance eventually walks a shadow stack off its fixed-size mapping.
     * See KAI_TSAN_SWITCH_TO. */
    void *tsan_fiber;
    /* Created when a mailbox is first stamped with this fiber as owner;
     * see KaiFiberExit. */
    _Atomic(KaiFiberExit *) exit_rec;
    /* Stack of KaiUnwEntry runs. Every landing pad records its height and
     * a non-local exit releases what lies above the pad it jumps to. */
    KaiUnwEntry    *unw_buf;
    uint32_t        unw_top;
    uint32_t        unw_cap;
    /* The trampoline's frame address: a trap's unwind stops there. */
    uintptr_t       unwind_frame;
    /* Pending clause tails, in push order. A handle's own may sit below
     * those of the handles around it, so removal is by node, not by height. */
    KaiResumeFrame *rframe_buf;
    uint32_t        rframe_top;
    uint32_t        rframe_cap;
    /* The stack segment running on this fiber, NULL on the fiber's own stack. */
    KaiSegment     *seg;
    /* Nonzero while the fiber must not change OS thread. */
    int             seg_pin;
    /* The worker running this fiber, stored by the dispatcher on every
     * resume. Code that crossed a switch reaches scheduler state through
     * it: a worker pointer held from before the switch may name the thread
     * the fiber left. */
    struct KaiWorker *worker;
    /* Cells pending reclamation in this fiber's free drain, allocated on
     * its first drain. */
    struct KaiFreeStack *free_stack;
    /* The constants of a lazy type this fiber has read, each evaluated
     * once and kept until the fiber ends. */
    KaiConstCell *consts;
    uint32_t      const_n;
    uint32_t      const_cap;
};

/* The per-context half of a fiber's state. A segment switch exchanges it, so
 * a segment keeps its own evidence above its base, unwind stack, pending
 * clause tails and stack bounds, and the resumer's LIFO disciplines never
 * interleave with the segment's. */
typedef struct {
    KaiEvidence    *evidence_top;
    KaiEvidence    *in_dispatch_node;
    KaiUnwEntry    *unw_buf;
    uint32_t        unw_top;
    uint32_t        unw_cap;
    KaiResumeFrame *rframe_buf;
    uint32_t        rframe_top;
    uint32_t        rframe_cap;
    void           *stack_base;
    size_t          stack_size;
    uintptr_t       unwind_frame;
    void           *tsan_fiber;
    KaiSegment     *seg;
} KaiSegCtx;

typedef enum {
    KAI_SEG_RUNNING   = 0,
    KAI_SEG_SUSPENDED = 1,
    KAI_SEG_RETURNED  = 2,
    KAI_SEG_UNWOUND   = 3,  /* discontinued, unwound to its entry */
    KAI_SEG_ESCAPED   = 4,  /* a non-local exit left through its entry */
    KAI_SEG_FORWARD   = 5   /* a request for a handle below the resumer */
} KaiSegState;

struct KaiSegment {
    void        *sp;         /* the segment's stack pointer while switched out */
    void        *outer_sp;   /* the resumer's, while the segment runs */
    KaiSegCtx    own;        /* the segment's context while switched out */
    KaiSegCtx    outer;      /* the resumer's, while the segment runs */
    KaiEvidence *base;       /* the node the segment's evidence chain hangs from */
    KaiFiber    *fiber;
    KaiValue    *body;
    KaiValue    *xfer;       /* the value a suspend, return or resume hands over */
    KaiSegState  state;
    int          discontinue;
    int          trap;       /* an escape continues as a trap */
    KaiEvidence *escape_to;  /* the handle an escape lands on; NULL: the cancel pad */
    jmp_buf      pad;        /* in the segment's entry frame; _setjmp: no signal mask */
    KaiValue    *spare;      /* a spent box kept for the next suspend */
    void        *tsan_outer;
    const void  *asan_outer_bottom;
    size_t       asan_outer_size;
    KaiSegment  *pool_next;
    struct KaiSegRelay *relays;  /* NULL label: a spare */
};

/* Leave the running segment by a non-local exit; its resumer continues it. */
KAI_SCHED_FN void kai_seg_escape(KaiFiber *f, KaiEvidence *to, int trap) __attribute__((noreturn));
/* The free arm of a KAI_CONT box. */
KAI_SCHED_FN void kai_seg_cont_drop(KaiValue *box);
/* Unmap this thread's pooled segments; a worker runs it before it exits. */
KAI_SCHED_FN __attribute__((noinline)) void kai_seg_pool_drain(void);

/* Issue #959 — one open structured-concurrency scope. Children spawned
 * while this scope is the active fiber's `nursery_top` are pushed on
 * `children_head` (intrusive via the child's `scope_sibling_next`).
 * `parent` chains to the enclosing nursery so a nested `nursery { }`
 * restores it on exit. The scope holds one extra ref on each child's
 * wrapper (taken at registration, released at join) so a discarded
 * `Fiber[T]` handle cannot free the struct before the scope joins it.
 *
 * `children_head` is atomic because a failing child walks the list
 * from its own thread (eager cancel-on-fail) while the owner may
 * still be pushing spawns; the owner's store publishes the new
 * node's `scope_sibling_next` to that walker.
 *
 * `failed_child` latches the first child that terminated CANCELLED
 * without a requested cancellation. The latch elects a single eager
 * walker when two children fail at once, and it is what `scope_exit`
 * trusts for failure: the eager walk sets `cancel_requested` on
 * peers, so a concurrently-failing peer can terminate flagged and
 * would otherwise read as an expected cancellation, swallowing the
 * failure. */
struct KaiNursery {
    _Atomic(KaiFiber *) children_head;
    KaiNursery         *parent;
    _Atomic(KaiFiber *) failed_child;
    KaiEvidence        *ev_top;
};

/* A worker's root fiber starts as the OS-thread context, representing the
 * dispatch loop, and running; every other field starts zero. Its ctx is
 * filled lazily on first yield (getcontext at the moment we suspend the
 * dispatch loop into a fiber). */
#define KAI_MAIN_FIBER_INIT { .state = KAI_FIBER_RUNNING }
/* The landing a fiber context falls off into runs only setcontext, so its
 * stack is small. A literal, not SIGSTKSZ: glibc defines that as a sysconf()
 * call, which would make the array below a VLA. */
#define KAI_UC_LINK_STACK_SIZE 65536

/* A free reclaims a cell's children by recursion up to KAI_FREE_DEPTH
 * levels, as deep as a shallow structure ever needs, and below that drains
 * the rest through this stack of pending cells instead of recursing: a
 * structure of any depth takes bounded C stack. Never a local array: a free
 * runs at the deepest point of whatever dropped the last reference, often on
 * a 64 KiB fiber stack, and must not add to it. Past the inline buffer the
 * pending cells spill to the heap until the drain finishes.
 *
 * The stack belongs to the draining fiber, not the thread: dropping a
 * continuation discontinues its segment, which switches context, and the
 * fiber can come back on another thread with the drain half done. A root
 * fiber uses its worker's; any other allocates one on its first drain. */
#define KAI_FREE_DEPTH 16
#define KAI_FREE_STACK_LOCAL 64
typedef struct KaiFreeStack {
    KaiValue **items;
    int n, cap, active;
    KaiValue *local[KAI_FREE_STACK_LOCAL];
} KaiFreeStack;

/* One OS scheduler thread's own state: the fiber it is running, its root
 * fiber (the dispatch loop's context), the N=1 ready queue, and the stacks
 * the root drains after each switch. Only its own thread touches it.
 *
 * TRAP: a worker pointer is valid only until the next switch. A fiber that
 * parks on one worker can be resumed by another, so code after a switch
 * reads `fiber->worker`, never a worker pointer it held from before. The
 * root's own pointer is the exception: a root never changes thread. */
typedef struct KaiWorker {
    KaiFiber   *active;
    int         id;
    int         parked_count;   /* N=1 deadlock detection */
    KaiFiber   *ready_head;     /* N=1 ready queue; at N>1 it is the slot's */
    KaiFiber   *ready_tail;
    KaiFiber   *commit_stack_head;
    KaiFiber   *requeue_stack_head;
    KaiFiber   *pending_free;
    KaiValue   *pending_sched_drop;
    void       *sigalt_stack;
    int         sigalt_ready;
    int         uc_link_ready;
    KaiFiber    main_fiber;
    KaiFreeStack free_stack;    /* the root fiber's */
#if KAI_HOST_FIBERS
    ucontext_t  uc_link_ctx;
    char        uc_link_stack[KAI_UC_LINK_STACK_SIZE];
#endif
} KaiWorker;

/* The scheduler's only thread-local. Read nowhere but kai_worker_here. */
#if defined(KAI_SEPARATE_COMPILATION)
extern KAI_TLS KaiWorker kai_worker;
#  if defined(KAI_RUNTIME_OWNER)
KAI_TLS KaiWorker kai_worker = { .main_fiber = KAI_MAIN_FIBER_INIT };
#  endif
#else
static KAI_TLS KaiWorker kai_worker = { .main_fiber = KAI_MAIN_FIBER_INIT };
#endif

/* This thread's worker, bound on the thread's first call. noinline and
 * defined in the owner only: each call re-reads the thread pointer, so no
 * caller can cache a thread-local address across a switch. */
KAI_SCHED_FN __attribute__((noinline)) KaiWorker *kai_worker_here(void)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    KaiWorker *w = &kai_worker;
    if (__builtin_expect(w->active == NULL, 0)) {
        w->main_fiber.worker = w;
        w->main_fiber.free_stack = &w->free_stack;
        w->active = &w->main_fiber;
    }
    return w;
}
#endif

/* ---------- ThreadSanitizer fiber annotations ----------
 *
 * TSAN keys its shadow stack and sync clocks to the OS thread. A ucontext
 * switch is invisible to it, so an unannotated fiber runtime feeds one
 * thread's bookkeeping with another fiber's entries: under work stealing a
 * fiber pushes frames on the thread that started it and pops them on the
 * thread that resumed it, and the drift eventually walks a shadow stack off
 * its mapping — a store fault inside the sanitizer, which the process then
 * spins on rather than dying. The annotations below give
 * each fiber (and each thread's scheduler root) its own TSAN state, which is
 * what makes race reports meaningful under M:N at all.
 *
 * Every swapcontext/setcontext in the scheduler is immediately preceded by
 * KAI_TSAN_SWITCH_TO / KAI_TSAN_SWITCH_TO_ROOT. Outside a TSAN build all of
 * this compiles away.
 *
 * The switch itself is a macro, not a function, and that is load-bearing.
 * TSAN pushes a frame record on entry to every instrumented function and pops
 * one on return; a helper that changed fiber identity in its middle would push
 * onto the outgoing fiber's shadow stack and pop from the incoming one,
 * leaking a record per switch until a shadow stack ran off the end of its
 * mapping — the very failure these annotations exist to prevent. Expanded at
 * the call site, the identity change happens inside a frame that enters and
 * leaves on one fiber, so the bookkeeping stays balanced. The two out-of-line
 * accessors below run entirely on the outgoing fiber, before the switch. */
#if defined(__SANITIZE_THREAD__)
#  define KAI_TSAN_FIBERS 1
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define KAI_TSAN_FIBERS 1
#  endif
#endif

#if defined(KAI_TSAN_FIBERS)
void *__tsan_get_current_fiber(void);
void *__tsan_create_fiber(unsigned flags);
void  __tsan_destroy_fiber(void *fiber);
void  __tsan_switch_to_fiber(void *fiber, unsigned flags);

/* This thread's scheduler root adopts the thread's own TSAN state rather than
 * a fresh one, so a root never looks like a fiber. It must be captured while
 * the thread is still running AS the root — `__tsan_get_current_fiber()` from
 * inside a fiber would alias the root's handle onto that fiber's state, and
 * every later "switch back to root" would then land on a fiber. Hence the
 * eager bind at the top of each scheduler loop rather than a lazy one at the
 * first switch. */
static void kai_tsan_bind_root(KaiWorker *w) {
    if (!w->main_fiber.tsan_fiber) {
        w->main_fiber.tsan_fiber = __tsan_get_current_fiber();
    }
}

/* A fiber's own state, created on first dispatch. */
__attribute__((noinline))
static void *kai_tsan_fiber_of(KaiFiber *f) {
    if (!f->tsan_fiber) f->tsan_fiber = __tsan_create_fiber(0);
    return f->tsan_fiber;
}

#define KAI_TSAN_BIND_ROOT(w)      kai_tsan_bind_root(w)
#define KAI_TSAN_SWITCH_TO(f)      __tsan_switch_to_fiber(kai_tsan_fiber_of(f), 0)
#define KAI_TSAN_SWITCH_TO_ROOT(w) __tsan_switch_to_fiber((w)->main_fiber.tsan_fiber, 0)

/* Release a finished fiber's TSAN state. Called from the deferred-free drain,
 * which runs on another context, so the state being dropped is never current. */
static void kai_tsan_fiber_free(KaiFiber *f) {
    if (f->tsan_fiber) {
        __tsan_destroy_fiber(f->tsan_fiber);
        f->tsan_fiber = NULL;
    }
}
#else
#define KAI_TSAN_BIND_ROOT(w)      ((void) (w))
#define KAI_TSAN_SWITCH_TO(f)      ((void) 0)
#define KAI_TSAN_SWITCH_TO_ROOT(w) ((void) (w))
static inline void kai_tsan_fiber_free(KaiFiber *f) { (void) f; }
#endif

/* AddressSanitizer learns about a stack-segment switch from these; without
 * them it misreads the stack bounds on a no-return path inside a segment. */
#if defined(__SANITIZE_ADDRESS__)
#  define KAI_ASAN_FIBERS 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define KAI_ASAN_FIBERS 1
#  endif
#endif
#if defined(KAI_ASAN_FIBERS)
void __sanitizer_start_switch_fiber(void **fake_stack_save, const void *bottom, size_t size);
void __sanitizer_finish_switch_fiber(void *fake_stack_save, const void **bottom_old, size_t *size_old);
#endif
/* LeakSanitizer scans thread stacks, not a mapped segment stack: a value only
 * a suspended segment's frames hold would read as leaked. */
#if defined(KAI_ASAN_FIBERS) && defined(__linux__)
void __lsan_register_root_region(const void *p, size_t size);
void __lsan_unregister_root_region(const void *p, size_t size);
#  define KAI_SEG_LSAN_ROOT(seg)   __lsan_register_root_region((seg)->own.stack_base, (seg)->own.stack_size + kai_page_size())
#  define KAI_SEG_LSAN_UNROOT(seg) __lsan_unregister_root_region((seg)->own.stack_base, (seg)->own.stack_size + kai_page_size())
#else
#  define KAI_SEG_LSAN_ROOT(seg)   ((void) 0)
#  define KAI_SEG_LSAN_UNROOT(seg) ((void) 0)
#endif

static inline KaiFiber *kai_current_fiber(void) {
    return kai_worker_here()->active;
}

/* Make `f` the fiber `w` runs. Every dispatch goes through here, so a
 * running fiber's `worker` always names the thread it runs on. */
static inline void kai_worker_run(KaiWorker *w, KaiFiber *f) {
    f->worker = w;
    w->active = f;
}

KAI_RT_SHARED void kai_evidence_unwind_all(void);

/* A trap leaves through the unwinder rather than a bare longjmp, so the
 * landing pads of the frames it crosses release their references. The
 * stop function lands once the walk reaches `frame` (the fiber's
 * trampoline or the test harness) or the end of the walkable stack; when
 * the walk cannot start or stops early, or the stack is too short for the
 * unwinder, `land` runs at once — exactly the longjmp it replaces. The
 * exception object lives on the heap: a landing pad runs on the frames the
 * raising call sat below, and `resume` reads the object back. */
#if KAI_HOST_UNWIND
typedef struct {
    struct _Unwind_Exception ex;
    uintptr_t                frame;
    void                   (*land)(void *);
    void                    *arg;
} KaiTrapUnwind;

#define KAI_TRAP_EXCEPTION_CLASS 0x4b41490054524150ULL /* "KAI\0TRAP" */
#define KAI_TRAP_UNWIND_ROOM     (16 * 1024)

KAI_RT_SHARED _Unwind_Reason_Code kai_trap_stop(int version, _Unwind_Action actions,
                                         _Unwind_Exception_Class cls,
                                         struct _Unwind_Exception *ex,
                                         struct _Unwind_Context *ctx, void *arg) {
    (void) version; (void) cls; (void) ex;
    KaiTrapUnwind *u = (KaiTrapUnwind *) arg;
    if ((actions & _UA_END_OF_STACK) || (uintptr_t) _Unwind_GetCFA(ctx) > u->frame) {
        void (*land)(void *) = u->land;
        void *land_arg = u->arg;
        free(u);
        land(land_arg);
    }
    return _URC_NO_REASON;
}
#endif

static size_t kai_page_size(void);

#if KAI_HOST_UNWIND
/* Room left below this frame on a fiber's mapped stack; the OS stack
 * (`base` NULL) is taken to have enough. */
static int kai_trap_unwind_room(void *base) {
    if (!base) return 1;
    uintptr_t here = (uintptr_t) __builtin_frame_address(0);
    uintptr_t low  = (uintptr_t) base + kai_page_size();
    return here > low && here - low > KAI_TRAP_UNWIND_ROOM;
}
#endif

__attribute__((noreturn, noinline))
KAI_RT_SHARED void kai_trap_unwind(uintptr_t frame, void *stack_base, void (*land)(void *), void *arg) {
#if KAI_HOST_UNWIND
    KaiTrapUnwind *u = (frame && kai_trap_unwind_room(stack_base))
                           ? (KaiTrapUnwind *) calloc(1, sizeof *u) : NULL;
    if (u) {
        u->ex.exception_class = KAI_TRAP_EXCEPTION_CLASS;
        u->frame = frame;
        u->land  = land;
        u->arg   = arg;
        _Unwind_ForcedUnwind(&u->ex, kai_trap_stop, u);
        free(u);
    }
#else
    (void) frame; (void) stack_base;
#endif
    land(arg);
    __builtin_unreachable();
}

/* Every jump to the fiber's cancel pad goes through here: inside a segment
 * it first leaves the segment, and the resumer continues the same exit. */
__attribute__((noreturn))
KAI_RT_SHARED void kai_fiber_pad_jump(KaiFiber *f, int trap) {
    if (f->seg != NULL) kai_seg_escape(f, NULL, trap);
    longjmp(f->cancel_pad, 1);
}

KAI_RT_SHARED void kai_trap_land_fiber(void *arg) {
    kai_fiber_pad_jump((KaiFiber *) arg, 1);
}

/* A recoverable runtime trap (index out of range, divide by zero,
 * non-exhaustive match). With a fiber pad installed, unwind to it
 * like Cancel — the trampoline reports the fiber TRAPPED and a
 * supervisor contains the fault. With no pad (main_fiber, or before
 * runtime init) terminate the process, preserving the pre-existing
 * top-level behaviour. `msg` must be a static string. Out of line: the
 * unwind runs `finally` clauses that can switch context, and a caller
 * inlining it would hold its own frame across that switch. */
__attribute__((noinline, noreturn))
KAI_RT_SHARED void kai_trap_abort(const char *msg) {
    KaiFiber *f = kai_current_fiber();
    if (f && f->cancel_pad_set) {
        f->trapped  = 1;
        f->trap_msg = msg;
        /* Before the walk: once a landing pad has run, the frames below it
         * are dead and the unwinder's own calls reuse their stack, which
         * the unwind entries point into. */
        kai_evidence_unwind_all();
        kai_trap_unwind(f->unwind_frame, f->stack_base, kai_trap_land_fiber, f);
    }
    kai_eprintf("kai: trap: %s\n", msg ? msg : "runtime trap");
    kai_exit(1);
}


