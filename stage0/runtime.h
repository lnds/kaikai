/*
 * kaikai-minimal runtime for stage 0.
 *
 * Header-only. Generated C files #include this and compile together in
 * one translation unit, so static storage and linkage are fine.
 *
 * All values are heap-allocated KaiValues with a reference-count header
 * and a discriminating tag. Uniform boxing per docs/stage0-design.md;
 * primitives (Int, Real, Bool, Char, Unit) are wrapped in the same
 * struct so the generated code does not branch on shape.
 *
 * Calling convention for functions emitted by stage 0:
 *   static KaiValue *kai_<name>(KaiValue *arg0, KaiValue *arg1, ...);
 * Arguments are handed to the callee as owned references (callee must
 * decref them or keep them alive as part of the returned value). The
 * return value is owned by the caller.
 *
 * Closures and higher-order prelude helpers (map/filter/reduce/each)
 * go through a dynamic dispatch path `kai_apply(closure, argc, argv)`
 * to keep the generated code uniform.
 */

/* m8.x cooperative scheduler substrate: ucontext.
 *
 * The _XOPEN_SOURCE feature-test macro must be defined BEFORE ANY
 * system header is included, otherwise sys/types.h (transitively
 * pulled in by stdio.h, time.h, etc.) freezes the legacy POSIX
 * ucontext_t layout (~56 bytes) instead of the full XSI shape
 * (~880 bytes on darwin arm64). swapcontext then writes 880 bytes
 * into a 56-byte buffer, silently corrupting whatever sits next to
 * the embedded ucontext_t — exactly what bit Phase 2 once
 * (kai_main_fiber.evidence_top got clobbered to a saved register
 * value because the static evidence nodes were laid out adjacent).
 * Spec: docs/fibers-impl.md §*macOS deprecation handling*. */
#define _XOPEN_SOURCE 600
/* mmap(MAP_ANON) is a BSD extension hidden by strict _XOPEN_SOURCE.
 * Re-expose it: _DARWIN_C_SOURCE on macOS, _DEFAULT_SOURCE on glibc.
 * The fiber stack allocator (m8.x guard pages) needs anonymous mmap. */
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE 1
#endif
#if defined(__linux__)
#  define _DEFAULT_SOURCE 1
#endif

#ifndef KAI_RUNTIME_H
#define KAI_RUNTIME_H

#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* net-tcp-v1 — sockets API for the NetTcp default handler.
 * POSIX everywhere we ship: macOS, Linux, *BSD. The handler is
 * blocking-only in v1 (the inline-eager scheduler can't suspend a
 * fiber on a readiness event yet); kqueue / epoll integration lands
 * with m8.x alongside the rest of the cooperative scheduler. */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

/* Apple deprecated ucontext in macOS 10.6 (POSIX-2008 obsoletion).
 * The functions still work; the deprecation attribute on the
 * prototypes triggers -Wdeprecated-declarations, which the local
 * pragma silences. Same trick libco / libtask / Boost.Context use.
 * The same envelope wraps every swap/get/makecontext call site. */
#if defined(__APPLE__) || defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <ucontext.h>
#if defined(__APPLE__) || defined(__clang__)
#  pragma clang diagnostic pop
#endif

/* Not every program uses every prelude function; silence the
   unused-function warnings that would otherwise pile up in `cc` output
   when stage 0 links only the parts a given program needs. */
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#endif

/* ---------- types ---------- */

typedef enum {
    KAI_UNIT,
    KAI_BOOL,
    KAI_INT,
    KAI_REAL,
    KAI_CHAR,
    KAI_STR,
    KAI_NIL,
    KAI_CONS,
    KAI_RECORD,
    KAI_VARIANT,
    KAI_CLOSURE,
    KAI_ARRAY,
    KAI_FIBER,      /* m8 #3: Spawn / Fiber[T] handle (opaque) */
    KAI_PID         /* m8 #7: Actor[Msg] / Pid[Msg] handle (opaque) */
} KaiTag;

typedef struct KaiValue KaiValue;

/* Dynamic-dispatch signature used for closures and higher-order calls. */
typedef KaiValue *(*KaiFn)(KaiValue *self, KaiValue **args, int n_args);

struct KaiValue {
    int32_t rc;
    int32_t tag;
    union {
        int      b;                                 /* KAI_BOOL */
        int64_t  i;                                 /* KAI_INT */
        double   r;                                 /* KAI_REAL */
        uint32_t c;                                 /* KAI_CHAR */
        struct { size_t len; char *bytes; } s;      /* KAI_STR (heap, not NUL-terminated but we always allocate +1 byte for safety) */
        struct { KaiValue *head; KaiValue *tail; } cons;
        struct {
            int          n_fields;
            KaiValue   **fields;
            const char **names;                     /* static strings, not freed */
        } rec;
        struct {
            int32_t     variant_tag;                /* index into the sum type */
            const char *variant_name;               /* static string */
            int32_t     n_args;
            KaiValue  **args;
        } var;
        struct {
            KaiFn       fn;
            int32_t     arity;
            int32_t     n_captures;
            KaiValue  **captures;
        } clo;
        /* Opaque mutable array. Used by the stage 2 inferencer to keep
           the HM substitution indexed by TyVar id, so apply_ty lookups
           are O(1) instead of O(k) over an association list. Set
           mutates in place and returns the same value (Perceus unaware
           — callers must not alias an array across logical versions). */
        struct {
            int64_t     len;
            int64_t     cap;
            KaiValue  **items;
        } arr;
        /* m8 #3: opaque handle to a KaiFiber. The KaiFiber struct
         * itself is heap-allocated by Spawn.spawn and owned by this
         * value: when the value's RC drops to zero, kai_free_value
         * frees the KaiFiber and decrefs its result + thunk. */
        struct KaiFiber *fib;
        /* m8 #7: opaque handle to a KaiMailbox. Pid values share the
         * mailbox's lifetime through borrowed pointers — the mailbox
         * is owned by the with_mailbox / spawn_actor helper that
         * allocated it, not by the Pid value. RC on the Pid value
         * is just a handle count; freeing a Pid does not free the
         * mailbox (that happens when the with_mailbox / spawn_actor
         * scope exits). */
        struct KaiMailbox *mb;
    } as;
};

/* ---------- allocation and refcounting ---------- */

/* Forward declarations used across sections. */
static int       kai_op_truthy(KaiValue *v);

/* Refcount tracing (m5 #0): always-compiled counters; the per-process
   report at exit is gated on the env var KAI_TRACE_RC. The counters
   add 4 increments per kai_alloc and 2 per kai_free_value — cheap
   enough that the always-on path costs ~ns per allocation, but small
   enough that we keep them on rather than ifdef'ing them in/out
   (otherwise a measurement run would need a runtime rebuild). */
static int64_t kai_rc_alloc_total = 0;
static int64_t kai_rc_free_total  = 0;
static int64_t kai_rc_live_now    = 0;
static int64_t kai_rc_live_peak   = 0;
static int64_t kai_rc_alloc_by_tag[16] = {0};
/* issue #118 — Perceus reuse-in-place counter. Bumped by every
 * successful in-place rewrite in kai_reuse_or_alloc_* (further down). */
static int64_t kai_rc_reuse_total = 0;

static const char *kai_rc_tag_name(int t) {
    switch (t) {
        case KAI_UNIT:    return "unit";
        case KAI_BOOL:    return "bool";
        case KAI_INT:     return "int";
        case KAI_REAL:    return "real";
        case KAI_CHAR:    return "char";
        case KAI_STR:     return "str";
        case KAI_NIL:     return "nil";
        case KAI_CONS:    return "cons";
        case KAI_RECORD:  return "record";
        case KAI_VARIANT: return "variant";
        case KAI_CLOSURE: return "closure";
        case KAI_ARRAY:   return "array";
        default:          return "?";
    }
}

static void kai_rc_report(void) {
    if (!getenv("KAI_TRACE_RC")) return;
    int64_t leaked = kai_rc_alloc_total - kai_rc_free_total;
    fprintf(stderr,
        "[KAI_TRACE_RC] alloc_total=%lld free_total=%lld leaked=%lld live_peak=%lld\n",
        (long long) kai_rc_alloc_total,
        (long long) kai_rc_free_total,
        (long long) leaked,
        (long long) kai_rc_live_peak);
    for (int i = 0; i < 16; i++) {
        if (kai_rc_alloc_by_tag[i] > 0) {
            fprintf(stderr, "[KAI_TRACE_RC]   tag %-7s allocs=%lld\n",
                    kai_rc_tag_name(i),
                    (long long) kai_rc_alloc_by_tag[i]);
        }
    }
    /* issue #118 — Perceus reuse-in-place counter. */
    if (kai_rc_reuse_total > 0) {
        fprintf(stderr, "[KAI_TRACE_RC]   reuse_in_place=%lld\n",
                (long long) kai_rc_reuse_total);
    }
}

static int kai_rc_registered = 0;
static void kai_rc_register_once(void) {
    if (kai_rc_registered) return;
    kai_rc_registered = 1;
    atexit(kai_rc_report);
}

static KaiValue *kai_alloc(KaiTag tag) {
    KaiValue *v = (KaiValue *) calloc(1, sizeof(KaiValue));
    if (!v) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    v->rc = 1;
    v->tag = (int32_t) tag;
    /* trace */
    kai_rc_alloc_total++;
    kai_rc_live_now++;
    if (kai_rc_live_now > kai_rc_live_peak) kai_rc_live_peak = kai_rc_live_now;
    if ((int) tag >= 0 && (int) tag < 16) kai_rc_alloc_by_tag[(int) tag]++;
    return v;
}

/* m5 #7: constant pool for nullary primitives.
 *
 * `kai_unit()`, `kai_bool(true)`, `kai_bool(false)`, `kai_nil()` are
 * the four nullary constructors that dominated the per-tag alloc
 * breakdown on kaic2 self-compile (~78% combined). Returning shared
 * static singletons collapses every call to those factories into a
 * pointer to .data, eliminating the calloc and the four
 * `kai_rc_alloc_total` / `kai_rc_alloc_by_tag[]` increments per call.
 *
 * Singletons carry `rc = INT32_MAX` as a saturation sentinel.
 * `kai_incref` / `kai_decref` short-circuit when they see the
 * sentinel, so RC bookkeeping skips the singletons entirely:
 *   - incref leaves rc as-is (no overflow toward zero).
 *   - decref never triggers `kai_free_value` on a static, which
 *     would otherwise call `free()` on .data and crash.
 * The sentinel costs one extra `int` compare in the hot RC path;
 * the saved alloc/free traffic dominates by orders of magnitude.
 *
 * Selfhost stays byte-identical because the emitted text is
 * unchanged — only the runtime semantics shift.
 */
static KaiValue kai_singleton_unit  = { INT32_MAX, KAI_UNIT, { .b = 0 } };
static KaiValue kai_singleton_true  = { INT32_MAX, KAI_BOOL, { .b = 1 } };
static KaiValue kai_singleton_false = { INT32_MAX, KAI_BOOL, { .b = 0 } };
static KaiValue kai_singleton_nil   = { INT32_MAX, KAI_NIL,  { .b = 0 } };

static KaiValue *kai_incref(KaiValue *v) {
    if (v && v->rc != INT32_MAX) v->rc++;
    return v;
}
static void       kai_decref(KaiValue *v);

/* m8 #1/#3: KaiFiber definitions sit here (before kai_free_value)
 * because KAI_FIBER values own their KaiFiber struct and the free
 * path needs the full layout. The handler-stack runtime
 * (KaiEvidence + push/pop/lookup) lives further down; KaiFiber only
 * holds a `KaiEvidence *`, so a forward declaration is enough. The
 * Spawn default handlers (kai_default_spawn_*) come later in the
 * file too — they reach the struct through this declaration and use
 * kai_apply (defined further down) to invoke spawned thunks. */
typedef struct KaiEvidence KaiEvidence;

/* Issue #104 — forward decls: walk / clone heap evidence chains
 * (allocated in kai_default_spawn_spawn). The full KaiEvidence
 * struct sits further down, but kai_free_value (which holds the
 * KAI_FIBER cleanup) and kai_default_spawn_spawn are defined
 * above the struct, so these helpers let those paths drive the
 * walk without touching struct members before they're declared. */
static void         kai_free_cloned_evidence_chain(KaiEvidence *head);
static KaiEvidence *kai_clone_evidence_chain(KaiEvidence *src);

/* m8 #3 + m8.x: fiber lifecycle states. m8 v1 used only NEW/DONE
 * (the inline-eager scheduler ran spawn synchronously); m8.x adds
 * READY (enqueued in the run queue), RUNNING (currently dispatched),
 * and PARKED (blocked on await / receive / send). Spec:
 * docs/fibers-impl.md §*Fiber state machine*. The numeric values are
 * not stable across versions — no ABI commitment yet (CLAUDE.md
 * "Backward compatibility — not promised until post-MVP"). */
typedef enum {
    KAI_FIBER_NEW       = 0,
    KAI_FIBER_READY     = 1,  /* m8.x */
    KAI_FIBER_RUNNING   = 2,  /* m8.x */
    KAI_FIBER_PARKED    = 3,  /* m8.x */
    KAI_FIBER_DONE      = 4,
    KAI_FIBER_CANCELLED = 5
} KaiFiberState;

typedef struct KaiFiber   KaiFiber;
typedef struct KaiLinkNode KaiLinkNode;  /* Phase 5 — defined below */
typedef struct KaiMonitorNode KaiMonitorNode;  /* Tier 2 Monitor — defined below */

struct KaiFiber {
    KaiEvidence    *evidence_top;
    int             cancel_requested;  /* Spawn.cancel(target) sets this (#4) */
    int             cancel_delivered;  /* Cancel.raise() injected once (#4)   */
    KaiFiber       *sched_next;        /* intrusive ready-queue link          */
    KaiFiber       *parent;            /* spawning fiber, NULL for the root   */
    KaiFiberState   state;
    KaiValue       *thunk;             /* held alive while the fiber runs     */
    KaiValue       *result;            /* set on DONE; what await returns     */
    /* m8.x cooperative scheduler additions. Spec: docs/fibers-impl.md
     * §*Scheduler*. main_fiber leaves ctx zero-initialised (filled in
     * by getcontext on first dispatch); spawned fibers fill ctx via
     * makecontext + a heap-allocated stack. */
    ucontext_t      ctx;               /* swapcontext target              */
    void           *stack_base;        /* heap-allocated; freed on RC=0   */
    size_t          stack_size;        /* set per fiber (env-configurable) */
    KaiFiber       *awaiters_head;     /* per-fiber awaiter chain head    */
    KaiFiber       *awaiters_next;     /* link into another fiber's chain */
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
    /* Phase 5 — intrusive list of linked peer fibers. Walked at
     * trampoline termination (DONE or CANCELLED branches) to set
     * cancel_requested on each peer. Owned by the fiber; nodes are
     * freed during the propagation walk and as a safety net in
     * kai_free_value's KAI_FIBER branch. */
    KaiLinkNode    *linked_head;
    /* m8 bug #12 (per-fiber dispatch state). Points at the evidence
     * node whose clause body is currently on this fiber's stack, or
     * NULL otherwise. `kai_evidence_lookup_node` skips it so a
     * `Eff.op(...)` invoked from inside the clause resolves to the
     * outer handler instead of recursing. Lives on the fiber (not
     * the node) because spawned fibers inherit the parent's
     * evidence_top — the same node is shared across fibers, so the
     * "in dispatch right now" state must be per-fiber. The op-call
     * site saves the previous value, sets this to its own node,
     * runs the clause, then restores. */
    KaiEvidence    *in_dispatch_node;
    /* R4 fix — back-pointer to the KaiValue wrapper that owns this
     * fiber struct. Set in kai_fiber_value when the wrapper is
     * allocated; the scheduler holds an incref on the wrapper from
     * spawn-enqueue until the trampoline's DONE/CANCELLED tail, which
     * `kai_decref`s `value`. Pairing the scheduler-side ref with the
     * caller-side ref makes `let _ = fiber_spawn(…)` (discarding the
     * Fiber value) safe: the wrapper stays alive while the struct is
     * still referenced from the run queue. NULL on `kai_main_fiber`,
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
    /* Tier 2 — most-recently-allocated mailbox owned by this fiber.
     * Set by kai_mailbox_alloc[_bounded] and cleared by
     * kai_mailbox_free. Read by kai_link_propagate_terminate when
     * trap_exit=1 to find a delivery target for the Exit string.
     * v1 simplification: nested with_mailbox is not tracked — the
     * inner allocation overwrites and the inner free clears the
     * slot, leaving the outer mailbox unreachable to the trap-exit
     * walker until the inner scope exits. Demos do not nest.
     * Forward-declared as struct KaiMailbox * because the full
     * KaiMailbox typedef sits below KaiFiber in this header. */
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
    /* Issue #104 — heap-cloned head of the inherited evidence chain.
     * At spawn time the runtime walks parent->evidence_top and
     * allocates a heap KaiEvidence for each frame, copying the
     * fields (eff_label, handler, parent). Child's evidence_top is
     * set to the head of this heap chain instead of sharing a
     * pointer into parent's stack. The clone is needed because the
     * parent's stack contents at the inherited frame addresses get
     * overwritten by any function call the parent makes after
     * popping those frames (printf, runtime ops, anything that
     * reuses the same SP region) — even when the parent's stack
     * mmap is kept alive. The lookup walk reads only eff_label and
     * parent fields, both copied; handler is deref'd only when a
     * frame matches, and default handlers point to static *Ev
     * structs that outlive every fiber. Inherited user handlers
     * whose *Ev sits on a parent's stack remain a separate
     * concern (the *Ev itself can be overwritten the same way) —
     * not safe across parent termination, tracked for follow-up. */
    KaiEvidence    *cloned_evidence_chain;
};

/* m7a #5 + m8.x: kai_main_fiber starts as the OS-thread context,
 * representing the dispatch loop. Its ctx is filled lazily on first
 * yield (getcontext at the moment we suspend the dispatch loop into
 * a fiber). The active-fiber pointer (kai_active_fiber) tracks
 * whoever is currently executing; kai_current_fiber returns it.
 * Spec: docs/fibers-impl.md §*Dispatch loop*. */
static KaiFiber kai_main_fiber = {
    NULL,                /* evidence_top */
    0, 0,                /* cancel_requested, cancel_delivered */
    NULL, NULL,          /* sched_next, parent */
    KAI_FIBER_RUNNING,   /* state — main starts running on the OS thread */
    NULL, NULL,          /* thunk, result */
    {0},                 /* ctx — getcontext fills it on first swap */
    NULL, 0,             /* stack_base, stack_size — main uses the OS stack */
    NULL, NULL,          /* awaiters_head, awaiters_next */
    {0}, 0,              /* cancel_pad, cancel_pad_set — main has no pad */
    NULL,                /* linked_head */
    NULL,                /* in_dispatch_node */
    NULL,                /* value — main has no wrapper */
    0,                   /* trap_exit — main starts opted out */
    NULL,                /* mailbox — set by with_mailbox if main uses one */
    NULL,                /* monitor_head — Monitor.monitor(...) appends here */
    NULL                 /* cloned_evidence_chain — main never inherits */
};

static KaiFiber *kai_active_fiber = &kai_main_fiber;

static KaiFiber *kai_current_fiber(void) {
    return kai_active_fiber;
}

/* m8 #1 + m8.x: ready queue (intrusive singly-linked, head/tail).
 * Fibers go on the queue when spawned (NEW→READY) or unparked
 * (PARKED→READY); off the queue when dispatched (READY→RUNNING).
 * The dispatch loop drains it; deadlock detection panics when the
 * queue is empty *and* parked fibers exist with no wakeup path. */
static KaiFiber *kai_ready_head = NULL;
static KaiFiber *kai_ready_tail = NULL;
static int       kai_parked_count = 0;  /* deadlock detection */

/* R4 fix — single-slot pending-free for fiber structs whose wrappers
 * went to RC=0 while the fiber itself was still the current fiber
 * (the trampoline tail's `kai_decref(self->value)` is the producer).
 * Drained at every entry point that follows a context switch (top of
 * trampoline, post-swapcontext in yield/park) so the freed stack is
 * never the one we are running on. */
static KaiFiber *kai_pending_free = NULL;

/* Forward decl — defined alongside the m8.x fiber stack allocator
 * later in the file, but used here by the free path (munmap needs the
 * stack_size + page_size total). */
static size_t kai_page_size(void);

/* Linux's <sys/mman.h> exposes MAP_ANONYMOUS; older BSD-style headers
 * (and the legacy macOS spelling) use MAP_ANON. Define whichever the
 * platform omits in terms of the other so the fiber stack mmap call
 * compiles on both. */
#if !defined(MAP_ANON) && defined(MAP_ANONYMOUS)
#  define MAP_ANON MAP_ANONYMOUS
#endif

static void kai_drain_pending_free(void) {
    KaiFiber *f = kai_pending_free;
    if (!f) return;
    kai_pending_free = NULL;
    /* thunk / result / linked_head were handled at wrapper-free time;
     * only stack + struct remain. The stack is an mmap region of
     * size stack_size + one guard page; pair the call with munmap
     * (free would corrupt the heap). */
    if (f->stack_base) {
        munmap(f->stack_base, f->stack_size + kai_page_size());
    }
    free(f);
}

/* m8 #3: wrap a heap-allocated KaiFiber in an opaque KAI_FIBER
 * value. The KaiFiber struct's lifetime is tied to the value's RC
 * (kai_free_value frees both together). The struct's `value`
 * back-pointer lets the scheduler retain its own incref on the
 * wrapper from spawn-enqueue until the trampoline tail (R4 fix). */
static KaiValue *kai_fiber_value(KaiFiber *f) {
    KaiValue *v = kai_alloc(KAI_FIBER);
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

/* Phase 4 forward decls: mailbox push/pop park/wake the calling
 * fiber via the scheduler primitives, which are defined further
 * down (after the handler-stack runtime). The decls here let the
 * mailbox ops compile in their natural file location. */
static void kai_sched_park(void);
static void kai_sched_unpark(KaiFiber *target);

static KaiMailbox *kai_mailbox_alloc(void) {
    KaiMailbox *mb = (KaiMailbox *) calloc(1, sizeof(KaiMailbox));
    if (!mb) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    /* default policy: unbounded — matches m8 #7 behaviour. */
    mb->cap          = 0;
    mb->overflow     = KAI_OVERFLOW_UNBOUNDED;
    /* Phase 5: associate the mailbox with the allocating fiber. v1
     * actor surface (with_mailbox) is the only path to mailbox_alloc,
     * and the allocating fiber IS the actor; spawn_actor uses
     * kai_mailbox_alloc_unowned + kai_mailbox_assign_owner to point
     * at the spawned fiber instead of the parent. */
    mb->owner_fiber  = kai_current_fiber();
    /* Tier 2 trap-exit: stamp the mailbox onto its owner fiber so
     * kai_link_propagate_terminate can locate it without a global
     * registry. Nested allocs overwrite; kai_mailbox_free clears. */
    if (mb->owner_fiber) mb->owner_fiber->mailbox = mb;
    return mb;
}

/* Tier 2 spawn_actor — variant of kai_mailbox_alloc that does NOT
 * stamp owner_fiber. Used by stdlib's `spawn_actor` so the
 * mailbox's owner can be reassigned to the spawned fiber via
 * kai_mailbox_assign_owner before the spawned body runs. The
 * `mailbox` slot on the parent fiber is left untouched too —
 * spawn_actor's mailbox does not belong to the parent. */
static KaiMailbox *kai_mailbox_alloc_unowned(void) {
    KaiMailbox *mb = (KaiMailbox *) calloc(1, sizeof(KaiMailbox));
    if (!mb) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    mb->cap         = 0;
    mb->overflow    = KAI_OVERFLOW_UNBOUNDED;
    mb->owner_fiber = NULL;  /* set later by kai_mailbox_assign_owner */
    return mb;
}

static KaiMailbox *kai_mailbox_alloc_bounded(int cap, int overflow) {
    KaiMailbox *mb = (KaiMailbox *) calloc(1, sizeof(KaiMailbox));
    if (!mb) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    /* Phase 4: BlockSender now supported via the per-mailbox sender
     * waiter queue + cooperative parking on full push. */
    mb->cap          = cap;
    mb->overflow     = overflow;
    mb->owner_fiber  = kai_current_fiber();
    if (mb->owner_fiber) mb->owner_fiber->mailbox = mb;
    return mb;
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

static void kai_mailbox_push(KaiMailbox *mb, KaiValue *msg) {
    /* m8 #8 + Phase 4: enforce policy on full. */
    if (mb->cap > 0 && mb->len >= mb->cap) {
        if (mb->overflow == KAI_OVERFLOW_DROP_NEWEST) {
            kai_decref(msg);
            return;
        } else if (mb->overflow == KAI_OVERFLOW_DROP_OLDEST) {
            /* Pop and discard the head; fall through to enqueue. */
            KaiMboxNode *old = mb->head;
            mb->head = old->next;
            if (!mb->head) { mb->tail = NULL; }
            kai_decref(old->msg);
            free(old);
            mb->len--;
        } else if (mb->overflow == KAI_OVERFLOW_BLOCK_SENDER) {
            /* Park sender until a receiver pops a slot. The cooperative
             * scheduler guarantees forward progress: the receiver that
             * eventually pops will wake one parked sender (FIFO). The
             * loop re-checks because between unpark and resume another
             * sender could have refilled the slot. */
            while (mb->len >= mb->cap) {
                kai_mailbox_waiter_enqueue(&mb->send_waiter_head,
                                            &mb->send_waiter_tail,
                                            kai_current_fiber());
                kai_sched_park();
            }
        }
    }
    KaiMboxNode *node = (KaiMboxNode *) calloc(1, sizeof(KaiMboxNode));
    if (!node) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    node->msg  = msg;
    node->next = NULL;
    if (mb->tail) { mb->tail->next = node; }
    else          { mb->head       = node; }
    mb->tail = node;
    mb->len++;
    /* Phase 4: wake one parked receiver if any. */
    KaiFiber *waiter = kai_mailbox_waiter_dequeue(&mb->recv_waiter_head,
                                                    &mb->recv_waiter_tail);
    if (waiter) kai_sched_unpark(waiter);
}

static KaiValue *kai_mailbox_pop(KaiMailbox *mb) {
    /* Phase 4: park the calling fiber until a sender enqueues. The
     * loop handles the case where another receiver took the slot
     * between our unpark and resume (rare but possible if multiple
     * fibers race on the same mailbox). */
    while (!mb->head) {
        kai_mailbox_waiter_enqueue(&mb->recv_waiter_head,
                                    &mb->recv_waiter_tail,
                                    kai_current_fiber());
        kai_sched_park();
    }
    KaiMboxNode *node = mb->head;
    mb->head = node->next;
    if (!mb->head) { mb->tail = NULL; }
    KaiValue *msg = node->msg;
    free(node);
    mb->len--;
    /* Phase 4: wake one parked sender if any (only meaningful for
     * BlockSender; unbounded and Drop* mailboxes never park senders). */
    KaiFiber *waiter = kai_mailbox_waiter_dequeue(&mb->send_waiter_head,
                                                    &mb->send_waiter_tail);
    if (waiter) kai_sched_unpark(waiter);
    return msg;
}

static void kai_mailbox_free(KaiMailbox *mb) {
    if (!mb) return;
    /* Tier 2 trap-exit: drop the back-pointer if the owner fiber
     * still has us as its current mailbox. Nested with_mailbox: the
     * inner free clears the slot even though the outer mailbox is
     * still alive — accepted v1 limitation. */
    if (mb->owner_fiber && mb->owner_fiber->mailbox == mb) {
        mb->owner_fiber->mailbox = NULL;
    }
    KaiMboxNode *node = mb->head;
    while (node) {
        KaiMboxNode *next = node->next;
        kai_decref(node->msg);
        free(node);
        node = next;
    }
    free(mb);
}

/* m8 #7: wrap a borrowed mailbox pointer as a KAI_PID value. The
 * mailbox itself is owned by the with_mailbox / spawn_actor scope
 * that allocated it; Pid values are non-owning handles that just
 * carry the address. */
static KaiValue *kai_pid_value(KaiMailbox *mb) {
    KaiValue *v = kai_alloc(KAI_PID);
    v->as.mb = mb;
    return v;
}

static void kai_free_value(KaiValue *v) {
    switch ((KaiTag) v->tag) {
        case KAI_STR:
            free(v->as.s.bytes);
            break;
        case KAI_CONS:
            kai_decref(v->as.cons.head);
            kai_decref(v->as.cons.tail);
            break;
        case KAI_RECORD:
            for (int i = 0; i < v->as.rec.n_fields; ++i) kai_decref(v->as.rec.fields[i]);
            free(v->as.rec.fields);
            free((void *) v->as.rec.names);
            break;
        case KAI_VARIANT:
            for (int i = 0; i < v->as.var.n_args; ++i) kai_decref(v->as.var.args[i]);
            free(v->as.var.args);
            break;
        case KAI_CLOSURE:
            for (int i = 0; i < v->as.clo.n_captures; ++i) kai_decref(v->as.clo.captures[i]);
            free(v->as.clo.captures);
            break;
        case KAI_ARRAY:
            for (int64_t i = 0; i < v->as.arr.len; ++i) kai_decref(v->as.arr.items[i]);
            free(v->as.arr.items);
            break;
        case KAI_FIBER:
            if (v->as.fib) {
                /* Issue #104 — free the heap-cloned inherited
                 * evidence chain. Each node was malloc'd in
                 * kai_default_spawn_spawn from parent's evidence
                 * chain at spawn time. The walk lives in a helper
                 * defined alongside the KaiEvidence struct further
                 * down (forward-declared above kai_free_value). */
                kai_free_cloned_evidence_chain(v->as.fib->cloned_evidence_chain);
                v->as.fib->cloned_evidence_chain = NULL;
                kai_decref(v->as.fib->thunk);
                kai_decref(v->as.fib->result);
                /* Phase 5: free any remaining link nodes. Normally
                 * the trampoline's kai_link_propagate_terminate
                 * empties this chain; the safety net here covers
                 * fibers that died without going through the
                 * trampoline (or through this path before the
                 * trampoline ran). */
                {
                    KaiLinkNode *ln = v->as.fib->linked_head;
                    while (ln) {
                        KaiLinkNode *next = ln->next;
                        free(ln);
                        ln = next;
                    }
                    v->as.fib->linked_head = NULL;
                }
                /* Tier 2 Monitor — same safety net as the link chain.
                 * Each entry holds an owning ref on its target_pid via
                 * kai_monitor_add; we release that ref before freeing
                 * the node so values referenced only by the monitor
                 * chain can be reclaimed. */
                {
                    KaiMonitorNode *mn = v->as.fib->monitor_head;
                    while (mn) {
                        KaiMonitorNode *next = mn->next;
                        if (mn->target_pid) kai_decref(mn->target_pid);
                        free(mn);
                        mn = next;
                    }
                    v->as.fib->monitor_head = NULL;
                }
                /* R4 fix — when the trampoline drops the scheduler's
                 * ref on its own wrapper at DONE/CANCELLED, the wrapper
                 * may go to RC=0 here while we are still running on
                 * the fiber's private stack. Freeing the stack now
                 * would yank the ground out from under the trampoline
                 * tail. Defer the struct + stack free to the next
                 * fiber's drain hook (top of trampoline / post-swap
                 * in yield/park). The single-slot pending pointer is
                 * sufficient because the only producer is the
                 * trampoline tail, and every consumer drains before
                 * any other produce can run. */
                if (v->as.fib == kai_current_fiber()) {
                    v->as.fib->value = NULL;
                    kai_pending_free = v->as.fib;
                } else {
                    /* m8.x: release the private stack. Main fiber has
                     * stack_base == NULL (uses the OS thread stack)
                     * and is statically allocated; spawned fibers own
                     * an mmap region of stack_size + guard page bytes
                     * and must release it via munmap. */
                    if (v->as.fib->stack_base) {
                        munmap(v->as.fib->stack_base,
                               v->as.fib->stack_size + kai_page_size());
                    }
                    free(v->as.fib);
                }
            }
            break;
        case KAI_PID:
            /* The mailbox is owned by the with_mailbox / spawn_actor
             * scope, NOT by the Pid value. Dropping a Pid handle
             * does not free the mailbox. */
            break;
        default: break;
    }
    free(v);
    /* trace */
    kai_rc_free_total++;
    kai_rc_live_now--;
}

static void kai_decref(KaiValue *v) {
    if (!v) return;
    if (v->rc == INT32_MAX) return;   /* m5 #7 — singleton, saturated */
    if (--v->rc == 0) kai_free_value(v);
}

/* m5 #4 — Perceus dup/drop wrappers callable as KaiValue-returning fns.
 *
 * `__perceus_dup` and `__perceus_drop` are AST-level magic names the
 * `perceus_pass` rewrites onto non-last EVar reads (dup) and unused-
 * binding scope ends (drop). The emitter lowers them to these
 * wrappers so the pass operates entirely through the prelude path
 * without bespoke C generation.
 *
 * `kai_internal_dup` is `kai_incref` with a typed return; the AST
 * rewrite wraps `EVar(x)` as `ECall(EVar("__perceus_dup"), [EVar(x)])`
 * and the emitter sees a normal call.
 *
 * `kai_internal_drop` decrefs and returns `unit`. Drops emit as
 * `SExprStmt(ECall(EVar("__perceus_drop"), [EVar(x)]))` so the unit
 * return goes into the discarded `KaiValue *_` slot of the existing
 * `SExprStmt` lowering. */
static KaiValue *kai_internal_dup(KaiValue *v) { return kai_incref(v); }
static KaiValue *kai_internal_drop(KaiValue *v) { kai_decref(v); return &kai_singleton_unit; }

/* ---------- constructors ---------- */

static KaiValue *kai_unit(void) { return &kai_singleton_unit; }

static KaiValue *kai_bool(int b) {
    return b ? &kai_singleton_true : &kai_singleton_false;
}

/* m5.x flip Phase 4 (small-int + char cache). Same idea as the
 * m5 #7 singleton pool — every cached entry carries
 * `rc = INT32_MAX` so `kai_incref` / `kai_decref` skip them.
 * The flag lazily initializes the table on first call so the
 * constructor stays trivial when the program never reaches one
 * of these constructors (e.g., a fizzbuzz that only allocates
 * strings). The ranges are deliberately narrow:
 *   - Int  [-128..127]: covers tight loop indices, list lengths
 *     under ~100 elements, comparison constants, AST tag ints.
 *   - Char [0..127]: ASCII printable + control. UTF-8 multibyte
 *     codepoints fall through to a fresh alloc.
 * On the kaic2 self-compile, the int+char tags account for
 * ~39M of 66M total allocs; the cache wipes most of those out
 * without changing any caller. */
#define KAI_INT_CACHE_LO   ((int64_t) -128)
#define KAI_INT_CACHE_HI   ((int64_t) 127)
#define KAI_INT_CACHE_SIZE 256
#define KAI_CHAR_CACHE_HI  ((uint32_t) 127)
#define KAI_CHAR_CACHE_SIZE 128

static KaiValue kai_int_cache[KAI_INT_CACHE_SIZE];
static KaiValue kai_char_cache[KAI_CHAR_CACHE_SIZE];
static int kai_int_cache_init  = 0;
static int kai_char_cache_init = 0;

static void kai_int_cache_warm(void) {
    for (int k = 0; k < KAI_INT_CACHE_SIZE; k++) {
        kai_int_cache[k].rc = INT32_MAX;
        kai_int_cache[k].tag = KAI_INT;
        kai_int_cache[k].as.i = (int64_t) k + KAI_INT_CACHE_LO;
    }
    kai_int_cache_init = 1;
}

static void kai_char_cache_warm(void) {
    for (int k = 0; k < KAI_CHAR_CACHE_SIZE; k++) {
        kai_char_cache[k].rc = INT32_MAX;
        kai_char_cache[k].tag = KAI_CHAR;
        kai_char_cache[k].as.c = (uint32_t) k;
    }
    kai_char_cache_init = 1;
}

static KaiValue *kai_int(int64_t i) {
    if (i >= KAI_INT_CACHE_LO && i <= KAI_INT_CACHE_HI) {
        if (!kai_int_cache_init) kai_int_cache_warm();
        return &kai_int_cache[i - KAI_INT_CACHE_LO];
    }
    KaiValue *v = kai_alloc(KAI_INT);
    v->as.i = i;
    return v;
}

static KaiValue *kai_real(double r) {
    KaiValue *v = kai_alloc(KAI_REAL);
    v->as.r = r;
    return v;
}

static KaiValue *kai_char(uint32_t c) {
    if (c <= KAI_CHAR_CACHE_HI) {
        if (!kai_char_cache_init) kai_char_cache_warm();
        return &kai_char_cache[c];
    }
    KaiValue *v = kai_alloc(KAI_CHAR);
    v->as.c = c;
    return v;
}

static KaiValue *kai_str_from_bytes(const char *bytes, size_t len) {
    KaiValue *v = kai_alloc(KAI_STR);
    v->as.s.len = len;
    v->as.s.bytes = (char *) malloc(len + 1);
    if (!v->as.s.bytes) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    if (len > 0) memcpy(v->as.s.bytes, bytes, len);
    v->as.s.bytes[len] = '\0';
    return v;
}

static KaiValue *kai_str(const char *cstr) {
    return kai_str_from_bytes(cstr, strlen(cstr));
}

static KaiValue *kai_nil(void) { return &kai_singleton_nil; }

static KaiValue *kai_cons(KaiValue *head, KaiValue *tail) {
    KaiValue *v = kai_alloc(KAI_CONS);
    v->as.cons.head = head;
    v->as.cons.tail = tail;
    return v;
}

static KaiValue *kai_record(int n, KaiValue **fields, const char **names) {
    KaiValue *v = kai_alloc(KAI_RECORD);
    v->as.rec.n_fields = n;
    v->as.rec.fields = (KaiValue **) malloc(n * sizeof(KaiValue *));
    v->as.rec.names  = (const char **) malloc(n * sizeof(const char *));
    for (int i = 0; i < n; ++i) {
        v->as.rec.fields[i] = fields[i];
        v->as.rec.names[i]  = names[i];
    }
    return v;
}

static KaiValue *kai_variant(int32_t tag, const char *name, int n, KaiValue **args) {
    KaiValue *v = kai_alloc(KAI_VARIANT);
    v->as.var.variant_tag = tag;
    v->as.var.variant_name = name;
    v->as.var.n_args = n;
    if (n > 0) {
        v->as.var.args = (KaiValue **) malloc(n * sizeof(KaiValue *));
        for (int i = 0; i < n; ++i) v->as.var.args[i] = args[i];
    } else {
        v->as.var.args = NULL;
    }
    return v;
}

/* ---------- Perceus reuse-in-place (issue #118 / Anga Roa wave) ----------
 *
 * Koka-style reuse-in-place: when a constructor consumes a value the
 * Perceus pass can prove uniquely owned (RC == 1), reuse the consumed
 * cell as the storage for the new value instead of paired free + alloc.
 *
 * Calling convention — these helpers are invoked from inside a
 * `match` arm body. The match emitter (`emit_match_default`) wraps
 * the scrutinee as `_scr` and *always* `kai_decref(_scr)`s on exit.
 * To avoid a double-free / use-after-free on a reused cell, these
 * helpers `kai_incref` the cell on the unique branch — the extra
 * ref balances the post-arm `kai_decref(_scr)`, leaving the caller
 * with a unique (RC=1) cell. The fallback branch leaves `_scr`
 * untouched; the match exit reclaims it normally.
 *
 * Discipline summary (per branch):
 *   - Unique: rewrite children in place, `kai_incref(_scr)`, return
 *     `_scr`. Net RC after match exit: 1.
 *   - Not unique / wrong shape: leave `_scr` alone, allocate fresh
 *     via the existing `kai_<shape>` constructor. Net RC after match
 *     exit: original_rc - 1.
 *
 * `head` / `tail` / `fields` / `args` are owning refs on entry — the
 * allocator branch hands them straight to `kai_<shape>` (which does
 * not incref); the reuse branch decref's the outgoing children
 * before storing the incoming ones, exactly as `kai_free_value`
 * would have during a paired free + alloc.
 *
 * The trace counter `kai_rc_reuse_total` increments on every
 * successful in-place rewrite (see top of file).
 */
static int kai_check_unique(KaiValue *v) {
    /* Singletons (rc == INT32_MAX) are NEVER unique — they are
     * shared, immutable, and live in .data. The recogniser only
     * fires on KAI_CONS / KAI_RECORD / KAI_VARIANT, none of which
     * are pooled today, so the singleton check is defensive but
     * cheap and keeps the predicate honest. */
    return v != NULL && v->rc == 1 && v->rc != INT32_MAX;
}

static KaiValue *kai_reuse_or_alloc_cons(KaiValue *_scr,
                                         KaiValue *head, KaiValue *tail) {
    if (_scr != NULL && _scr->tag == KAI_CONS && kai_check_unique(_scr)) {
        kai_decref(_scr->as.cons.head);
        kai_decref(_scr->as.cons.tail);
        _scr->as.cons.head = head;
        _scr->as.cons.tail = tail;
        kai_rc_reuse_total++;
        return kai_incref(_scr);   /* survive enclosing match-exit decref */
    }
    return kai_cons(head, tail);
}

static KaiValue *kai_reuse_or_alloc_record(KaiValue *_scr,
                                           int n, KaiValue **fields,
                                           const char **names) {
    if (_scr != NULL && _scr->tag == KAI_RECORD &&
        _scr->as.rec.n_fields == n && kai_check_unique(_scr)) {
        /* Decref the existing field values then overwrite. The
         * fields[] / names[] pointer arrays are reused — both have
         * the same length n, so no realloc is needed. names[]
         * usually points at the same static-string literals across
         * rebuilds (parsers thread the same record shape), so
         * overwriting is idempotent in the common case. */
        for (int i = 0; i < n; ++i) {
            kai_decref(_scr->as.rec.fields[i]);
            _scr->as.rec.fields[i] = fields[i];
            _scr->as.rec.names[i]  = names[i];
        }
        kai_rc_reuse_total++;
        return kai_incref(_scr);
    }
    return kai_record(n, fields, names);
}

static KaiValue *kai_reuse_or_alloc_variant(KaiValue *_scr,
                                            int32_t tag, const char *name,
                                            int n, KaiValue **args) {
    if (_scr != NULL && _scr->tag == KAI_VARIANT &&
        _scr->as.var.n_args == n && kai_check_unique(_scr)) {
        for (int i = 0; i < n; ++i) {
            kai_decref(_scr->as.var.args[i]);
            _scr->as.var.args[i] = args[i];
        }
        _scr->as.var.variant_tag  = tag;
        _scr->as.var.variant_name = name;
        kai_rc_reuse_total++;
        return kai_incref(_scr);
    }
    return kai_variant(tag, name, n, args);
}

/* Allocate an array of `len` slots, each initialised to `init`
   (incref'd once per slot). Caller owns the returned array. */
static KaiValue *kai_array_make(int64_t len, KaiValue *init) {
    if (len < 0) { fprintf(stderr, "kai: array_make: negative length\n"); exit(1); }
    KaiValue *v = kai_alloc(KAI_ARRAY);
    v->as.arr.len = len;
    v->as.arr.cap = len > 0 ? len : 1;
    v->as.arr.items = (KaiValue **) malloc((size_t) v->as.arr.cap * sizeof(KaiValue *));
    if (!v->as.arr.items) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    for (int64_t i = 0; i < len; ++i) v->as.arr.items[i] = kai_incref(init);
    return v;
}

/* Grow `a` to length `new_len`, initialising new slots with `init`.
   Mutates in place, returns the same value (incref'd for the caller
   so the result can be bound just like kai_array_make). If new_len
   <= current len this is a no-op beyond the incref. */
static KaiValue *kai_array_grow_impl(KaiValue *a, int64_t new_len, KaiValue *init) {
    if (!a || a->tag != KAI_ARRAY) {
        fprintf(stderr, "kai: array_grow: not an array\n"); exit(1);
    }
    if (new_len > a->as.arr.cap) {
        int64_t nc = a->as.arr.cap * 2;
        if (nc < new_len) nc = new_len;
        a->as.arr.items = (KaiValue **) realloc(a->as.arr.items, (size_t) nc * sizeof(KaiValue *));
        if (!a->as.arr.items) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
        a->as.arr.cap = nc;
    }
    for (int64_t i = a->as.arr.len; i < new_len; ++i) a->as.arr.items[i] = kai_incref(init);
    if (new_len > a->as.arr.len) a->as.arr.len = new_len;
    return kai_incref(a);
}

/* O(1) read. Callers own the returned reference. */
static KaiValue *kai_array_get_impl(KaiValue *a, int64_t i) {
    if (!a || a->tag != KAI_ARRAY) {
        fprintf(stderr, "kai: array_get: not an array\n"); exit(1);
    }
    if (i < 0 || i >= a->as.arr.len) {
        fprintf(stderr, "kai: array_get: index %lld out of range (len=%lld)\n",
                (long long) i, (long long) a->as.arr.len); exit(1);
    }
    return kai_incref(a->as.arr.items[i]);
}

/* O(1) write. Takes ownership of `v`, decref's the previous slot.
   Returns the same array (incref'd) so callers can thread it. */
static KaiValue *kai_array_set_impl(KaiValue *a, int64_t i, KaiValue *v) {
    if (!a || a->tag != KAI_ARRAY) {
        fprintf(stderr, "kai: array_set: not an array\n"); exit(1);
    }
    if (i < 0 || i >= a->as.arr.len) {
        fprintf(stderr, "kai: array_set: index %lld out of range (len=%lld)\n",
                (long long) i, (long long) a->as.arr.len); exit(1);
    }
    kai_decref(a->as.arr.items[i]);
    a->as.arr.items[i] = v;
    return kai_incref(a);
}

/* m5.x #2: incref each captured value so the closure owns its own
 * reference. Symmetric with `kai_free_value` (KAI_CLOSURE branch),
 * which decrefs every capture on chain-free. Without this incref the
 * closure stores raw aliases of caller-owned bindings; the alias goes
 * dangling as soon as the caller's scope decrefs them — latent under
 * the loose runtime, a use-after-free under linear consumption. */
static KaiValue *kai_closure(KaiFn fn, int arity, int n_captures, KaiValue **captures) {
    KaiValue *v = kai_alloc(KAI_CLOSURE);
    v->as.clo.fn = fn;
    v->as.clo.arity = arity;
    v->as.clo.n_captures = n_captures;
    if (n_captures > 0) {
        v->as.clo.captures = (KaiValue **) malloc(n_captures * sizeof(KaiValue *));
        for (int i = 0; i < n_captures; ++i) v->as.clo.captures[i] = kai_incref(captures[i]);
    } else {
        v->as.clo.captures = NULL;
    }
    return v;
}

/* Invoke a closure dynamically. */
static KaiValue *kai_apply(KaiValue *clo, int argc, KaiValue **argv) {
    if (!clo || clo->tag != KAI_CLOSURE) {
        fprintf(stderr, "kai: attempted to call a non-callable value\n");
        exit(1);
    }
    return clo->as.clo.fn(clo, argv, argc);
}

/* ---------- field access helpers ---------- */

static KaiValue *kai_op_field(KaiValue *rec, const char *name) {
    if (!rec || rec->tag != KAI_RECORD) {
        fprintf(stderr, "kai: field access on non-record\n"); exit(1);
    }
    for (int i = 0; i < rec->as.rec.n_fields; ++i) {
        if (strcmp(rec->as.rec.names[i], name) == 0) {
            return kai_incref(rec->as.rec.fields[i]);
        }
    }
    fprintf(stderr, "kai: no such field `%s`\n", name); exit(1);
}

/* m5.x §4b sibling: borrow the field without incref. Used in pat_test
   paths where the caller only reads the field's tag / value to decide
   the arm match — no downstream consumer that would need an owned ref.
   Pre-fix every pat_test of a record-shaped pattern leaked one ref per
   field tested (kai_op_field always increfed; the test result was
   discarded). emit_pat_binds keeps using the incref-ing kai_op_field so
   bindings still own their own refs. */
static KaiValue *kai_op_field_borrow(KaiValue *rec, const char *name) {
    if (!rec || rec->tag != KAI_RECORD) {
        fprintf(stderr, "kai: field access on non-record\n"); exit(1);
    }
    for (int i = 0; i < rec->as.rec.n_fields; ++i) {
        if (strcmp(rec->as.rec.names[i], name) == 0) {
            return rec->as.rec.fields[i];
        }
    }
    fprintf(stderr, "kai: no such field `%s`\n", name); exit(1);
}

/* ---------- equality ---------- */

static int kai_op_eq(KaiValue *a, KaiValue *b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->tag != b->tag) return 0;
    switch ((KaiTag) a->tag) {
        case KAI_UNIT: return 1;
        case KAI_BOOL: return a->as.b == b->as.b;
        case KAI_INT:  return a->as.i == b->as.i;
        case KAI_REAL: return a->as.r == b->as.r;
        case KAI_CHAR: return a->as.c == b->as.c;
        case KAI_STR:  return a->as.s.len == b->as.s.len &&
                              memcmp(a->as.s.bytes, b->as.s.bytes, a->as.s.len) == 0;
        case KAI_NIL:  return 1;
        case KAI_CONS:
            return kai_op_eq(a->as.cons.head, b->as.cons.head) &&
                   kai_op_eq(a->as.cons.tail, b->as.cons.tail);
        case KAI_VARIANT:
            if (a->as.var.variant_tag != b->as.var.variant_tag) return 0;
            if (a->as.var.n_args != b->as.var.n_args) return 0;
            for (int i = 0; i < a->as.var.n_args; ++i) {
                if (!kai_op_eq(a->as.var.args[i], b->as.var.args[i])) return 0;
            }
            return 1;
        case KAI_RECORD:
            if (a->as.rec.n_fields != b->as.rec.n_fields) return 0;
            for (int i = 0; i < a->as.rec.n_fields; ++i) {
                if (!kai_op_eq(a->as.rec.fields[i], b->as.rec.fields[i])) return 0;
            }
            return 1;
        case KAI_CLOSURE: return 0;      /* closures are not equatable */
        case KAI_ARRAY:   return 0;      /* arrays are opaque, identity-compared */
        case KAI_FIBER:   return a->as.fib == b->as.fib;  /* identity */
        case KAI_PID:     return a->as.mb  == b->as.mb;   /* identity */
    }
    return 0;
}

/* ---------- to-string ---------- */

static KaiValue *kai_string_concat(KaiValue *a, KaiValue *b);

static KaiValue *kai_to_string(KaiValue *v);

static KaiValue *kai_list_to_string(KaiValue *v) {
    KaiValue *acc = kai_str("[");
    int first = 1;
    while (v && v->tag == KAI_CONS) {
        if (!first) {
            KaiValue *c = kai_string_concat(acc, kai_str(", "));
            kai_decref(acc); acc = c;
        }
        first = 0;
        KaiValue *s = kai_to_string(v->as.cons.head);
        KaiValue *c = kai_string_concat(acc, s);
        kai_decref(acc); kai_decref(s); acc = c;
        v = v->as.cons.tail;
    }
    KaiValue *c = kai_string_concat(acc, kai_str("]"));
    kai_decref(acc); return c;
}

static KaiValue *kai_to_string(KaiValue *v) {
    if (!v) return kai_str("<null>");
    char buf[64];
    switch ((KaiTag) v->tag) {
        case KAI_UNIT: return kai_str("()");
        case KAI_BOOL: return kai_str(v->as.b ? "true" : "false");
        case KAI_INT:
            snprintf(buf, sizeof(buf), "%lld", (long long) v->as.i);
            return kai_str(buf);
        case KAI_REAL:
            snprintf(buf, sizeof(buf), "%g", v->as.r);
            return kai_str(buf);
        case KAI_CHAR:
            snprintf(buf, sizeof(buf), "%c", (char) v->as.c);
            return kai_str(buf);
        case KAI_STR:    return kai_incref(v);
        case KAI_NIL:    return kai_str("[]");
        case KAI_CONS:   return kai_list_to_string(v);
        case KAI_VARIANT: {
            KaiValue *acc = kai_str(v->as.var.variant_name);
            if (v->as.var.n_args > 0) {
                KaiValue *lp = kai_string_concat(acc, kai_str("(")); kai_decref(acc); acc = lp;
                for (int i = 0; i < v->as.var.n_args; ++i) {
                    if (i) { KaiValue *sep = kai_string_concat(acc, kai_str(", ")); kai_decref(acc); acc = sep; }
                    KaiValue *s = kai_to_string(v->as.var.args[i]);
                    KaiValue *c = kai_string_concat(acc, s); kai_decref(acc); kai_decref(s); acc = c;
                }
                KaiValue *rp = kai_string_concat(acc, kai_str(")")); kai_decref(acc); acc = rp;
            }
            return acc;
        }
        case KAI_RECORD: {
            KaiValue *acc = kai_str("{");
            for (int i = 0; i < v->as.rec.n_fields; ++i) {
                if (i) { KaiValue *sep = kai_string_concat(acc, kai_str(", ")); kai_decref(acc); acc = sep; }
                KaiValue *nm = kai_str(v->as.rec.names[i]);
                KaiValue *a1 = kai_string_concat(acc, nm); kai_decref(acc); kai_decref(nm); acc = a1;
                KaiValue *a2 = kai_string_concat(acc, kai_str(": ")); kai_decref(acc); acc = a2;
                KaiValue *fs = kai_to_string(v->as.rec.fields[i]);
                KaiValue *a3 = kai_string_concat(acc, fs); kai_decref(acc); kai_decref(fs); acc = a3;
            }
            KaiValue *cl = kai_string_concat(acc, kai_str("}")); kai_decref(acc);
            return cl;
        }
        case KAI_CLOSURE: return kai_str("<closure>");
        case KAI_ARRAY:   return kai_str("<array>");
        case KAI_FIBER:   return kai_str("<fiber>");
        case KAI_PID:     return kai_str("<pid>");
    }
    return kai_str("?");
}

static KaiValue *kai_string_concat(KaiValue *a, KaiValue *b) {
    size_t la = (a && a->tag == KAI_STR) ? a->as.s.len : 0;
    size_t lb = (b && b->tag == KAI_STR) ? b->as.s.len : 0;
    KaiValue *v = kai_alloc(KAI_STR);
    v->as.s.len = la + lb;
    v->as.s.bytes = (char *) malloc(la + lb + 1);
    if (la) memcpy(v->as.s.bytes, a->as.s.bytes, la);
    if (lb) memcpy(v->as.s.bytes + la, b->as.s.bytes, lb);
    v->as.s.bytes[la + lb] = '\0';
    return v;
}

/* Two-pass concat of every KAI_STR in the cons list: measure total
   length, allocate once, memcpy each piece in. Avoids the O(n²)
   accumulation that a naive fold of kai_string_concat produces, which
   dominates emit-heavy workloads like the self-hosting compiler. */
static KaiValue *kai_string_concat_all_impl(KaiValue *xs) {
    size_t total = 0;
    for (KaiValue *p = xs; p && p->tag == KAI_CONS; p = p->as.cons.tail) {
        KaiValue *s = p->as.cons.head;
        if (s && s->tag == KAI_STR) total += s->as.s.len;
    }
    KaiValue *v = kai_alloc(KAI_STR);
    v->as.s.len = total;
    v->as.s.bytes = (char *) malloc(total + 1);
    if (!v->as.s.bytes) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    size_t off = 0;
    for (KaiValue *p = xs; p && p->tag == KAI_CONS; p = p->as.cons.tail) {
        KaiValue *s = p->as.cons.head;
        if (s && s->tag == KAI_STR && s->as.s.len > 0) {
            memcpy(v->as.s.bytes + off, s->as.s.bytes, s->as.s.len);
            off += s->as.s.len;
        }
    }
    v->as.s.bytes[total] = '\0';
    return v;
}

/* Two-pass join: like concat_all but interleaves `sep` between pieces. */
static KaiValue *kai_string_join_impl(KaiValue *xs, KaiValue *sep) {
    size_t slen = (sep && sep->tag == KAI_STR) ? sep->as.s.len : 0;
    size_t total = 0;
    int count = 0;
    for (KaiValue *p = xs; p && p->tag == KAI_CONS; p = p->as.cons.tail) {
        KaiValue *s = p->as.cons.head;
        if (s && s->tag == KAI_STR) total += s->as.s.len;
        count++;
    }
    if (count > 1) total += slen * (size_t)(count - 1);
    KaiValue *v = kai_alloc(KAI_STR);
    v->as.s.len = total;
    v->as.s.bytes = (char *) malloc(total + 1);
    if (!v->as.s.bytes) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    size_t off = 0;
    int first = 1;
    for (KaiValue *p = xs; p && p->tag == KAI_CONS; p = p->as.cons.tail) {
        if (!first && slen > 0) {
            memcpy(v->as.s.bytes + off, sep->as.s.bytes, slen);
            off += slen;
        }
        first = 0;
        KaiValue *s = p->as.cons.head;
        if (s && s->tag == KAI_STR && s->as.s.len > 0) {
            memcpy(v->as.s.bytes + off, s->as.s.bytes, s->as.s.len);
            off += s->as.s.len;
        }
    }
    v->as.s.bytes[total] = '\0';
    return v;
}

/* ---------- prelude: IO ---------- */

/*
 * Calling convention for the prelude: borrow semantics. Arguments are
 * borrowed from the caller (no incref / no decref on them). Return
 * values are owned by the caller.
 */

static KaiValue *kai_prelude_print(KaiValue *arg) {
    if (!arg) { fputc('\n', stdout); return kai_unit(); }
    if (arg->tag == KAI_STR) {
        fwrite(arg->as.s.bytes, 1, arg->as.s.len, stdout);
    } else {
        KaiValue *s = kai_to_string(arg);
        fwrite(s->as.s.bytes, 1, s->as.s.len, stdout);
        kai_decref(s);
    }
    fputc('\n', stdout);
    kai_decref(arg);
    return kai_unit();
}

static KaiValue *kai_prelude_eprint(KaiValue *arg) {
    if (!arg) { fputc('\n', stderr); return kai_unit(); }
    if (arg->tag == KAI_STR) {
        fwrite(arg->as.s.bytes, 1, arg->as.s.len, stderr);
    } else {
        KaiValue *s = kai_to_string(arg);
        fwrite(s->as.s.bytes, 1, s->as.s.len, stderr);
        kai_decref(s);
    }
    fputc('\n', stderr);
    kai_decref(arg);
    return kai_unit();
}

static KaiValue *kai_prelude_panic(KaiValue *msg) {
    fprintf(stderr, "panic: ");
    if (msg && msg->tag == KAI_STR) {
        fwrite(msg->as.s.bytes, 1, msg->as.s.len, stderr);
    }
    fputc('\n', stderr);
    exit(1);
    return kai_unit();
}

static KaiValue *kai_prelude_exit(KaiValue *code) {
    int c = (code && code->tag == KAI_INT) ? (int) code->as.i : 0;
    exit(c);
    return kai_unit();
}

/* ---------- prelude: conversions ---------- */

static KaiValue *kai_prelude_int_to_string(KaiValue *v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long) v->as.i);
    KaiValue *r = kai_str(buf);
    kai_decref(v);
    return r;
}

static KaiValue *kai_prelude_real_to_string(KaiValue *v) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", v->as.r);
    KaiValue *r = kai_str(buf);
    kai_decref(v);
    return r;
}

static KaiValue *kai_prelude_int_to_real(KaiValue *v) {
    int64_t n = (v && v->tag == KAI_INT) ? v->as.i : 0;
    KaiValue *r = kai_real((double) n);
    kai_decref(v);
    return r;
}

/* Truncating cast toward zero. Out-of-range, NaN and Inf collapse to
 * 0 — kaikai has no IEEE 754 surface for the user yet, so the safer
 * default beats a UB conversion. Revisit when the math/real lane
 * adds NaN-aware predicates. */
static KaiValue *kai_prelude_real_to_int(KaiValue *v) {
    if (!v || v->tag != KAI_REAL) { if (v) kai_decref(v); return kai_int(0); }
    double r = v->as.r;
    KaiValue *out;
    if (r != r) out = kai_int(0);                       /* NaN */
    else if (r >  9.2233720368547748e18) out = kai_int(0); /* > INT64_MAX */
    else if (r < -9.2233720368547758e18) out = kai_int(0); /* < INT64_MIN */
    else out = kai_int((int64_t) r);
    kai_decref(v);
    return out;
}

/* ---------- prelude: strings ---------- */

static KaiValue *kai_prelude_string_length(KaiValue *s) {
    int64_t n = (s && s->tag == KAI_STR) ? (int64_t) s->as.s.len : 0;
    KaiValue *r = kai_int(n);
    if (s) kai_decref(s);
    return r;
}

static KaiValue *kai_prelude_string_concat(KaiValue *a, KaiValue *b) {
    KaiValue *r = kai_string_concat(a, b);
    if (a) kai_decref(a);
    if (b) kai_decref(b);
    return r;
}

static KaiValue *kai_prelude_string_concat_all(KaiValue *xs) {
    KaiValue *r = kai_string_concat_all_impl(xs);
    if (xs) kai_decref(xs);
    return r;
}

static KaiValue *kai_prelude_string_join(KaiValue *xs, KaiValue *sep) {
    KaiValue *r = kai_string_join_impl(xs, sep);
    if (xs) kai_decref(xs);
    if (sep) kai_decref(sep);
    return r;
}

/* ---------- prelude: arrays ---------- */

static KaiValue *kai_prelude_array_make(KaiValue *n, KaiValue *init) {
    int64_t len = (n && n->tag == KAI_INT) ? n->as.i : 0;
    /* impl increfs `init` once per slot; consume our own input
     * refs (n and init) at the boundary. */
    KaiValue *r = kai_array_make(len, init);
    if (n) kai_decref(n);
    if (init) kai_decref(init);
    return r;
}

static KaiValue *kai_prelude_array_length(KaiValue *a) {
    int64_t len = (a && a->tag == KAI_ARRAY) ? a->as.arr.len : 0;
    KaiValue *r = kai_int(len);
    if (a) kai_decref(a);
    return r;
}

static KaiValue *kai_prelude_array_get(KaiValue *a, KaiValue *i) {
    int64_t idx = (i && i->tag == KAI_INT) ? i->as.i : 0;
    KaiValue *r = kai_array_get_impl(a, idx);
    if (a) kai_decref(a);
    if (i) kai_decref(i);
    return r;
}

static KaiValue *kai_prelude_array_set(KaiValue *a, KaiValue *i, KaiValue *v) {
    int64_t idx = (i && i->tag == KAI_INT) ? i->as.i : 0;
    /* impl returns kai_incref(a) — a fresh ref the caller owns.
     * Our input `a` ref is therefore redundant under the callee-
     * consumes convention; decref it so the array's refcount only
     * reflects (1) the slot self-ref + (2) the caller's returned
     * ref, never the borrowed entry. */
    KaiValue *r = kai_array_set_impl(a, idx, kai_incref(v));
    if (a) kai_decref(a);
    if (i) kai_decref(i);
    if (v) kai_decref(v);
    return r;
}

static KaiValue *kai_prelude_array_grow(KaiValue *a, KaiValue *n, KaiValue *init) {
    int64_t new_len = (n && n->tag == KAI_INT) ? n->as.i : 0;
    /* impl returns kai_incref(a) — caller owns the new ref. The
     * input `a` ref is consumed under the callee-consumes
     * convention. */
    KaiValue *r = kai_array_grow_impl(a, new_len, init);
    if (a) kai_decref(a);
    if (n) kai_decref(n);
    if (init) kai_decref(init);
    return r;
}

/* ---------- prelude: lists ---------- */

static KaiValue *kai_prelude_list_length(KaiValue *xs) {
    int64_t n = 0;
    KaiValue *p = xs;
    while (p && p->tag == KAI_CONS) { n++; p = p->as.cons.tail; }
    KaiValue *r = kai_int(n);
    if (xs) kai_decref(xs);
    return r;
}

/* Recursive helper: borrows `xs` (does NOT decref it), so the
 * recursion can walk the cons chain without consuming on the way
 * down. The public `kai_prelude_list_append` wraps this and
 * decrefs `xs`/`ys` after the chain is built. */
static KaiValue *kai_list_append_borrow(KaiValue *xs, KaiValue *ys) {
    if (!xs || xs->tag == KAI_NIL) return kai_incref(ys);
    KaiValue *rest = kai_list_append_borrow(xs->as.cons.tail, ys);
    return kai_cons(kai_incref(xs->as.cons.head), rest);
}

static KaiValue *kai_prelude_list_append(KaiValue *xs, KaiValue *ys) {
    KaiValue *r = kai_list_append_borrow(xs, ys);
    if (xs) kai_decref(xs);
    if (ys) kai_decref(ys);
    return r;
}

static KaiValue *kai_prelude_list_reverse(KaiValue *xs) {
    KaiValue *acc = kai_nil();
    KaiValue *p   = xs;
    while (p && p->tag == KAI_CONS) {
        acc = kai_cons(kai_incref(p->as.cons.head), acc);
        p   = p->as.cons.tail;
    }
    if (xs) kai_decref(xs);
    return acc;
}

/* ---------- prelude: higher order ---------- */
/*
 * Iterative refactor (Perceus Tier 2 part 3, 2026-04-29). Pre-flip
 * these were recursive over `xs->as.cons.tail`, which forced the
 * helpers to *borrow* `xs` (the recursion holds aliased pointers
 * to every cell while the closure runs). Now they walk iteratively
 * with `KaiValue *p = xs; p = p->as.cons.tail;` so the entire
 * cons-chain stays alive under `xs`'s single reference until the
 * helper exits, and we can decref `xs` once at the end.
 *
 * kai_apply contract under m5.x flip: every arg slot holds an OWNED
 * reference that the callee consumes (the closure body is perceus-
 * compiled and decrefs each arg through normal use). The helpers
 * `kai_incref(p->as.cons.head)` to give the closure its own
 * ownership of each element while the cons cell stays alive under
 * `xs`. `kai_apply` itself does NOT consume `f` — the helper does
 * that once, post-loop.
 *
 * `_map` / `_filter` build their result reversed (each iteration
 * cons-prepends) and call `kai_prelude_list_reverse` once to
 * restore order. `list_reverse` consumes its arg, so the
 * intermediate reversed list is freed in the same step that
 * produces the final list — no extra retention.
 */

static KaiValue *kai_prelude_map(KaiValue *xs, KaiValue *f) {
    KaiValue *acc = kai_nil();
    KaiValue *p = xs;
    while (p && p->tag == KAI_CONS) {
        KaiValue *arg0 = kai_incref(p->as.cons.head);
        KaiValue *head = kai_apply(f, 1, &arg0);
        acc = kai_cons(head, acc);
        p = p->as.cons.tail;
    }
    KaiValue *result = kai_prelude_list_reverse(acc);  /* consumes acc */
    if (xs) kai_decref(xs);
    if (f)  kai_decref(f);
    return result;
}

/* issue #201: runtime backing for the flat-map-pipe operator (`||`).
 * Mirrors `kai_prelude_map` but each `f(elem)` produces a list that
 * gets cons-prepended onto the reversed accumulator; the per-element
 * piece is decref'd in the same step. The final reverse + decref loop
 * matches `_map`'s ownership story so RC stays balanced regardless of
 * the input shape. Empty pieces are a no-op for the inner loop.
 */
static KaiValue *kai_prelude_flat_map(KaiValue *xs, KaiValue *f) {
    KaiValue *acc = kai_nil();
    KaiValue *p = xs;
    while (p && p->tag == KAI_CONS) {
        KaiValue *arg0 = kai_incref(p->as.cons.head);
        KaiValue *piece = kai_apply(f, 1, &arg0);
        /* Each `piece` is owned; append it onto `acc` (which is in
         * reverse order). `kai_prelude_list_append` consumes both
         * arguments. We append `acc` onto the front of `piece`'s
         * reversed form by reversing piece into acc cons-by-cons.
         */
        KaiValue *q = piece;
        while (q && q->tag == KAI_CONS) {
            acc = kai_cons(kai_incref(q->as.cons.head), acc);
            q = q->as.cons.tail;
        }
        if (piece) kai_decref(piece);
        p = p->as.cons.tail;
    }
    KaiValue *result = kai_prelude_list_reverse(acc);  /* consumes acc */
    if (xs) kai_decref(xs);
    if (f)  kai_decref(f);
    return result;
}

static KaiValue *kai_prelude_filter(KaiValue *xs, KaiValue *pred) {
    KaiValue *acc = kai_nil();
    KaiValue *p = xs;
    while (p && p->tag == KAI_CONS) {
        KaiValue *arg0 = kai_incref(p->as.cons.head);
        KaiValue *keep = kai_apply(pred, 1, &arg0);
        int yes = kai_op_truthy(keep);
        kai_decref(keep);
        if (yes) {
            acc = kai_cons(kai_incref(p->as.cons.head), acc);
        }
        p = p->as.cons.tail;
    }
    KaiValue *result = kai_prelude_list_reverse(acc);  /* consumes acc */
    if (xs)   kai_decref(xs);
    if (pred) kai_decref(pred);
    return result;
}

static KaiValue *kai_prelude_reduce(KaiValue *xs, KaiValue *init, KaiValue *f) {
    /* acc starts as the caller's `init` ref (transferred). Each
     * iteration hands acc to the closure (which consumes it) and
     * receives a freshly-owned `next` back. If `xs` is empty we
     * return init unchanged — its single ref flows out to the
     * caller. */
    KaiValue *acc = init;
    KaiValue *p = xs;
    while (p && p->tag == KAI_CONS) {
        KaiValue *args[2];
        args[0] = acc;                              /* transfer to closure */
        args[1] = kai_incref(p->as.cons.head);      /* closure consumes */
        acc = kai_apply(f, 2, args);                /* closure produces fresh acc */
        p = p->as.cons.tail;
    }
    if (xs) kai_decref(xs);
    if (f)  kai_decref(f);
    return acc;
}

static KaiValue *kai_prelude_each(KaiValue *xs, KaiValue *f) {
    KaiValue *p = xs;
    while (p && p->tag == KAI_CONS) {
        KaiValue *arg0 = kai_incref(p->as.cons.head);
        KaiValue *r = kai_apply(f, 1, &arg0);
        kai_decref(r);
        p = p->as.cons.tail;
    }
    if (xs) kai_decref(xs);
    if (f)  kai_decref(f);
    return kai_unit();
}

/* ---------- binary and unary operators ---------- */
/*
 * Linear-consumption primitives (m5.x-flip Phase 3, 2026-04-28):
 * each op reads ALL relevant fields of its arguments BEFORE decref'ing,
 * then returns a freshly-allocated result. Aliasing-safe: `kai_op_eq_v(x, x)`
 * decrefs `a` and `b` separately, but the read-then-decref ordering means
 * both reads complete on a still-alive value. This pairs with the dup
 * pass + exit drops + producer-side incref-on-extract (Steps A + B) so
 * every value is released exactly once. NOT flipped: `kai_op_field` (already
 * increfs its return; introspection rather than consumer), `kai_op_eq` (C-int
 * returning, used inside non-consuming match tests), `kai_apply` (closure
 * invocation; lifecycle managed at the call lowering).
 */

static KaiValue *kai_op_add(KaiValue *a, KaiValue *b) {
    KaiValue *r;
    if (a->tag == KAI_INT  && b->tag == KAI_INT)       r = kai_int(a->as.i + b->as.i);
    else if (a->tag == KAI_REAL && b->tag == KAI_REAL) r = kai_real(a->as.r + b->as.r);
    else { fprintf(stderr, "kai: type mismatch in +\n"); exit(1); }
    kai_decref(a); kai_decref(b);
    return r;
}

static KaiValue *kai_op_sub(KaiValue *a, KaiValue *b) {
    KaiValue *r;
    if (a->tag == KAI_INT  && b->tag == KAI_INT)       r = kai_int(a->as.i - b->as.i);
    else if (a->tag == KAI_REAL && b->tag == KAI_REAL) r = kai_real(a->as.r - b->as.r);
    else { fprintf(stderr, "kai: type mismatch in -\n"); exit(1); }
    kai_decref(a); kai_decref(b);
    return r;
}

static KaiValue *kai_op_mul(KaiValue *a, KaiValue *b) {
    KaiValue *r;
    if (a->tag == KAI_INT  && b->tag == KAI_INT)       r = kai_int(a->as.i * b->as.i);
    else if (a->tag == KAI_REAL && b->tag == KAI_REAL) r = kai_real(a->as.r * b->as.r);
    else { fprintf(stderr, "kai: type mismatch in *\n"); exit(1); }
    kai_decref(a); kai_decref(b);
    return r;
}

static KaiValue *kai_op_div(KaiValue *a, KaiValue *b) {
    KaiValue *r;
    if (a->tag == KAI_INT && b->tag == KAI_INT) {
        if (b->as.i == 0) { fprintf(stderr, "kai: divide by zero\n"); exit(1); }
        r = kai_int(a->as.i / b->as.i);
    } else if (a->tag == KAI_REAL && b->tag == KAI_REAL) {
        r = kai_real(a->as.r / b->as.r);
    } else { fprintf(stderr, "kai: type mismatch in /\n"); exit(1); }
    kai_decref(a); kai_decref(b);
    return r;
}

static KaiValue *kai_op_idiv(KaiValue *a, KaiValue *b) {
    int64_t av = 0, bv = 0;
    if      (a->tag == KAI_INT)  av = a->as.i;
    else if (a->tag == KAI_REAL) av = (int64_t) a->as.r;
    else { fprintf(stderr, "kai: type mismatch in //\n"); exit(1); }
    if      (b->tag == KAI_INT)  bv = b->as.i;
    else if (b->tag == KAI_REAL) bv = (int64_t) b->as.r;
    else { fprintf(stderr, "kai: type mismatch in //\n"); exit(1); }
    if (bv == 0) { fprintf(stderr, "kai: divide by zero\n"); exit(1); }
    KaiValue *r = kai_int(av / bv);
    kai_decref(a); kai_decref(b);
    return r;
}

static KaiValue *kai_op_mod(KaiValue *a, KaiValue *b) {
    KaiValue *r;
    if (a->tag == KAI_INT && b->tag == KAI_INT) {
        if (b->as.i == 0) { fprintf(stderr, "kai: mod by zero\n"); exit(1); }
        r = kai_int(a->as.i % b->as.i);
    } else { fprintf(stderr, "kai: type mismatch in %%\n"); exit(1); }
    kai_decref(a); kai_decref(b);
    return r;
}

static KaiValue *kai_op_lt(KaiValue *a, KaiValue *b) {
    KaiValue *r;
    if (a->tag == KAI_INT  && b->tag == KAI_INT)       r = kai_bool(a->as.i < b->as.i);
    else if (a->tag == KAI_REAL && b->tag == KAI_REAL) r = kai_bool(a->as.r < b->as.r);
    else if (a->tag == KAI_CHAR && b->tag == KAI_CHAR) r = kai_bool(a->as.c < b->as.c);
    else if (a->tag == KAI_STR  && b->tag == KAI_STR) {
        size_t n = a->as.s.len < b->as.s.len ? a->as.s.len : b->as.s.len;
        int c = memcmp(a->as.s.bytes, b->as.s.bytes, n);
        if (c != 0) r = kai_bool(c < 0);
        else        r = kai_bool(a->as.s.len < b->as.s.len);
    } else { fprintf(stderr, "kai: type mismatch in <\n"); exit(1); }
    kai_decref(a); kai_decref(b);
    return r;
}

static KaiValue *kai_op_gt(KaiValue *a, KaiValue *b) {
    KaiValue *r;
    if (a->tag == KAI_INT  && b->tag == KAI_INT)       r = kai_bool(a->as.i > b->as.i);
    else if (a->tag == KAI_REAL && b->tag == KAI_REAL) r = kai_bool(a->as.r > b->as.r);
    else if (a->tag == KAI_CHAR && b->tag == KAI_CHAR) r = kai_bool(a->as.c > b->as.c);
    else if (a->tag == KAI_STR  && b->tag == KAI_STR) {
        size_t n = a->as.s.len < b->as.s.len ? a->as.s.len : b->as.s.len;
        int c = memcmp(a->as.s.bytes, b->as.s.bytes, n);
        if (c != 0) r = kai_bool(c > 0);
        else        r = kai_bool(a->as.s.len > b->as.s.len);
    } else { fprintf(stderr, "kai: type mismatch in >\n"); exit(1); }
    kai_decref(a); kai_decref(b);
    return r;
}

/* `kai_op_le` and `kai_op_ge` are now layered over the consuming `kai_op_gt` /
 * `kai_op_lt`. The inner call consumes `a` and `b` already; we must not
 * decref them again here. The intermediate bool result is consumed
 * locally to read the inverted truth value. */
static KaiValue *kai_op_le(KaiValue *a, KaiValue *b) {
    KaiValue *g = kai_op_gt(a, b);
    KaiValue *r = kai_bool(!g->as.b);
    kai_decref(g);
    return r;
}

static KaiValue *kai_op_ge(KaiValue *a, KaiValue *b) {
    KaiValue *l = kai_op_lt(a, b);
    KaiValue *r = kai_bool(!l->as.b);
    kai_decref(l);
    return r;
}

/* `kai_op_eq` does NOT consume — it is the C-int returning equality used by
 * pattern tests and other non-consuming sites. `kai_op_eq_v` / `kai_op_ne_v`
 * are the value-level wrappers used for `==` / `!=` expressions; those
 * DO consume per the m5.x flip. Self-aliasing (`kai_op_eq_v(x, x)`) is safe
 * because `kai_op_eq` reads both pointers before either decref runs.  */
static KaiValue *kai_op_eq_v(KaiValue *a, KaiValue *b) {
    KaiValue *r = kai_bool(kai_op_eq(a, b));
    kai_decref(a); kai_decref(b);
    return r;
}

static KaiValue *kai_op_ne_v(KaiValue *a, KaiValue *b) {
    KaiValue *r = kai_bool(!kai_op_eq(a, b));
    kai_decref(a); kai_decref(b);
    return r;
}

/* `kai_op_pow_int(a, b)` — integer-exponent power for the `^` operator.
 * Lives in runtime so the operator works without requiring the
 * Numeric protocol in scope. The unit of a dimensioned operand is
 * metadata on the type only; the runtime value carries no unit, so
 * the same code path serves `Real` and `Real<u>` alike.
 *
 * For Int: negative exponents truncate to 0 (no integer reciprocal).
 * For Real: negative exponents compute `1.0 / base^|e|` so unit
 * lifting at the type level (`r : Real<m>`, `r ^ -1 : Real<m^-1>`)
 * matches a meaningful runtime result. This is intentionally more
 * lenient than the `Numeric.pow_int` stdlib impl, which clamps
 * negatives to 0; the discrepancy is acceptable because `^`
 * dispatches through this helper, never through the protocol. */
static KaiValue *kai_op_pow_int(KaiValue *a, KaiValue *b) {
    if (b->tag != KAI_INT) {
        fprintf(stderr, "kai: type mismatch in ^ (exponent must be Int)\n"); exit(1);
    }
    int64_t e = b->as.i;
    KaiValue *r;
    if (a->tag == KAI_INT) {
        if (e < 0) { r = kai_int(0); }
        else {
            int64_t base = a->as.i;
            int64_t acc = 1;
            for (int64_t i = 0; i < e; i++) { acc *= base; }
            r = kai_int(acc);
        }
    } else if (a->tag == KAI_REAL) {
        double base = a->as.r;
        int64_t k = e < 0 ? -e : e;
        double acc = 1.0;
        for (int64_t i = 0; i < k; i++) { acc *= base; }
        if (e < 0) { acc = 1.0 / acc; }
        r = kai_real(acc);
    } else { fprintf(stderr, "kai: type mismatch in ^\n"); exit(1); }
    kai_decref(a); kai_decref(b);
    return r;
}

static KaiValue *kai_op_neg(KaiValue *a) {
    KaiValue *r;
    if (a->tag == KAI_INT)       r = kai_int(-a->as.i);
    else if (a->tag == KAI_REAL) r = kai_real(-a->as.r);
    else { fprintf(stderr, "kai: type mismatch in unary -\n"); exit(1); }
    kai_decref(a);
    return r;
}

static KaiValue *kai_op_boolnot(KaiValue *a) {
    KaiValue *r;
    if (a->tag == KAI_BOOL) r = kai_bool(!a->as.b);
    else { fprintf(stderr, "kai: type mismatch in `not`\n"); exit(1); }
    kai_decref(a);
    return r;
}

/* `kai_op_truthy` is a non-consuming C-int predicate used inside
 * `if (kai_op_truthy(...))`, ternary lowerings of `if`/`and`/`or`, and
 * `kai_assert_check`. It is intentionally NOT flipped under the
 * m5.x runtime flip: the LLVM short-circuit lowering's phi node
 * returns `lhs` itself in the early-exit branch, so consuming `lhs`
 * inside the truthiness probe would alias-free a value still
 * referenced downstream. The emitted-C path leaks the temporary
 * argument; tracked in issue #82 as future cleanup (predicate
 * consumes + emit-side incref before short-circuit). */
static int kai_op_truthy(KaiValue *v) {
    return v && v->tag == KAI_BOOL && v->as.b;
}

/* ---------- range construction ---------- */

static KaiValue *kai_range(KaiValue *from, KaiValue *to) {
    int64_t f = from->as.i, t = to->as.i;
    KaiValue *acc = kai_nil();
    for (int64_t i = t; i >= f; --i) acc = kai_cons(kai_int(i), acc);
    return acc;
}

static KaiValue *kai_range_step(KaiValue *from, KaiValue *to, KaiValue *step) {
    int64_t f = from->as.i, t = to->as.i, s = step->as.i;
    if (s == 0) { fprintf(stderr, "kai: zero step in range\n"); exit(1); }
    int64_t *buf = NULL;
    size_t cap = 0, n = 0;
    if (s > 0) {
        for (int64_t i = f; i <= t; i += s) {
            if (n == cap) { cap = cap ? cap * 2 : 16; buf = (int64_t *) realloc(buf, cap * sizeof(int64_t)); }
            buf[n++] = i;
        }
    } else {
        for (int64_t i = f; i >= t; i += s) {
            if (n == cap) { cap = cap ? cap * 2 : 16; buf = (int64_t *) realloc(buf, cap * sizeof(int64_t)); }
            buf[n++] = i;
        }
    }
    KaiValue *acc = kai_nil();
    for (size_t i = n; i > 0;) { --i; acc = kai_cons(kai_int(buf[i]), acc); }
    free(buf);
    return acc;
}

/* ---------- prelude: args (set by generated `int main`) ---------- */

static int          kai_g_argc = 0;
static char       **kai_g_argv = NULL;

static void kai_set_args(int argc, char **argv) {
    kai_g_argc = argc;
    kai_g_argv = argv;
    /* Lazy registration: kai_set_args runs once at program start from
       the emitted main wrapper. atexit fires only when the env var
       gates a report, so the side effect is harmless when tracing is
       off. Keeps the emitter unchanged — no new emit site needed. */
    kai_rc_register_once();
}

static KaiValue *kai_prelude_args(void) {
    KaiValue *acc = kai_nil();
    for (int i = kai_g_argc - 1; i >= 1; --i) {
        acc = kai_cons(kai_str(kai_g_argv[i]), acc);
    }
    return acc;
}

/* Issue #127: argv[0] as a kaikai String. Mirrors `kai_prelude_args`
 * in routing through the `kai_g_argv` snapshot installed by
 * `kai_set_args` at process entry. Returns the empty string when
 * argv was never captured (unit tests linking the runtime without
 * a generated `int main` wrapper) — leaves the surface total. */
static KaiValue *kai_prelude_program_name(void) {
    if (kai_g_argv == NULL || kai_g_argv[0] == NULL) return kai_str("");
    return kai_str(kai_g_argv[0]);
}

/* ---------- prelude: mailbox runtime (m8 #7) ---------- */

/* User code reaches the mailbox runtime through these prelude
 * functions. They are wrapped in stdlib/actor.kai's `with_mailbox`
 * helper, which also installs the user-facing Actor[Msg] handler.
 * The polymorphic surface uses Nothing → TyAny so a single set of
 * runtime entries serves every Msg type. */

static KaiValue *kai_prelude_mailbox_alloc(void) {
    return kai_pid_value(kai_mailbox_alloc());
}

/* Tier 2 spawn_actor — allocate a mailbox without stamping any
 * owner. Pair with kai_prelude_mailbox_assign_owner to wire the
 * mailbox onto the spawned fiber before its body runs. */
static KaiValue *kai_prelude_mailbox_alloc_unowned(void) {
    return kai_pid_value(kai_mailbox_alloc_unowned());
}

/* Tier 2 spawn_actor — set `pid->as.mb->owner_fiber = fiber->as.fib`
 * AND `fiber->as.fib->mailbox = pid->as.mb`. Used after
 * kai_prelude_mailbox_alloc_unowned + fiber_spawn so the spawned
 * fiber owns the mailbox for monitor / link / trap-exit lookups.
 *
 * Safe under the cooperative scheduler because fiber_spawn enqueues
 * the spawned fiber but does not yield — the parent runs through to
 * this assign call before the spawned trampoline gets the CPU. */
static KaiValue *kai_prelude_mailbox_assign_owner(KaiValue *pid, KaiValue *fiber) {
    if (pid && pid->tag == KAI_PID && pid->as.mb &&
        fiber && fiber->tag == KAI_FIBER && fiber->as.fib) {
        pid->as.mb->owner_fiber = fiber->as.fib;
        fiber->as.fib->mailbox  = pid->as.mb;
    }
    /* m5.x flip Phase 3 closeout (issue #82): consume input refs. */
    if (pid)   kai_decref(pid);
    if (fiber) kai_decref(fiber);
    return kai_unit();
}

static KaiValue *kai_prelude_mailbox_alloc_bounded(KaiValue *cap, KaiValue *overflow) {
    int c = (cap && cap->tag == KAI_INT) ? (int) cap->as.i : 0;
    int o = (overflow && overflow->tag == KAI_INT) ? (int) overflow->as.i : 0;
    KaiValue *r = kai_pid_value(kai_mailbox_alloc_bounded(c, o));
    /* m5.x flip Phase 3 closeout (issue #82): consume input refs. */
    if (cap)      kai_decref(cap);
    if (overflow) kai_decref(overflow);
    return r;
}

static KaiValue *kai_prelude_mailbox_send(KaiValue *pid, KaiValue *msg) {
    if (!pid || pid->tag != KAI_PID || !pid->as.mb) {
        fprintf(stderr, "kai: mailbox_send: argument is not a Pid\n");
        exit(1);
    }
    /* m5.x flip Phase 3 closeout (issue #82): transfer the caller's
     * `msg` ref directly into the mailbox (kai_mailbox_push takes
     * ownership) and consume the `pid` ref. Pre-fix the helper did
     * `kai_incref(msg)` then dropped the caller's ref on the floor. */
    kai_mailbox_push(pid->as.mb, msg);
    kai_decref(pid);
    return kai_unit();
}

static KaiValue *kai_prelude_mailbox_recv(KaiValue *pid) {
    if (!pid || pid->tag != KAI_PID || !pid->as.mb) {
        fprintf(stderr, "kai: mailbox_recv: argument is not a Pid\n");
        exit(1);
    }
    /* kai_mailbox_pop returns the stored ref (mailbox transferred its
     * ownership to the caller). Consume the input `pid` ref so the
     * helper is callee-consumes-clean. */
    KaiValue *msg = kai_mailbox_pop(pid->as.mb);
    kai_decref(pid);
    return msg;
}

/* Free the mailbox attached to a Pid. Called by `with_mailbox` when
 * the scope exits; the Pid value itself is RC-managed independently. */
static KaiValue *kai_prelude_mailbox_free(KaiValue *pid) {
    if (pid && pid->tag == KAI_PID && pid->as.mb) {
        kai_mailbox_free(pid->as.mb);
        pid->as.mb = NULL;
    }
    /* m5.x flip Phase 3 closeout (issue #82): consume input ref. */
    if (pid) kai_decref(pid);
    return kai_unit();
}

/* ---------- prelude: file io ---------- */

static KaiValue *kai_prelude_read_file(KaiValue *path) {
    KaiValue *r = NULL;
    if (!path || path->tag != KAI_STR) {
        KaiValue *msg = kai_str("read_file: argument is not a String");
        r = kai_variant(0, "Err", 1, &msg);
    } else {
        char pbuf[4096];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        FILE *fp = fopen(pbuf, "rb");
        if (!fp) {
            KaiValue *msg = kai_str("read_file: cannot open file");
            r = kai_variant(0, "Err", 1, &msg);
        } else if (fseek(fp, 0, SEEK_END) != 0) {
            fclose(fp);
            KaiValue *msg = kai_str("read_file: seek failed");
            r = kai_variant(0, "Err", 1, &msg);
        } else {
            long n = ftell(fp);
            if (n < 0) {
                fclose(fp);
                KaiValue *msg = kai_str("read_file: tell failed");
                r = kai_variant(0, "Err", 1, &msg);
            } else if (fseek(fp, 0, SEEK_SET) != 0) {
                fclose(fp);
                KaiValue *msg = kai_str("read_file: rewind failed");
                r = kai_variant(0, "Err", 1, &msg);
            } else {
                KaiValue *v = kai_alloc(KAI_STR);
                v->as.s.len = (size_t) n;
                v->as.s.bytes = (char *) malloc((size_t) n + 1);
                if (!v->as.s.bytes) { fclose(fp); fprintf(stderr, "kai: out of memory\n"); exit(1); }
                size_t got = fread(v->as.s.bytes, 1, (size_t) n, fp);
                fclose(fp);
                v->as.s.bytes[got] = '\0';
                v->as.s.len = got;
                r = kai_variant(0, "Ok", 1, &v);
            }
        }
    }
    if (path) kai_decref(path);
    return r;
}

static KaiValue *kai_prelude_write_file(KaiValue *path, KaiValue *content) {
    KaiValue *r = NULL;
    if (!path || path->tag != KAI_STR) {
        KaiValue *msg = kai_str("write_file: path is not a String");
        r = kai_variant(0, "Err", 1, &msg);
    } else if (!content || content->tag != KAI_STR) {
        KaiValue *msg = kai_str("write_file: content is not a String");
        r = kai_variant(0, "Err", 1, &msg);
    } else {
        char pbuf[4096];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        FILE *fp = fopen(pbuf, "wb");
        if (!fp) {
            KaiValue *msg = kai_str("write_file: cannot open file");
            r = kai_variant(0, "Err", 1, &msg);
        } else {
            size_t wrote = fwrite(content->as.s.bytes, 1, content->as.s.len, fp);
            fclose(fp);
            if (wrote != content->as.s.len) {
                KaiValue *msg = kai_str("write_file: short write");
                r = kai_variant(0, "Err", 1, &msg);
            } else {
                KaiValue *u = kai_unit();
                r = kai_variant(0, "Ok", 1, &u);
            }
        }
    }
    if (path)    kai_decref(path);
    if (content) kai_decref(content);
    return r;
}

static KaiValue *kai_prelude_read_line(void) {
    size_t cap = 128, n = 0;
    char *buf = (char *) malloc(cap);
    if (!buf) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    int ch;
    while ((ch = fgetc(stdin)) != EOF && ch != '\n') {
        if (n + 1 >= cap) { cap *= 2; buf = (char *) realloc(buf, cap); }
        buf[n++] = (char) ch;
    }
    if (ch == EOF && n == 0) {
        free(buf);
        KaiValue *msg = kai_str("read_line: end of input");
        return kai_variant(0, "Err", 1, &msg);
    }
    KaiValue *s = kai_str_from_bytes(buf, n);
    free(buf);
    return kai_variant(0, "Ok", 1, &s);
}

/* ---------- prelude: parsing and string helpers ---------- */

/* m5.x flip Phase 3 closeout (Perceus Tier 2 audit, 2026-04-29):
 * the next 8 helpers consume their args linearly. Result computed
 * first, args decref'd before the constructor allocates. */
static KaiValue *kai_prelude_string_to_int(KaiValue *s) {
    int ok = 0;
    int64_t value = 0;
    if (s && s->tag == KAI_STR && s->as.s.len > 0 && s->as.s.len < 64) {
        char buf[64];
        memcpy(buf, s->as.s.bytes, s->as.s.len);
        buf[s->as.s.len] = '\0';
        char *end = NULL;
        long long v = strtoll(buf, &end, 10);
        if (end && *end == '\0' && end != buf) {
            value = (int64_t) v;
            ok = 1;
        }
    }
    if (s) kai_decref(s);
    if (ok) {
        KaiValue *iv = kai_int(value);
        return kai_variant(0, "Some", 1, &iv);
    }
    return kai_variant(0, "None", 0, NULL);
}

static KaiValue *kai_prelude_string_to_real(KaiValue *s) {
    int ok = 0;
    double value = 0.0;
    if (s && s->tag == KAI_STR && s->as.s.len > 0 && s->as.s.len < 64) {
        char buf[64];
        memcpy(buf, s->as.s.bytes, s->as.s.len);
        buf[s->as.s.len] = '\0';
        char *end = NULL;
        double v = strtod(buf, &end);
        if (end && *end == '\0' && end != buf) {
            value = v;
            ok = 1;
        }
    }
    if (s) kai_decref(s);
    if (ok) {
        KaiValue *rv = kai_real(value);
        return kai_variant(0, "Some", 1, &rv);
    }
    return kai_variant(0, "None", 0, NULL);
}

static KaiValue *kai_prelude_char_at(KaiValue *s, KaiValue *i) {
    int ok = 0;
    uint32_t value = 0;
    if (s && s->tag == KAI_STR && i && i->tag == KAI_INT) {
        int64_t idx = i->as.i;
        if (idx >= 0 && (size_t) idx < s->as.s.len) {
            /* Byte-at semantics for now; multi-byte UTF-8 can return
             * surrogate-like codepoints once we need them. Stage 0
             * keeps it simple. */
            value = (uint32_t)(unsigned char) s->as.s.bytes[idx];
            ok = 1;
        }
    }
    if (s) kai_decref(s);
    if (i) kai_decref(i);
    if (ok) {
        KaiValue *cv = kai_char(value);
        return kai_variant(0, "Some", 1, &cv);
    }
    return kai_variant(0, "None", 0, NULL);
}

static KaiValue *kai_prelude_string_split(KaiValue *s, KaiValue *sep) {
    KaiValue *acc;
    if (!s || s->tag != KAI_STR) {
        acc = kai_nil();
    } else if (!sep || sep->tag != KAI_STR || sep->as.s.len == 0) {
        /* No separator → singleton list with own copy of `s`. */
        acc = kai_cons(kai_str_from_bytes(s->as.s.bytes, s->as.s.len), kai_nil());
    } else {
        const char *p = s->as.s.bytes;
        size_t slen = s->as.s.len;
        const char *sp = sep->as.s.bytes;
        size_t seplen = sep->as.s.len;
        /* Collect pieces into a temp array then fold right into a cons list. */
        size_t cap = 8, n = 0;
        struct { const char *b; size_t l; } *pieces = malloc(cap * sizeof(*pieces));
        size_t i = 0;
        size_t last = 0;
        while (i + seplen <= slen) {
            if (memcmp(p + i, sp, seplen) == 0) {
                if (n == cap) { cap *= 2; pieces = realloc(pieces, cap * sizeof(*pieces)); }
                pieces[n].b = p + last;
                pieces[n].l = i - last;
                n++;
                i += seplen;
                last = i;
            } else {
                i++;
            }
        }
        if (n == cap) { cap *= 2; pieces = realloc(pieces, cap * sizeof(*pieces)); }
        pieces[n].b = p + last;
        pieces[n].l = slen - last;
        n++;
        acc = kai_nil();
        for (size_t k = n; k > 0;) {
            --k;
            acc = kai_cons(kai_str_from_bytes(pieces[k].b, pieces[k].l), acc);
        }
        free(pieces);
    }
    if (s) kai_decref(s);
    if (sep) kai_decref(sep);
    return acc;
}

static KaiValue *kai_prelude_string_slice(KaiValue *s, KaiValue *from, KaiValue *len) {
    KaiValue *r;
    if (!s || s->tag != KAI_STR) {
        r = kai_str("");
    } else {
        int64_t f = (from && from->tag == KAI_INT) ? from->as.i : 0;
        int64_t l = (len  && len->tag  == KAI_INT) ? len->as.i  : 0;
        if (f < 0) f = 0;
        if (l < 0) l = 0;
        if ((size_t) f > s->as.s.len) f = (int64_t) s->as.s.len;
        size_t avail = s->as.s.len - (size_t) f;
        size_t take  = ((size_t) l > avail) ? avail : (size_t) l;
        r = kai_str_from_bytes(s->as.s.bytes + f, take);
    }
    if (s)    kai_decref(s);
    if (from) kai_decref(from);
    if (len)  kai_decref(len);
    return r;
}

static KaiValue *kai_prelude_char_to_int(KaiValue *c) {
    int64_t value = (c && c->tag == KAI_CHAR) ? (int64_t) c->as.c : 0;
    if (c) kai_decref(c);
    return kai_int(value);
}

static KaiValue *kai_prelude_int_to_char(KaiValue *n) {
    uint32_t value = (n && n->tag == KAI_INT) ? (uint32_t) n->as.i : 0;
    if (n) kai_decref(n);
    return kai_char(value);
}

static KaiValue *kai_prelude_string_contains(KaiValue *s, KaiValue *sub) {
    int yes = 0;
    if (s && s->tag == KAI_STR && sub && sub->tag == KAI_STR) {
        if (sub->as.s.len == 0) {
            yes = 1;
        } else if (sub->as.s.len <= s->as.s.len) {
            for (size_t i = 0; i + sub->as.s.len <= s->as.s.len; ++i) {
                if (memcmp(s->as.s.bytes + i, sub->as.s.bytes, sub->as.s.len) == 0) {
                    yes = 1;
                    break;
                }
            }
        }
    }
    if (s)   kai_decref(s);
    if (sub) kai_decref(sub);
    return kai_bool(yes);
}

/* ---------- prelude thunks for first-class function refs ---------- */

static KaiValue *_kai_prelude_print_thunk(KaiValue *s, KaiValue **a, int n)          { (void) s; (void) n; return kai_prelude_print(a[0]); }
static KaiValue *_kai_prelude_eprint_thunk(KaiValue *s, KaiValue **a, int n)         { (void) s; (void) n; return kai_prelude_eprint(a[0]); }
static KaiValue *_kai_prelude_panic_thunk(KaiValue *s, KaiValue **a, int n)          { (void) s; (void) n; return kai_prelude_panic(a[0]); }
static KaiValue *_kai_prelude_exit_thunk(KaiValue *s, KaiValue **a, int n)           { (void) s; (void) n; return kai_prelude_exit(a[0]); }
static KaiValue *_kai_prelude_int_to_string_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_prelude_int_to_string(a[0]); }
static KaiValue *_kai_prelude_real_to_string_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_real_to_string(a[0]); }
static KaiValue *_kai_prelude_int_to_real_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_int_to_real(a[0]); }
static KaiValue *_kai_prelude_real_to_int_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_real_to_int(a[0]); }
static KaiValue *_kai_prelude_string_length_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_prelude_string_length(a[0]); }
static KaiValue *_kai_prelude_string_concat_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_prelude_string_concat(a[0], a[1]); }
static KaiValue *_kai_prelude_string_concat_all_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_string_concat_all(a[0]); }
static KaiValue *_kai_prelude_string_join_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_prelude_string_join(a[0], a[1]); }
static KaiValue *_kai_prelude_array_make_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_array_make(a[0], a[1]); }
static KaiValue *_kai_prelude_array_length_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_prelude_array_length(a[0]); }
static KaiValue *_kai_prelude_array_get_thunk(KaiValue *s, KaiValue **a, int n)     { (void) s; (void) n; return kai_prelude_array_get(a[0], a[1]); }
static KaiValue *_kai_prelude_array_set_thunk(KaiValue *s, KaiValue **a, int n)     { (void) s; (void) n; return kai_prelude_array_set(a[0], a[1], a[2]); }
static KaiValue *_kai_prelude_array_grow_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_array_grow(a[0], a[1], a[2]); }
static KaiValue *_kai_prelude_list_length_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_list_length(a[0]); }
static KaiValue *_kai_prelude_list_append_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_list_append(a[0], a[1]); }
static KaiValue *_kai_prelude_list_reverse_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_prelude_list_reverse(a[0]); }
static KaiValue *_kai_prelude_map_thunk(KaiValue *s, KaiValue **a, int n)            { (void) s; (void) n; return kai_prelude_map(a[0], a[1]); }
static KaiValue *_kai_prelude_flat_map_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_prelude_flat_map(a[0], a[1]); }
static KaiValue *_kai_prelude_filter_thunk(KaiValue *s, KaiValue **a, int n)         { (void) s; (void) n; return kai_prelude_filter(a[0], a[1]); }
static KaiValue *_kai_prelude_reduce_thunk(KaiValue *s, KaiValue **a, int n)         { (void) s; (void) n; return kai_prelude_reduce(a[0], a[1], a[2]); }
static KaiValue *_kai_prelude_each_thunk(KaiValue *s, KaiValue **a, int n)           { (void) s; (void) n; return kai_prelude_each(a[0], a[1]); }
static KaiValue *_kai_prelude_args_thunk(KaiValue *s, KaiValue **a, int n)           { (void) s; (void) a; (void) n; return kai_prelude_args(); }
static KaiValue *_kai_prelude_program_name_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) a; (void) n; return kai_prelude_program_name(); }
static KaiValue *_kai_prelude_read_file_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_prelude_read_file(a[0]); }
static KaiValue *_kai_prelude_write_file_thunk(KaiValue *s, KaiValue **a, int n)     { (void) s; (void) n; return kai_prelude_write_file(a[0], a[1]); }
static KaiValue *_kai_prelude_read_line_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) a; (void) n; return kai_prelude_read_line(); }
static KaiValue *_kai_prelude_string_to_int_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_prelude_string_to_int(a[0]); }
static KaiValue *_kai_prelude_string_to_real_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_string_to_real(a[0]); }
static KaiValue *_kai_prelude_char_at_thunk(KaiValue *s, KaiValue **a, int n)        { (void) s; (void) n; return kai_prelude_char_at(a[0], a[1]); }
static KaiValue *_kai_prelude_string_split_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_prelude_string_split(a[0], a[1]); }
static KaiValue *_kai_prelude_string_contains_thunk(KaiValue *s, KaiValue **a, int n){ (void) s; (void) n; return kai_prelude_string_contains(a[0], a[1]); }
static KaiValue *_kai_prelude_string_slice_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_prelude_string_slice(a[0], a[1], a[2]); }
static KaiValue *_kai_prelude_char_to_int_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_char_to_int(a[0]); }
static KaiValue *_kai_prelude_int_to_char_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_int_to_char(a[0]); }
static KaiValue *_kai_prelude_mailbox_alloc_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) a; (void) n; return kai_prelude_mailbox_alloc(); }
static KaiValue *_kai_prelude_mailbox_alloc_bounded_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_mailbox_alloc_bounded(a[0], a[1]); }
static KaiValue *_kai_prelude_mailbox_send_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_prelude_mailbox_send(a[0], a[1]); }
static KaiValue *_kai_prelude_mailbox_recv_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_prelude_mailbox_recv(a[0]); }
static KaiValue *_kai_prelude_mailbox_free_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_prelude_mailbox_free(a[0]); }

/* ---------- test harness hooks (used by --test runs) ---------- */

static int         kai_test_count_total  = 0;
static int         kai_test_count_passed = 0;
static const char *kai_test_current      = NULL;
static jmp_buf     kai_test_jmp;
static int         kai_test_in_progress  = 0;

static void kai_test_begin(const char *desc) {
    kai_test_count_total++;
    kai_test_current = desc;
}

static void kai_test_pass(void) {
    kai_test_count_passed++;
    fprintf(stderr, "  ok   %s\n", kai_test_current ? kai_test_current : "");
}

static void kai_test_fail(const char *desc, const char *msg) {
    fprintf(stderr, "  FAIL %s : %s\n",
            desc ? desc : "",
            msg  ? msg  : "assertion failed");
}

static int kai_test_summary(void) {
    fprintf(stderr, "\n%d/%d tests passed\n",
            kai_test_count_passed, kai_test_count_total);
    return (kai_test_count_passed == kai_test_count_total) ? 0 : 1;
}

/* ---------- bench harness hooks (used by --bench runs) ----------
 * bench v1 (issue #40): mean ns/iter only, N fixed at 1000. The
 * emitted main wrapper calls every _kai_bench_<id> in sequence and
 * returns kai_bench_summary().
 *
 * Median + MAD-based outlier detection deferred to v1.x; selfhost-
 * bench (the compiler measuring itself) deferred to v1.y. See
 * issue #40 for the split plan.
 */

#define KAI_BENCH_ITERS 1000

static int kai_bench_count_total = 0;

static long long kai_bench_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

static void kai_bench_report(const char *desc, long long total_ns, int iters) {
    /* Mean ns/iter only. desc is the raw source span of the string
       literal (quotes included) — fprintf passes it through verbatim,
       which is consistent with how kai_test_pass renders kai_test_current. */
    long long per = (iters > 0) ? (total_ns / (long long)iters) : 0;
    fprintf(stderr, "  %s: %d iter / %lld ns/iter\n",
            desc ? desc : "(unnamed)", iters, per);
    kai_bench_count_total++;
}

static int kai_bench_summary(void) {
    fprintf(stderr, "\n%d benches\n", kai_bench_count_total);
    return 0;
}

/* ---------- check harness hooks (used by --check runs) ----------
 * check v1 (issue #44): property-based testing. Each `check "..."
 * [with p: T, ...] { body }` block is executed KAI_CHECK_ITERS
 * times. Each iteration generates random values for the with-clause
 * params via kai_arbitrary_<T>() and runs the body. The body must
 * produce a Bool — true means the property held for that input,
 * false records a counterexample and exits early.
 *
 * v1 reports the FIRST counterexample only (no shrinking). Shrinking
 * + median-based outlier-counterexample selection deferred to v1.x;
 * protocol-based generators (impl Arbitrary for T) deferred to v2
 * post-protocols maturity (see issue #44 split plan).
 */

#define KAI_CHECK_ITERS 100

static int kai_check_count_total = 0;
static int kai_check_count_passed = 0;
static const char *kai_check_current_desc = NULL;

#define KAI_CHECK_CX_BUF 1024
static char kai_check_cx_buf[KAI_CHECK_CX_BUF];
static size_t kai_check_cx_len = 0;

/* xorshift64* PRNG. Seeded once per process with a constant so
   counterexample reports are reproducible run-to-run; reseed via
   KAI_CHECK_SEED env var lands in v1.x. */
static uint64_t kai_check_seed = 0xC0DECAFE12345678ULL;

static uint64_t kai_check_rand_u64(void) {
    kai_check_seed ^= kai_check_seed >> 12;
    kai_check_seed ^= kai_check_seed << 25;
    kai_check_seed ^= kai_check_seed >> 27;
    return kai_check_seed * 0x2545F4914F6CDD1DULL;
}

static void kai_check_cx_reset(void) {
    kai_check_cx_buf[0] = '\0';
    kai_check_cx_len = 0;
}

static void kai_check_cx_append_raw(const char *s, size_t n) {
    if (!s || n == 0) return;
    if (kai_check_cx_len + n + 1 >= KAI_CHECK_CX_BUF) {
        n = KAI_CHECK_CX_BUF - 1 - kai_check_cx_len;
        if (n == 0) return;
    }
    memcpy(kai_check_cx_buf + kai_check_cx_len, s, n);
    kai_check_cx_len += n;
    kai_check_cx_buf[kai_check_cx_len] = '\0';
}

static void kai_check_cx_append(const char *s) {
    if (!s) return;
    kai_check_cx_append_raw(s, strlen(s));
}

/* Records "name=<repr>" in the counterexample buffer. Called by the
   emitted check fn after each kai_arbitrary_<T> generator inside the
   per-iter loop, before evaluating the body, so that on failure the
   buffer already holds every input the predicate saw. v repr uses
   kai_to_string (borrow). */
static void kai_check_record_param(const char *name, KaiValue *v) {
    if (kai_check_cx_len > 0) kai_check_cx_append(", ");
    kai_check_cx_append(name);
    kai_check_cx_append("=");
    KaiValue *s = kai_to_string(v);
    if (s && s->tag == KAI_STR) {
        kai_check_cx_append_raw(s->as.s.bytes, s->as.s.len);
    }
    kai_decref(s);
}

static void kai_check_begin(const char *desc) {
    kai_check_count_total++;
    kai_check_current_desc = desc;
}

static void kai_check_pass(int iters) {
    kai_check_count_passed++;
    fprintf(stderr, "  %s: %d iter, OK\n",
            kai_check_current_desc ? kai_check_current_desc : "(unnamed)",
            iters);
}

static void kai_check_fail(int iter_at) {
    fprintf(stderr, "  %s: counterexample at iter %d: %s\n",
            kai_check_current_desc ? kai_check_current_desc : "(unnamed)",
            iter_at, kai_check_cx_buf);
}

static int kai_check_summary(void) {
    fprintf(stderr, "\n%d/%d checks passed\n",
            kai_check_count_passed, kai_check_count_total);
    return (kai_check_count_passed == kai_check_count_total) ? 0 : 1;
}

/* Intrinsic generators (issue #44 path b). Each returns a fresh
   KaiValue * with rc=1 ready for the body to consume or drop. The
   ranges are kept small so counterexamples are human-readable
   (Int in [-50, 50], String len in [0, 10], list len in [0, 7]). */

static KaiValue *kai_arbitrary_int(void) {
    int64_t r = (int64_t)(kai_check_rand_u64() % 101) - 50;
    return kai_int(r);
}

static KaiValue *kai_arbitrary_bool(void) {
    return kai_bool((int)(kai_check_rand_u64() & 1));
}

static KaiValue *kai_arbitrary_char(void) {
    /* printable ASCII ('!' .. '~') */
    uint32_t c = (uint32_t)(0x21 + (kai_check_rand_u64() % 0x5E));
    return kai_char(c);
}

static KaiValue *kai_arbitrary_string(void) {
    int len = (int)(kai_check_rand_u64() % 11);
    char buf[16];
    for (int i = 0; i < len; i++) {
        buf[i] = (char)(0x21 + (kai_check_rand_u64() % 0x5E));
    }
    return kai_str_from_bytes(buf, (size_t) len);
}

/* List generators by element kind. v1 supports primitive elements
   only. Structural sum/record/list-of-non-primitive lands in v1.x
   together with the typer-side derivation. */
#define KAI_DEFINE_ARBITRARY_LIST(NAME, ELEM)                          \
    static KaiValue *NAME(void) {                                      \
        int len = (int)(kai_check_rand_u64() % 8);                     \
        KaiValue *acc = kai_nil();                                     \
        for (int i = 0; i < len; i++) acc = kai_cons(ELEM(), acc);     \
        return acc;                                                    \
    }
KAI_DEFINE_ARBITRARY_LIST(kai_arbitrary_list_int,    kai_arbitrary_int)
KAI_DEFINE_ARBITRARY_LIST(kai_arbitrary_list_bool,   kai_arbitrary_bool)
KAI_DEFINE_ARBITRARY_LIST(kai_arbitrary_list_char,   kai_arbitrary_char)
KAI_DEFINE_ARBITRARY_LIST(kai_arbitrary_list_string, kai_arbitrary_string)
#undef KAI_DEFINE_ARBITRARY_LIST

/* Runtime-aware assert: called from every emitted `assert`. Inside an
   active test (kai_test_in_progress) it prints a failure and longjmps
   back to the test harness so the next test can run. Otherwise it
   aborts the process via kai_prelude_panic, matching stage 0's
   non-test-mode behaviour. */
static void kai_assert_check(KaiValue *cond, const char *msg) {
    int ok = kai_op_truthy(cond);
    kai_decref(cond);
    if (ok) return;
    if (kai_test_in_progress) {
        kai_test_fail(kai_test_current, msg ? msg : "assertion failed");
        longjmp(kai_test_jmp, 1);
    } else {
        kai_prelude_panic(kai_str(msg ? msg : "assertion failed"));
    }
}

/* =================================================================
 * Effects: handler-stack runtime (m7a #5)
 * =================================================================
 *
 * Per docs/effects-impl.md §*Handler-stack runtime*. Each fiber
 * owns a stack of Evidence nodes; m7a operates with a single
 * implicit fiber (kai_main_fiber), but the layout is per-fiber so
 * m8's real scheduler can introduce fibers without refactoring
 * this part of the runtime (Doc C OQ #3, decided).
 *
 * The Evidence node itself is intentionally untyped on the
 * `handler` slot: each effect's compiled `Ev<Eff>` struct lives
 * elsewhere; the call site casts back to *Ev<Eff> at the point
 * where the effect name is statically known.
 *
 * m7a #6 will wire `handle { ... } with Eff { ... }` lowering to
 * call kai_evidence_push / kai_evidence_pop, and `Eff.op(args)` to
 * call kai_evidence_lookup.
 */

/* m7a #6a: handler-id stamped onto each EvE struct at handle entry,
 * and copied into the continuation closure for one-shot diagnostics
 * (Doc C §*resume representation*). The id is cosmetic — the
 * status byte in the continuation is the real one-shot check.
 * Monotonic + unique-per-process is enough for v1; m8 may revisit
 * if cross-fiber id collisions become a debugging concern. */
typedef unsigned long long KaiHandlerId;

static KaiHandlerId kai_next_handler_id = 1;

static KaiHandlerId kai_fresh_handler_id(void) {
    return kai_next_handler_id++;
}

/* m7a #6d: continuation closure, stack-allocated at every op call
 * site (Doc C §*resume representation* one-shot path). `status` is
 * the one-shot check: the first call to `resume` flips it from
 * UNRESUMED to RESUMED, the second call aborts with a runtime
 * diagnostic. `fn` + `env` together name the rest of the
 * computation; m7a #6d ships only the identity continuation
 * (kai_cont_identity), so resume is effectively a tail return of
 * its argument. The full CPS reification (the rest of the caller's
 * body as a separately emitted function) is a later milestone. */
typedef enum {
    KAI_CONT_UNRESUMED = 0,
    KAI_CONT_RESUMED   = 1
} KaiContStatus;

typedef struct KaiCont KaiCont;
struct KaiCont {
    KaiContStatus  status;
    void          *env;
    KaiValue     *(*fn)(void *env, KaiValue *v);
    KaiHandlerId   handler_id;
};

/* Identity continuation: returns its argument unchanged. Until the
 * CPS transform reifies the rest of the caller's body, every op
 * call site uses this as the resume target — so the one-shot check
 * is observable but the continuation is functionally a no-op. */
static KaiValue *kai_cont_identity(void *env, KaiValue *v) {
    (void) env;
    return v;
}

static void kai_cont_init_identity(KaiCont *k, KaiHandlerId hid) {
    k->status     = KAI_CONT_UNRESUMED;
    k->env        = NULL;
    k->fn         = &kai_cont_identity;
    k->handler_id = hid;
}

/* Surface `resume(v)` lowers to this. The check + flip + tail call
 * all happen here; the clause body sees a single function call. */
static KaiValue *kai_cont_resume(KaiCont *k, KaiValue *v) {
    if (k->status != KAI_CONT_UNRESUMED) {
        fprintf(stderr,
            "kai: continuation resumed twice (handler #%llu)\n",
            (unsigned long long) k->handler_id);
        exit(1);
    }
    k->status = KAI_CONT_RESUMED;
    return k->fn(k->env, v);
}

/* m12.8 Phase 4b: split Console into atomic Stdout (print) and
 * Stderr (eprint) default handlers. Both write the string + a
 * trailing '\n' and resume with `()`. *Self is opaque to the
 * runtime because `EvStdout` / `EvStderr` are compiler-emitted
 * types — the cast happens at the assignment in the main wrapper.
 *
 * EPIPE absorption (Doc B §`Console`/Default handler) is later
 * polish — for now any write fault propagates through fputs's
 * default behaviour. The minimal-but-useful path lands first. */
static KaiValue *kai_default_stdout_print(void *self, KaiValue *s, KaiCont *k) {
    (void) self;
    if (s && s->tag == KAI_STR) {
        fwrite(s->as.s.bytes, 1, s->as.s.len, stdout);
    }
    fputc('\n', stdout);
    return kai_cont_resume(k, kai_unit());
}

static KaiValue *kai_default_stderr_eprint(void *self, KaiValue *s, KaiCont *k) {
    (void) self;
    if (s && s->tag == KAI_STR) {
        fwrite(s->as.s.bytes, 1, s->as.s.len, stderr);
    }
    fputc('\n', stderr);
    return kai_cont_resume(k, kai_unit());
}

/* m7a #7: default Fail handler. Doc B §`Fail` declares the op as
 * returning Nothing, so the handler must short-circuit — no resume,
 * no return value. The runtime writes a banner to stderr and exits
 * with status 1. Long-term Doc B prefers an unhandled-Fail compile
 * error (catalog says "no (unhandled = compile error)"); until the
 * typer's effect-row check in #8 enforces that, this default is
 * the safe fallback. */
static KaiValue *kai_default_fail_fail(void *self, KaiValue *msg, KaiCont *k) {
    (void) self;
    (void) k;
    fputs("kai: Fail.fail: ", stderr);
    if (msg && msg->tag == KAI_STR) {
        fwrite(msg->as.s.bytes, 1, msg->as.s.len, stderr);
    }
    fputc('\n', stderr);
    exit(1);
}

/* m7a #7: default Stdin handler. Doc B §`Stdin` declares
 * `read_line() : Option[String] / Fail`; m7a simplifies to
 * `: Option[String]` (no /Fail propagation yet — fread errors
 * panic the same way Console does). EOF maps to None; any byte
 * read returns Some(line) with the trailing '\n' stripped if
 * present. */
static KaiValue *kai_default_stdin_read_line(void *self, KaiCont *k) {
    (void) self;
    size_t cap = 128, n = 0;
    char *buf = (char *) malloc(cap);
    if (!buf) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    int ch;
    while ((ch = fgetc(stdin)) != EOF && ch != '\n') {
        if (n + 1 >= cap) { cap *= 2; buf = (char *) realloc(buf, cap); }
        buf[n++] = (char) ch;
    }
    if (ch == EOF && n == 0) {
        free(buf);
        return kai_cont_resume(k, kai_variant(0, "None", 0, NULL));
    }
    KaiValue *s = kai_str_from_bytes(buf, n);
    free(buf);
    KaiValue *some = kai_variant(0, "Some", 1, &s);
    return kai_cont_resume(k, some);
}

/* m7a #7: default Env handlers. `args()` reuses kai_prelude_args
 * (returns a [String] of argv[1..]); `var(name)` wraps getenv:
 * present → Some(value), absent → None. */
static KaiValue *kai_default_env_args(void *self, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_prelude_args());
}

static KaiValue *kai_default_env_var(void *self, KaiValue *name, KaiCont *k) {
    (void) self;
    if (!name || name->tag != KAI_STR) {
        return kai_cont_resume(k, kai_variant(0, "None", 0, NULL));
    }
    char nbuf[1024];
    size_t nlen = name->as.s.len < sizeof(nbuf) - 1 ? name->as.s.len : sizeof(nbuf) - 1;
    memcpy(nbuf, name->as.s.bytes, nlen);
    nbuf[nlen] = '\0';
    const char *got = getenv(nbuf);
    if (!got) return kai_cont_resume(k, kai_variant(0, "None", 0, NULL));
    KaiValue *s = kai_str(got);
    KaiValue *some = kai_variant(0, "Some", 1, &s);
    return kai_cont_resume(k, some);
}

/* Issue #127: Env write side + full block enumeration. POSIX
 * `setenv(name, value, 1)` copies its arguments into libc-owned
 * storage on both glibc and Apple libc, so the local stack buffers
 * here are safe to release on return — no buffer-ownership leak
 * across the FFI boundary. Errors lift `strerror(errno)` into a
 * fresh `Err(String)`. `vars()` walks the POSIX `environ` array,
 * splitting each `KEY=VALUE` entry into a `Pair { fst, snd }` so
 * the surface type lines up with `[(String, String)]`. */
extern char **environ;

static KaiValue *kai_default_env_set_var(void *self, KaiValue *name,
                                          KaiValue *value, KaiCont *k) {
    (void) self;
    if (!name || name->tag != KAI_STR || !value || value->tag != KAI_STR) {
        KaiValue *m = kai_str("set_var: name and value must be Strings");
        KaiValue *err = kai_variant(0, "Err", 1, &m);
        return kai_cont_resume(k, err);
    }
    char nbuf[1024];
    size_t nlen = name->as.s.len < sizeof(nbuf) - 1 ? name->as.s.len : sizeof(nbuf) - 1;
    memcpy(nbuf, name->as.s.bytes, nlen);
    nbuf[nlen] = '\0';
    /* setenv copies value too; an arbitrarily-long content string can
     * outgrow the stack buffer, so heap-dup once and free after the
     * call returns. */
    char *vbuf = (char *) malloc(value->as.s.len + 1);
    if (!vbuf) {
        KaiValue *m = kai_str("set_var: out of memory");
        KaiValue *err = kai_variant(0, "Err", 1, &m);
        return kai_cont_resume(k, err);
    }
    if (value->as.s.len > 0) memcpy(vbuf, value->as.s.bytes, value->as.s.len);
    vbuf[value->as.s.len] = '\0';
    int rc = setenv(nbuf, vbuf, 1);
    int saved_errno = errno;
    free(vbuf);
    if (rc != 0) {
        const char *msg = strerror(saved_errno);
        if (!msg) msg = "set_var failed";
        KaiValue *m = kai_str(msg);
        KaiValue *err = kai_variant(0, "Err", 1, &m);
        return kai_cont_resume(k, err);
    }
    KaiValue *u = kai_unit();
    KaiValue *ok = kai_variant(0, "Ok", 1, &u);
    return kai_cont_resume(k, ok);
}

static KaiValue *kai_default_env_unset_var(void *self, KaiValue *name, KaiCont *k) {
    (void) self;
    if (!name || name->tag != KAI_STR) {
        KaiValue *m = kai_str("unset_var: name must be a String");
        KaiValue *err = kai_variant(0, "Err", 1, &m);
        return kai_cont_resume(k, err);
    }
    char nbuf[1024];
    size_t nlen = name->as.s.len < sizeof(nbuf) - 1 ? name->as.s.len : sizeof(nbuf) - 1;
    memcpy(nbuf, name->as.s.bytes, nlen);
    nbuf[nlen] = '\0';
    if (unsetenv(nbuf) != 0) {
        const char *msg = strerror(errno);
        if (!msg) msg = "unset_var failed";
        KaiValue *m = kai_str(msg);
        KaiValue *err = kai_variant(0, "Err", 1, &m);
        return kai_cont_resume(k, err);
    }
    KaiValue *u = kai_unit();
    KaiValue *ok = kai_variant(0, "Ok", 1, &u);
    return kai_cont_resume(k, ok);
}

static KaiValue *kai_default_env_vars(void *self, KaiCont *k) {
    (void) self;
    /* Build the list head-down via list_append so the surface order
     * matches the libc enumeration order (first entry of `environ`
     * appears as head). cons-then-reverse would also work; this
     * shape mirrors how `kai_prelude_args` walks argv. */
    KaiValue *acc = kai_nil();
    int count = 0;
    for (char **ep = environ; ep && *ep; ++ep) ++count;
    for (int i = count - 1; i >= 0; --i) {
        const char *entry = environ[i];
        if (!entry) continue;
        const char *eq = strchr(entry, '=');
        size_t name_len  = eq ? (size_t) (eq - entry) : strlen(entry);
        const char *vstart = eq ? eq + 1 : "";
        size_t value_len = eq ? strlen(vstart) : 0;
        KaiValue *name_kv  = kai_str_from_bytes(entry, name_len);
        KaiValue *value_kv = kai_str_from_bytes(vstart, value_len);
        KaiValue *fields[2] = { name_kv, value_kv };
        static const char *names[2] = { "fst", "snd" };
        KaiValue *pair = kai_record(2, fields, names);
        acc = kai_cons(pair, acc);
    }
    return kai_cont_resume(k, acc);
}

/* m7a #7: default File handlers. Both reuse the prelude helpers
 * which already produce `Result[T, String]` shapes (Doc B §`File`
 * error model). */
static KaiValue *kai_default_file_read_file(void *self, KaiValue *path, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_prelude_read_file(path));
}

static KaiValue *kai_default_file_write_file(void *self, KaiValue *path,
                                              KaiValue *contents, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_prelude_write_file(path, contents));
}

/* m7b #2b: default Mutable handler — wraps the prelude `array_*`
 * helpers and resumes with the result. Doc B §`Mutable` *Default
 * handler* says the trivial wrapping default is installed for any
 * main row that mentions Mutable. The op signatures here mirror
 * the prelude's Array[T] return shape (see compiler.kai
 * builtin_mutable_decl for the divergence note from Doc B). */
static KaiValue *kai_default_mutable_array_make(void *self, KaiValue *n,
                                                 KaiValue *init, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_prelude_array_make(n, init));
}

static KaiValue *kai_default_mutable_array_length(void *self, KaiValue *a, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_prelude_array_length(a));
}

static KaiValue *kai_default_mutable_array_get(void *self, KaiValue *a,
                                                KaiValue *i, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_prelude_array_get(a, i));
}

static KaiValue *kai_default_mutable_array_set(void *self, KaiValue *a,
                                                KaiValue *i, KaiValue *v, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_prelude_array_set(a, i, v));
}

static KaiValue *kai_default_mutable_array_grow(void *self, KaiValue *a,
                                                 KaiValue *n, KaiValue *init, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_prelude_array_grow(a, n, init));
}

/* Default Random handler — PCG32 (M.E.O'Neill 2014). Per-process
 * shared state, seeded once on first use from time(NULL). Doc B
 * §`Random` specifies seeding from SecureRandom; that is deferred
 * until SecureRandom lands. Non-cryptographic by design — for
 * games, simulations, sampling, fixtures. */
static uint64_t _kai_pcg_state  = 0x853c49e6748fea9bULL;
static uint64_t _kai_pcg_inc    = 0xda3e39cb94b95bdbULL;
static int      _kai_pcg_seeded = 0;

static uint32_t _kai_pcg32_next(void) {
    uint64_t old = _kai_pcg_state;
    _kai_pcg_state = old * 6364136223846793005ULL + (_kai_pcg_inc | 1ULL);
    uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = (uint32_t)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << (((uint32_t)(-(int32_t)rot)) & 31u));
}

static void _kai_pcg32_seed(uint64_t initstate, uint64_t initseq) {
    _kai_pcg_state = 0u;
    _kai_pcg_inc   = (initseq << 1u) | 1u;
    (void) _kai_pcg32_next();
    _kai_pcg_state += initstate;
    (void) _kai_pcg32_next();
}

static void _kai_pcg32_ensure_seeded(void) {
    if (!_kai_pcg_seeded) {
        uint64_t t = (uint64_t) time(NULL);
        _kai_pcg32_seed(t ^ 0xa5a5a5a5a5a5a5a5ULL, t * 6364136223846793005ULL);
        _kai_pcg_seeded = 1;
    }
}

/* Doc B §`Random`: int_range(lo, hi) returns a value in [lo, hi).
 * lo >= hi is a panic — there is no meaningful value. The PRNG draw
 * uses the `% delta` shortcut; for delta well under 2^32 (e.g. 52
 * for a card deck) the modulo bias is negligible. Rejection sampling
 * is a future polish. */
static KaiValue *kai_default_random_int_range(void *self, KaiValue *lo, KaiValue *hi, KaiCont *k) {
    (void) self;
    int64_t lo_v = (lo && lo->tag == KAI_INT) ? lo->as.i : 0;
    int64_t hi_v = (hi && hi->tag == KAI_INT) ? hi->as.i : 0;
    if (lo_v >= hi_v) {
        fprintf(stderr, "kai: Random.int_range: lo (%lld) >= hi (%lld)\n",
                (long long) lo_v, (long long) hi_v);
        exit(1);
    }
    _kai_pcg32_ensure_seeded();
    uint64_t delta = (uint64_t) (hi_v - lo_v);
    uint64_t draw;
    if (delta <= 0xFFFFFFFFULL) {
        draw = (uint64_t) _kai_pcg32_next() % delta;
    } else {
        uint64_t hi32 = (uint64_t) _kai_pcg32_next();
        uint64_t lo32 = (uint64_t) _kai_pcg32_next();
        draw = ((hi32 << 32) | lo32) % delta;
    }
    return kai_cont_resume(k, kai_int(lo_v + (int64_t) draw));
}

/* =================================================================
 * Clock default handler
 * =================================================================
 *
 * Spec: docs/effects-stdlib.md §`Clock`. Three ops:
 *   wall_now()       -> WallTime { secs, nanos } via CLOCK_REALTIME
 *   monotonic_now()  -> Instant  { secs, nanos } via CLOCK_MONOTONIC
 *   sleep_ns(ns)     -> Unit     via nanosleep
 *
 * v1 sleep blocks the OS thread; the m8 v1 inline-eager scheduler has
 * no cooperative yield to deliver `Cancel` mid-sleep. Once m8.x ships
 * the cooperative scheduler, this handler upgrades to register the
 * fiber on a timer wheel and yield through `Spawn.yield`. Tracked in
 * the m8.x follow-up.
 *
 * Field names ("secs", "nanos") are load-bearing: kai_op_field reads
 * the slot by strcmp on the name pointer's contents, so the static
 * cstrings here must match the kaikai-side `WallTime` / `Instant`
 * declarations in stdlib/time.kai exactly. */
static KaiValue *_kai_clock_make_record(int64_t secs, int64_t nanos) {
    KaiValue *secs_kv  = kai_int(secs);
    KaiValue *nanos_kv = kai_int(nanos);
    KaiValue *fields[2] = { secs_kv, nanos_kv };
    static const char *names[2] = { "secs", "nanos" };
    return kai_record(2, fields, names);
}

static KaiValue *kai_default_clock_wall_now(void *self, KaiCont *k) {
    (void) self;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        fprintf(stderr, "kai: Clock.wall_now: clock_gettime failed: %s\n",
                strerror(errno));
        exit(1);
    }
    return kai_cont_resume(k, _kai_clock_make_record(
        (int64_t) ts.tv_sec, (int64_t) ts.tv_nsec));
}

static KaiValue *kai_default_clock_monotonic_now(void *self, KaiCont *k) {
    (void) self;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        fprintf(stderr, "kai: Clock.monotonic_now: clock_gettime failed: %s\n",
                strerror(errno));
        exit(1);
    }
    return kai_cont_resume(k, _kai_clock_make_record(
        (int64_t) ts.tv_sec, (int64_t) ts.tv_nsec));
}

static KaiValue *kai_default_clock_sleep_ns(void *self, KaiValue *ns, KaiCont *k) {
    (void) self;
    int64_t ns_v = (ns && ns->tag == KAI_INT) ? ns->as.i : 0;
    if (ns_v > 0) {
        struct timespec req;
        req.tv_sec  = (time_t) (ns_v / 1000000000LL);
        req.tv_nsec = (long)   (ns_v % 1000000000LL);
        struct timespec rem;
        /* Loop on EINTR so a stray signal doesn't shorten the sleep.
         * v1 is not Cancel-aware (m8.x scope) — Cancel-delivered while
         * a fiber sleeps will be observed only after wake-up. */
        while (nanosleep(&req, &rem) != 0) {
            if (errno != EINTR) break;
            req = rem;
        }
    }
    return kai_cont_resume(k, kai_unit());
}

/* =================================================================
 * NetTcp default handler (net-tcp-v1)
 * =================================================================
 *
 * Spec: docs/effects-stdlib.md §`NetTcp`. Six ops, all blocking in
 * v1. Maps directly to POSIX sockets via libc — same FFI-to-libc
 * pattern as the File / Random defaults above. Errors return
 * `Err(strerror(errno))`; success wraps the handle in a record
 * matching the surface types declared by the compiler:
 *
 *   Conn      = { fd: Int }
 *   Listener  = { fd: Int, port: Int }
 *
 * The `port` slot on Listener carries the kernel-assigned port back
 * to the caller (the `bind(port=0)` use case). It is populated by
 * the listen op via getsockname; ordinary `bind(port=N)` callers
 * see the same N echoed back.
 *
 * v1 keeps to AF_INET (IPv4). AF_INET6 fallback waits on a separate
 * lane — calling out IPv4-only as a v1 limitation in the spec note.
 */

static KaiValue *_kai_net_err(KaiCont *k, int saved_errno) {
    const char *msg = strerror(saved_errno);
    if (!msg) msg = "unknown error";
    KaiValue *m = kai_str(msg);
    KaiValue *err = kai_variant(0, "Err", 1, &m);
    return kai_cont_resume(k, err);
}

static KaiValue *_kai_net_err_msg(KaiCont *k, const char *msg) {
    KaiValue *m = kai_str(msg);
    KaiValue *err = kai_variant(0, "Err", 1, &m);
    return kai_cont_resume(k, err);
}

/* Build a Conn record `{ fd }`. Static field-name strings match
 * what the kaikai-side `type Conn = { fd: Int }` declaration
 * produces from emit_record_constructor — kai_op_field reads by
 * strcmp on the name pointer's contents, so any pointer to a
 * matching cstring works. */
static KaiValue *_kai_net_make_conn(int fd) {
    KaiValue *fd_kv = kai_int((int64_t) fd);
    KaiValue *fields[1] = { fd_kv };
    static const char *names[1] = { "fd" };
    return kai_record(1, fields, names);
}

static KaiValue *_kai_net_make_listener(int fd, int port) {
    KaiValue *fd_kv   = kai_int((int64_t) fd);
    KaiValue *port_kv = kai_int((int64_t) port);
    KaiValue *fields[2] = { fd_kv, port_kv };
    static const char *names[2] = { "fd", "port" };
    return kai_record(2, fields, names);
}

/* Pull the `fd` slot out of a Conn / Listener record. Returns -1 if
 * the value is the wrong shape (caller falls through to an error
 * return); v1 trusts the typer to keep this honest. */
static int _kai_net_record_fd(KaiValue *v) {
    if (!v || v->tag != KAI_RECORD) return -1;
    for (int i = 0; i < v->as.rec.n_fields; ++i) {
        if (v->as.rec.names[i] && strcmp(v->as.rec.names[i], "fd") == 0) {
            KaiValue *f = v->as.rec.fields[i];
            if (!f || f->tag != KAI_INT) return -1;
            return (int) f->as.i;
        }
    }
    return -1;
}

/* connect(host, port) -> Result[Conn, String]. host is a hostname
 * or IPv4 dotted-quad; getaddrinfo handles both. Restricted to
 * AF_INET in v1. */
static KaiValue *kai_default_nettcp_connect(void *self, KaiValue *host, KaiValue *port, KaiCont *k) {
    (void) self;
    if (!host || host->tag != KAI_STR || !port || port->tag != KAI_INT) {
        return _kai_net_err_msg(k, "connect: bad arguments");
    }
    char host_buf[256];
    size_t hlen = host->as.s.len < sizeof(host_buf) - 1 ? host->as.s.len : sizeof(host_buf) - 1;
    memcpy(host_buf, host->as.s.bytes, hlen);
    host_buf[hlen] = '\0';
    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%lld", (long long) port->as.i);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host_buf, port_buf, &hints, &res);
    if (gai != 0) {
        const char *msg = gai_strerror(gai);
        return _kai_net_err_msg(k, msg ? msg : "getaddrinfo failed");
    }
    int fd = -1;
    int saved_errno = 0;
    for (struct addrinfo *p = res; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) { saved_errno = errno; continue; }
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
            break;
        }
        saved_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        return _kai_net_err(k, saved_errno ? saved_errno : ECONNREFUSED);
    }
    KaiValue *conn = _kai_net_make_conn(fd);
    KaiValue *ok   = kai_variant(0, "Ok", 1, &conn);
    return kai_cont_resume(k, ok);
}

/* listen(host, port) -> Result[Listener, String]. host = "" or
 * "0.0.0.0" binds INADDR_ANY; specific IPv4 string also works.
 * port = 0 asks the kernel for an ephemeral port; we read it back
 * via getsockname so callers don't need a separate effect op. */
static KaiValue *kai_default_nettcp_listen(void *self, KaiValue *host, KaiValue *port, KaiCont *k) {
    (void) self;
    if (!host || host->tag != KAI_STR || !port || port->tag != KAI_INT) {
        return _kai_net_err_msg(k, "listen: bad arguments");
    }
    char host_buf[256];
    size_t hlen = host->as.s.len < sizeof(host_buf) - 1 ? host->as.s.len : sizeof(host_buf) - 1;
    memcpy(host_buf, host->as.s.bytes, hlen);
    host_buf[hlen] = '\0';

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return _kai_net_err(k, errno);

    /* SO_REUSEADDR mirrors the convention every server example sets
     * — without it a listener that crashed seconds ago can't rebind
     * because the kernel still holds the port in TIME_WAIT. The
     * fixture's `bind(0)` path ignores the assigned port across
     * runs anyway, but explicit servers benefit. */
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t) port->as.i);
    if (host_buf[0] == '\0') {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, host_buf, &addr.sin_addr) != 1) {
        /* Fall back to getaddrinfo for hostnames. */
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = NULL;
        int gai = getaddrinfo(host_buf, NULL, &hints, &res);
        if (gai != 0) {
            close(fd);
            const char *msg = gai_strerror(gai);
            return _kai_net_err_msg(k, msg ? msg : "getaddrinfo failed");
        }
        struct sockaddr_in *sin = (struct sockaddr_in *) res->ai_addr;
        addr.sin_addr = sin->sin_addr;
        freeaddrinfo(res);
    }

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        int e = errno; close(fd); return _kai_net_err(k, e);
    }
    if (listen(fd, 128) < 0) {
        int e = errno; close(fd); return _kai_net_err(k, e);
    }

    /* Read back the assigned port (kernel may have picked one when
     * the caller passed 0). */
    struct sockaddr_in bound;
    socklen_t blen = sizeof(bound);
    int actual_port = (int) port->as.i;
    if (getsockname(fd, (struct sockaddr *) &bound, &blen) == 0) {
        actual_port = (int) ntohs(bound.sin_port);
    }

    KaiValue *l  = _kai_net_make_listener(fd, actual_port);
    KaiValue *ok = kai_variant(0, "Ok", 1, &l);
    return kai_cont_resume(k, ok);
}

/* accept(l) -> Result[Conn, String]. Blocks until a peer connects;
 * v1 has no scheduler suspension, so the fiber is genuinely parked
 * in the syscall. The fixture sequences listen → connect → accept
 * so the connection is queued before accept is called. */
static KaiValue *kai_default_nettcp_accept(void *self, KaiValue *l, KaiCont *k) {
    (void) self;
    int lfd = _kai_net_record_fd(l);
    if (lfd < 0) return _kai_net_err_msg(k, "accept: invalid listener");
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int cfd = accept(lfd, (struct sockaddr *) &peer, &plen);
    if (cfd < 0) return _kai_net_err(k, errno);
    KaiValue *conn = _kai_net_make_conn(cfd);
    KaiValue *ok   = kai_variant(0, "Ok", 1, &conn);
    return kai_cont_resume(k, ok);
}

/* send(c, data) -> Result[Int, String]. data is a [Byte] = [Int]
 * cons list; each element is taken mod 256. v1 walks the list once
 * to assemble a contiguous buffer, then issues a single send(2);
 * partial sends short-circuit and return the byte count actually
 * written, matching the POSIX contract that callers may have to
 * loop. */
static KaiValue *kai_default_nettcp_send(void *self, KaiValue *c, KaiValue *data, KaiCont *k) {
    (void) self;
    int fd = _kai_net_record_fd(c);
    if (fd < 0) return _kai_net_err_msg(k, "send: invalid conn");
    if (!data) return _kai_net_err_msg(k, "send: null data");

    /* Count the cons cells; a [Byte] of millions of bytes is out of
     * scope for v1 — chunked send via streaming is a follow-up. */
    size_t n = 0;
    for (KaiValue *p = data; p && p->tag == KAI_CONS; p = p->as.cons.tail) ++n;
    unsigned char *buf = NULL;
    if (n > 0) {
        buf = (unsigned char *) malloc(n);
        if (!buf) return _kai_net_err_msg(k, "send: out of memory");
        size_t i = 0;
        for (KaiValue *p = data; p && p->tag == KAI_CONS; p = p->as.cons.tail) {
            KaiValue *h = p->as.cons.head;
            int64_t b = (h && h->tag == KAI_INT) ? h->as.i : 0;
            buf[i++] = (unsigned char) (b & 0xff);
        }
    }
    ssize_t wrote = (n == 0) ? 0 : send(fd, buf, n, 0);
    int saved_errno = errno;
    free(buf);
    if (wrote < 0) return _kai_net_err(k, saved_errno);

    KaiValue *cnt = kai_int((int64_t) wrote);
    KaiValue *ok  = kai_variant(0, "Ok", 1, &cnt);
    return kai_cont_resume(k, ok);
}

/* recv(c, max) -> Result[[Byte], String]. max = 0 panics per spec
 * (no useful "read zero bytes"); negative max is treated the same.
 * Peer clean-close (recv == 0) returns Ok([]) so callers can
 * distinguish from "not yet" via the empty list. */
static KaiValue *kai_default_nettcp_recv(void *self, KaiValue *c, KaiValue *max, KaiCont *k) {
    (void) self;
    int fd = _kai_net_record_fd(c);
    if (fd < 0) return _kai_net_err_msg(k, "recv: invalid conn");
    int64_t cap = (max && max->tag == KAI_INT) ? max->as.i : 0;
    if (cap <= 0) {
        fputs("kai: NetTcp.recv: max must be > 0\n", stderr);
        exit(1);
    }
    if (cap > (1 << 20)) cap = 1 << 20;  /* 1 MiB ceiling for v1 */
    unsigned char *buf = (unsigned char *) malloc((size_t) cap);
    if (!buf) return _kai_net_err_msg(k, "recv: out of memory");
    ssize_t got = recv(fd, buf, (size_t) cap, 0);
    int saved_errno = errno;
    if (got < 0) {
        free(buf);
        return _kai_net_err(k, saved_errno);
    }
    KaiValue *acc = kai_nil();
    for (ssize_t i = got; i > 0;) { --i; acc = kai_cons(kai_int((int64_t) buf[i]), acc); }
    free(buf);
    KaiValue *ok = kai_variant(0, "Ok", 1, &acc);
    return kai_cont_resume(k, ok);
}

/* close(c) -> Unit. Spec says errors from close(2) are logged and
 * swallowed — the user can do nothing useful with the value, and
 * shutdown() is the right tool for "did everything flush?" anyway.
 * Returns kai_unit(). */
static KaiValue *kai_default_nettcp_close(void *self, KaiValue *c, KaiCont *k) {
    (void) self;
    int fd = _kai_net_record_fd(c);
    if (fd >= 0) {
        if (close(fd) < 0) {
            /* Stderr only — the surface op returns Unit. */
            fprintf(stderr, "kai: NetTcp.close: %s\n", strerror(errno));
        }
    }
    return kai_cont_resume(k, kai_unit());
}

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

typedef struct {
    int         signo;
    const char *name;
} KaiSignalEntry;

static const KaiSignalEntry kai_signal_entries[] = {
    { SIGINT,  "SigInt"  },
    { SIGTERM, "SigTerm" },
    { SIGHUP,  "SigHup"  },
    { SIGUSR1, "SigUsr1" },
    { SIGUSR2, "SigUsr2" },
    { 0,       NULL      }
};

static int _kai_signal_from_variant(KaiValue *sig_v) {
    if (!sig_v || sig_v->tag != KAI_VARIANT) return 0;
    const char *n = sig_v->as.var.variant_name;
    if (!n) return 0;
    for (const KaiSignalEntry *e = kai_signal_entries; e->name; ++e) {
        if (strcmp(e->name, n) == 0) return e->signo;
    }
    return 0;
}

static KaiValue *_kai_signal_to_variant(int signo) {
    for (const KaiSignalEntry *e = kai_signal_entries; e->name; ++e) {
        if (e->signo == signo) {
            return kai_variant(0, e->name, 0, NULL);
        }
    }
    return NULL;
}

static sigset_t kai_signal_subscribed;
static int      kai_signal_subscribed_init = 0;

static void _kai_signal_init_subscribed(void) {
    if (!kai_signal_subscribed_init) {
        sigemptyset(&kai_signal_subscribed);
        kai_signal_subscribed_init = 1;
    }
}

static KaiValue *kai_default_signal_on(void *self, KaiValue *sig_v, KaiCont *k) {
    (void) self;
    _kai_signal_init_subscribed();
    int signo = _kai_signal_from_variant(sig_v);
    if (signo > 0) {
        sigaddset(&kai_signal_subscribed, signo);
        sigset_t one;
        sigemptyset(&one);
        sigaddset(&one, signo);
        sigprocmask(SIG_BLOCK, &one, NULL);
    }
    return kai_cont_resume(k, kai_unit());
}

/* off(sig): a pending instance that arrived while blocked is delivered
 * with the default disposition as soon as sigprocmask unblocks —
 * typically killing the program on SIGINT/SIGTERM. Documented in
 * docs/effects-stdlib.md §Signal as a sharp edge. */
static KaiValue *kai_default_signal_off(void *self, KaiValue *sig_v, KaiCont *k) {
    (void) self;
    _kai_signal_init_subscribed();
    int signo = _kai_signal_from_variant(sig_v);
    if (signo > 0) {
        sigdelset(&kai_signal_subscribed, signo);
        sigset_t one;
        sigemptyset(&one);
        sigaddset(&one, signo);
        sigprocmask(SIG_UNBLOCK, &one, NULL);
    }
    return kai_cont_resume(k, kai_unit());
}

/* await(): empty subscribed set adds SIGINT defensively so Ctrl-C
 * still wakes the caller. EINTR / unknown signo: retry. */
static KaiValue *kai_default_signal_await(void *self, KaiCont *k) {
    (void) self;
    _kai_signal_init_subscribed();
    sigset_t wait_set = kai_signal_subscribed;
    int any = 0;
    for (const KaiSignalEntry *e = kai_signal_entries; e->name; ++e) {
        if (sigismember(&wait_set, e->signo)) { any = 1; break; }
    }
    if (!any) sigaddset(&wait_set, SIGINT);
    for (;;) {
        int sig = 0;
        int rc  = sigwait(&wait_set, &sig);
        if (rc == 0 && sig > 0) {
            KaiValue *v = _kai_signal_to_variant(sig);
            if (v) return kai_cont_resume(k, v);
        }
    }
}

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
 *   - All ops blocking; the inline-eager scheduler (m8 v1) parks the
 *     OS thread inside waitpid. Reactor-driven cancellation-aware
 *     wait (the `wait_or_kill` shape) lands with m8.x.
 *   - No stdio pipe plumbing. `pipe_stdout` / `pipe_stdin` and the
 *     surface helpers (wait_or_kill, signal) live in the follow-up
 *     `stdlib/os/process.kai` lane on top of these four ops.
 *   - SIGCHLD handling is implicit through blocking waitpid; the
 *     Signal effect intentionally omits SIGCHLD per its catalog
 *     comment so the two effects don't fight over the disposition.
 */

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
            if (!f || f->tag != KAI_INT) return -1;
            return (int) f->as.i;
        }
    }
    return -1;
}

/* Mint Exited(code) / Signaled(signo) — variant *names* are the
 * runtime contract with `builtin_exit_decl`. */
static KaiValue *_kai_process_make_exit_exited(int code) {
    KaiValue *n = kai_int((int64_t) code);
    return kai_variant(0, "Exited", 1, &n);
}

static KaiValue *_kai_process_make_exit_signaled(int signo) {
    KaiValue *n = kai_int((int64_t) signo);
    return kai_variant(1, "Signaled", 1, &n);
}

static KaiValue *_kai_process_err(KaiCont *k, int saved_errno) {
    const char *msg = strerror(saved_errno);
    if (!msg) msg = "unknown error";
    KaiValue *m = kai_str(msg);
    KaiValue *err = kai_variant(0, "Err", 1, &m);
    return kai_cont_resume(k, err);
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
        if (h && h->tag == KAI_STR) {
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

/* start(cmd, args) -> Child. fork + execvp; on failure of either
 * primitive, panic with strerror — start has no Result wrapper in
 * v1 (see the divergence note in builtin_process_decl). */
static KaiValue *kai_default_process_start(void *self, KaiValue *cmd, KaiValue *args, KaiCont *k) {
    (void) self;
    if (!cmd || cmd->tag != KAI_STR) {
        fputs("kai: Process.start: cmd must be a String\n", stderr);
        exit(1);
    }
    /* cmd is heap-allocated by kai_str_from_bytes with a trailing
     * NUL, so passing bytes directly to execvp is safe. */
    const char *cmd_cstr = cmd->as.s.bytes ? cmd->as.s.bytes : "";
    int argc = 0;
    char **argv = _kai_process_build_argv(cmd_cstr, args, &argc);
    if (!argv) {
        fputs("kai: Process.start: out of memory building argv\n", stderr);
        exit(1);
    }

    pid_t pid = fork();
    if (pid < 0) {
        int e = errno;
        _kai_process_free_argv(argv, argc);
        fprintf(stderr, "kai: Process.start: fork: %s\n", strerror(e));
        exit(1);
    }
    if (pid == 0) {
        /* Child: replace image. execvp searches PATH for relative
         * names; absolute paths bypass the search. On any failure
         * exit 127, the shell convention for "command not found". */
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
    KaiValue *child = _kai_process_make_child((int) pid);
    return kai_cont_resume(k, child);
}

/* wait(c) -> Result[Exit, String]. waitpid blocks until the child
 * terminates; EINTR retries (cooperative-scheduler integration is
 * m8.x scope). WIFEXITED → Exited(status); WIFSIGNALED →
 * Signaled(signo). Other states (stopped, continued) cannot occur
 * with `options=0`, so we only branch on the two terminal cases. */
static KaiValue *kai_default_process_wait(void *self, KaiValue *child, KaiCont *k) {
    (void) self;
    int pid = _kai_process_record_pid(child);
    if (pid <= 0) {
        KaiValue *m = kai_str("wait: invalid Child");
        KaiValue *err = kai_variant(0, "Err", 1, &m);
        return kai_cont_resume(k, err);
    }
    int status = 0;
    pid_t rc;
    do {
        rc = waitpid((pid_t) pid, &status, 0);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0) {
        return _kai_process_err(k, errno);
    }
    KaiValue *exit_v;
    if (WIFEXITED(status)) {
        exit_v = _kai_process_make_exit_exited(WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        exit_v = _kai_process_make_exit_signaled(WTERMSIG(status));
    } else {
        /* options=0 should preclude this; treat as Exited(-1) so
         * the user can still match. */
        exit_v = _kai_process_make_exit_exited(-1);
    }
    KaiValue *ok = kai_variant(0, "Ok", 1, &exit_v);
    return kai_cont_resume(k, ok);
}

/* kill(c, sig) -> Result[Unit, String]. Maps directly to kill(2);
 * sig is taken as a raw signo Int (full POSIX set including
 * SIGKILL — see the builtin_process_decl divergence note). */
static KaiValue *kai_default_process_kill(void *self, KaiValue *child, KaiValue *sig, KaiCont *k) {
    (void) self;
    int pid = _kai_process_record_pid(child);
    if (pid <= 0) {
        KaiValue *m = kai_str("kill: invalid Child");
        KaiValue *err = kai_variant(0, "Err", 1, &m);
        return kai_cont_resume(k, err);
    }
    int signo = (sig && sig->tag == KAI_INT) ? (int) sig->as.i : 0;
    if (kill((pid_t) pid, signo) < 0) {
        return _kai_process_err(k, errno);
    }
    KaiValue *u  = kai_unit();
    KaiValue *ok = kai_variant(0, "Ok", 1, &u);
    return kai_cont_resume(k, ok);
}

/* exit(code) -> Nothing. _exit(2) — skip libc atexit / stdio flush
 * to match the doc spec contract. The `: Nothing` return type is
 * load-bearing: we never resume k. */
static KaiValue *kai_default_process_exit(void *self, KaiValue *code, KaiCont *k) {
    (void) self;
    (void) k;
    int c = (code && code->tag == KAI_INT) ? (int) code->as.i : 0;
    _exit(c);
    /* unreachable */
    return NULL;
}

/* =================================================================
 * Log effect — issue #141. Tier S2 #7 of `docs/stdlib-roadmap.md`.
 *
 * Four leveled ops (debug / info / warn / error) routed through the
 * Log effect. The default handler installed by `kai_main_install_
 * defaults` writes to stderr in
 *
 *     [YYYY-MM-DDTHH:MM:SSZ] LEVEL message\n
 *
 * form. The level field is left-padded to 5 chars so the column
 * after it is aligned across all four ops:
 *     "DEBUG", "INFO ", "WARN ", "ERROR".
 *
 * Timestamp source: `clock_gettime(CLOCK_REALTIME)` + `gmtime_r`
 * + `strftime`. Calling `clock_gettime` directly here (rather than
 * routing through the kaikai-side `Clock` effect) keeps the `Log`
 * row free of `Clock` — a program can declare `: Unit / Log` and
 * have its main install the Log default without also pulling
 * `Clock`'s default handler into the row.
 *
 * The runtime flushes stderr after each write so test harnesses
 * see the output deterministically (no buffer hold-back if a
 * later abort kills the process before exit-time flushing fires).
 *
 * v1 limitations (mirrored in stdlib/log.kai):
 *   - No level filtering. All four levels write unconditionally.
 *   - No structured fields, redaction, rotation, color, async
 *     batching, or trace-context propagation. All of those are
 *     ahu.log territory (a higher layer that wraps Log). */

static void _kai_log_emit(const char *level_padded, KaiValue *msg) {
    /* Format the timestamp into a stack buffer. clock_gettime /
     * gmtime_r failure is rare; fall back to a sentinel rather
     * than swallow the message — the user should still see what
     * level + body was intended. */
    char ts[32];
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
        struct tm tm;
        if (gmtime_r(&now.tv_sec, &tm) != NULL &&
            strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm) > 0) {
            /* ok */
        } else {
            memcpy(ts, "?????", 6);
        }
    } else {
        memcpy(ts, "?????", 6);
    }

    /* Header `[<ts>] <LEVEL> ` then the message bytes then newline.
     * fputs on the prefix, fwrite on the message body so embedded
     * NULs in user strings don't truncate the line. */
    fputc('[', stderr);
    fputs(ts, stderr);
    fputs("] ", stderr);
    fputs(level_padded, stderr);
    fputc(' ', stderr);
    if (msg && msg->tag == KAI_STR && msg->as.s.bytes && msg->as.s.len > 0) {
        fwrite(msg->as.s.bytes, 1, msg->as.s.len, stderr);
    }
    fputc('\n', stderr);
    fflush(stderr);
}

static KaiValue *kai_default_log_debug(void *self, KaiValue *msg, KaiCont *k) {
    (void) self;
    _kai_log_emit("DEBUG", msg);
    return kai_cont_resume(k, kai_unit());
}

static KaiValue *kai_default_log_info(void *self, KaiValue *msg, KaiCont *k) {
    (void) self;
    _kai_log_emit("INFO ", msg);
    return kai_cont_resume(k, kai_unit());
}

static KaiValue *kai_default_log_warn(void *self, KaiValue *msg, KaiCont *k) {
    (void) self;
    _kai_log_emit("WARN ", msg);
    return kai_cont_resume(k, kai_unit());
}

static KaiValue *kai_default_log_error(void *self, KaiValue *msg, KaiCont *k) {
    (void) self;
    _kai_log_emit("ERROR", msg);
    return kai_cont_resume(k, kai_unit());
}

/* =================================================================
 * SecureRandom effect — issue #140. Cryptographically-secure RNG
 * deliberately separated from Random so test handlers stubbing the
 * latter cannot weaken security-sensitive paths.
 *
 * Default handler uses the platform CSPRNG, never a userspace PRNG:
 *   - Linux: getrandom(2) syscall (works pre-prng-init; we loop on
 *     EINTR but otherwise treat any failure as fatal — the kernel
 *     pool being unavailable is catastrophic and there is no
 *     meaningful Result for callers who already opted into crypto-
 *     grade randomness).
 *   - macOS / *BSD: arc4random_buf(3) — returns void; the libc
 *     implementation reseeds itself from /dev/urandom and panics
 *     internally on failure, matching the same "infallible from
 *     callers' perspective" contract.
 *
 * v1 limitations (mirrored in docs/effects-stdlib.md §SecureRandom):
 *   - POSIX only. Windows BCryptGenRandom is post-MVP.
 *   - No /dev/urandom fallback. getrandom is the entire Linux story.
 *   - No userspace ChaCha20 CSPRNG layer. Each draw hits the kernel.
 *   - Uniform `int(min, max)` uses `% delta` reduction. The modulo
 *     bias is acceptable for v1 (security-grade rejection sampling
 *     is a follow-up); cryptographic-protocol callers that need a
 *     strictly-uniform integer should draw `bytes` and reduce
 *     themselves.
 */

#if defined(__linux__)
#  include <sys/random.h>
#elif defined(__APPLE__) || defined(__MACH__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
   /* arc4random_buf is in <stdlib.h> on macOS/BSD, already included
    * above. No extra header needed. */
#else
#  error "kai SecureRandom: platform unsupported (POSIX getrandom / arc4random_buf required)"
#endif

static void _kai_securerandom_fill(unsigned char *buf, size_t n) {
#if defined(__linux__)
    size_t off = 0;
    while (off < n) {
        ssize_t got = getrandom(buf + off, n - off, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "kai: SecureRandom: getrandom failed: %s\n", strerror(errno));
            exit(1);
        }
        off += (size_t) got;
    }
#else
    arc4random_buf(buf, n);
#endif
}

/* int_range(min, max) -> Int in [min, max] (inclusive). Pulls 8
 * bytes from the platform CSPRNG, interprets them as a uint64,
 * reduces modulo (max - min + 1), and adds min. min > max is a
 * panic: there is no meaningful uniform draw over an empty range.
 * Op named `int_range` (not the doc's `int()`) because `int` is a
 * C keyword and the C backend emits each effect op as a struct
 * field by name. */
static KaiValue *kai_default_securerandom_int_range(void *self, KaiValue *min_v, KaiValue *max_v, KaiCont *k) {
    (void) self;
    int64_t lo = (min_v && min_v->tag == KAI_INT) ? min_v->as.i : 0;
    int64_t hi = (max_v && max_v->tag == KAI_INT) ? max_v->as.i : 0;
    if (lo > hi) {
        fprintf(stderr, "kai: SecureRandom.int: min (%lld) > max (%lld)\n",
                (long long) lo, (long long) hi);
        exit(1);
    }
    unsigned char buf[8];
    _kai_securerandom_fill(buf, sizeof(buf));
    uint64_t draw = 0;
    for (int i = 0; i < 8; ++i) draw = (draw << 8) | (uint64_t) buf[i];
    uint64_t span = (uint64_t) (hi - lo) + 1ULL;
    uint64_t pick = (span == 0) ? draw : (draw % span);
    return kai_cont_resume(k, kai_int(lo + (int64_t) pick));
}

/* bytes(n) -> [Int] (each in [0, 256), surface-typed [Byte] but
 * stage 2 has no first-class Byte yet — same divergence the NetTcp
 * decl documents). n <= 0 returns the empty list; n > 1 MiB is
 * capped (matches NetTcp.recv's v1 ceiling — multi-megabyte single
 * draws are out of scope and almost always wrong for crypto use,
 * which is happier with smaller, repeated draws). */
static KaiValue *kai_default_securerandom_bytes(void *self, KaiValue *n_v, KaiCont *k) {
    (void) self;
    int64_t n = (n_v && n_v->tag == KAI_INT) ? n_v->as.i : 0;
    if (n <= 0) {
        return kai_cont_resume(k, kai_nil());
    }
    if (n > (1 << 20)) n = 1 << 20;
    unsigned char *buf = (unsigned char *) malloc((size_t) n);
    if (!buf) {
        fputs("kai: SecureRandom.bytes: out of memory\n", stderr);
        exit(1);
    }
    _kai_securerandom_fill(buf, (size_t) n);
    KaiValue *acc = kai_nil();
    for (int64_t i = n; i > 0;) { --i; acc = kai_cons(kai_int((int64_t) buf[i]), acc); }
    free(buf);
    return kai_cont_resume(k, acc);
}

/* =================================================================
 * m8.x cooperative scheduler primitives
 * =================================================================
 *
 * Spec: docs/fibers-impl.md §*Scheduler* and §*Yield primitives*.
 *
 * Single-threaded cooperative scheduler. The OS thread starts in
 * kai_main_fiber (state=RUNNING); spawned fibers each get a private
 * heap-allocated stack and a ucontext_t. Yield/park use swapcontext
 * to hand control between fibers. The dispatcher is implicit — there
 * is no separate scheduler context, just whoever was running before
 * the current fiber gets resumed when the queue empties.
 *
 * v1 ships Phase 2 (scheduler core); Phase 3 adds Cancel delivery at
 * yield points; Phase 4 adds blocking primitives (BlockSender mailbox
 * + Actor.receive parking); Phase 5 adds Link/Monitor runtime.
 */

/* Page size, queried once via sysconf and cached. macOS arm64 pages
 * are 16 KiB, x86_64 / Linux are 4 KiB; the guard arithmetic must use
 * the runtime value or mprotect rejects the call. */
static size_t kai_page_size(void) {
    static size_t cached = 0;
    if (cached == 0) {
        long ps = sysconf(_SC_PAGESIZE);
        cached = (ps > 0) ? (size_t) ps : 4096;
    }
    return cached;
}

/* Read KAI_FIBER_STACK_SIZE once and cache. Out-of-range values
 * fall back to the default and log a warning. The result is rounded
 * up to a page-size multiple — mmap + mprotect both require it, and
 * 16 KiB pages on macOS arm64 silently break sub-page values
 * otherwise. */
static size_t kai_fiber_stack_size(void) {
    static size_t cached = 0;
    if (cached != 0) return cached;
    const char *env = getenv("KAI_FIBER_STACK_SIZE");
    size_t sz = 64 * 1024;  /* default 64 KiB */
    if (env && *env) {
        char *end = NULL;
        long long parsed = strtoll(env, &end, 10);
        if (end != env && *end == '\0' && parsed >= 4096 && parsed <= (long long)(64 * 1024 * 1024)) {
            sz = (size_t) parsed;
        } else {
            fprintf(stderr,
                "kai: KAI_FIBER_STACK_SIZE=%s out of range [4096, 64M]; using default 64K\n",
                env);
        }
    }
    size_t ps = kai_page_size();
    if (sz % ps != 0) sz = ((sz / ps) + 1) * ps;
    cached = sz;
    return cached;
}

/* SIGSEGV / SIGBUS handler for fiber stack overflow. The fault lands
 * on the active fiber's guard page (PROT_NONE); we print a diagnostic
 * and re-raise with the default disposition. Faults outside any guard
 * (e.g. NULL deref in user code) fall through to default — we only
 * decorate the stack-overflow case. The handler runs on a sigaltstack
 * so the overflowed stack is never used to format the message. Spec:
 * `docs/fibers-honesty-targets.md` Tier 1. */
static void *kai_sigalt_stack = NULL;
static int   kai_sigsegv_installed = 0;

static void kai_fiber_sigsegv_handler(int sig, siginfo_t *info, void *ucp) {
    (void) ucp;
    KaiFiber *f = kai_active_fiber;
    if (f && f->stack_base && info && info->si_addr) {
        char *guard_lo = (char *) f->stack_base;
        char *guard_hi = guard_lo + kai_page_size();
        char *addr     = (char *) info->si_addr;
        if (addr >= guard_lo && addr < guard_hi) {
            char buf[96];
            int n = snprintf(buf, sizeof(buf),
                             "kai: fiber stack overflow at %p\n", (void *) f);
            if (n > 0) {
                ssize_t w = write(2, buf, (size_t) n);
                (void) w;
            }
        }
    }
    /* Re-raise with default disposition so the process terminates with
     * the original signal — preserving the standard SIGSEGV exit
     * status for callers (shell `$?` = 139) without swallowing
     * unrelated faults. */
    signal(sig, SIG_DFL);
    raise(sig);
}

static void kai_install_fiber_sigsegv_handler(void) {
    if (kai_sigsegv_installed) return;
    kai_sigsegv_installed = 1;

    size_t altsize = (size_t) SIGSTKSZ;
    if (altsize < 32 * 1024) altsize = 32 * 1024;
    kai_sigalt_stack = malloc(altsize);
    if (kai_sigalt_stack) {
        stack_t ss;
        ss.ss_sp    = kai_sigalt_stack;
        ss.ss_size  = altsize;
        ss.ss_flags = 0;
        sigaltstack(&ss, NULL);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = kai_fiber_sigsegv_handler;
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
}

static void kai_sched_enqueue(KaiFiber *f) {
    f->sched_next = NULL;
    if (kai_ready_tail) {
        kai_ready_tail->sched_next = f;
    } else {
        kai_ready_head = f;
    }
    kai_ready_tail = f;
}

static KaiFiber *kai_sched_dequeue(void) {
    KaiFiber *f = kai_ready_head;
    if (!f) return NULL;
    kai_ready_head = f->sched_next;
    if (!kai_ready_head) kai_ready_tail = NULL;
    f->sched_next = NULL;
    return f;
}

/* Forward decl: the trampoline drives a fiber's body and walks its
 * awaiter chain on completion. Defined below so it can call the
 * scheduler primitives + kai_apply. */
static void kai_fiber_trampoline(void);

/* Phase 5 forward decl: link propagation runs in the trampoline's
 * termination tail; the helper itself is defined alongside the
 * Link default handler further down. The reason argument distinguishes
 * Normal (DONE) from Crashed (CANCELLED) for trap-exit delivery. */
typedef enum {
    KAI_EXIT_NORMAL  = 0,
    KAI_EXIT_CRASHED = 1
} KaiExitReason;

static void kai_link_propagate_terminate(KaiFiber *self, KaiExitReason reason);
/* Tier 2 Monitor — observers learn about target's termination via a
 * single push of `target_pid` into observer->mailbox. Defined
 * alongside the Monitor default handler further down. */
static void kai_monitor_propagate_terminate(KaiFiber *self);

/* Initialise a freshly-allocated KaiFiber's ucontext + private stack.
 * Sets f->ctx so it can be a swapcontext target; fills uc_link as a
 * fallback (the trampoline always exits via setcontext, so uc_link
 * is reachable only on a runtime bug). The thunk slot must already
 * be filled by the caller before the fiber runs. */
#if defined(__APPLE__) || defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
static void kai_fiber_init_ctx(KaiFiber *f) {
    kai_install_fiber_sigsegv_handler();
    if (getcontext(&f->ctx) != 0) {
        fprintf(stderr, "kai: getcontext failed for new fiber\n");
        exit(1);
    }
    f->stack_size = kai_fiber_stack_size();
    /* Allocate stack + one guard page below it. Stack grows down on
     * x86_64 / arm64, so the lowest address is the overflow target.
     * stack_base points at the guard; the usable region starts one
     * page above. Layout (low → high):
     *   [ guard page (PROT_NONE) | stack_size bytes (RW) ]
     * We store stack_base as the mmap base so munmap covers both;
     * stack_size remains the usable size, and total = stack_size +
     * page_size whenever we need to release the region. */
    size_t page = kai_page_size();
    size_t total = f->stack_size + page;
    void *region = mmap(NULL, total, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (region == MAP_FAILED) {
        fprintf(stderr, "kai: mmap failed allocating fiber stack (%zu bytes)\n",
                total);
        exit(1);
    }
    if (mprotect(region, page, PROT_NONE) != 0) {
        fprintf(stderr, "kai: mprotect guard page failed for fiber stack\n");
        munmap(region, total);
        exit(1);
    }
    f->stack_base = region;
    f->ctx.uc_stack.ss_sp   = (char *) region + page;
    f->ctx.uc_stack.ss_size = f->stack_size;
    f->ctx.uc_link          = &kai_main_fiber.ctx;
    makecontext(&f->ctx, kai_fiber_trampoline, 0);
}

/* Yield: caller stays READY and goes back on the run queue; control
 * swaps to the head of the queue. No-op if the queue is empty (caller
 * is the only ready fiber, nothing to switch to). */
static void kai_sched_yield(void) {
    KaiFiber *current = kai_active_fiber;
    KaiFiber *next    = kai_sched_dequeue();
    if (!next) return;  /* alone — nothing to yield to */
    current->state = KAI_FIBER_READY;
    kai_sched_enqueue(current);
    next->state = KAI_FIBER_RUNNING;
    kai_active_fiber = next;
    swapcontext(&current->ctx, &next->ctx);
    /* Resumed: another fiber yielded/parked back to us; the swap
     * source (current->ctx) holds the state that was just restored.
     * R4 fix — if the fiber that swapped to us was the trampoline
     * tail of a now-discarded fiber, `kai_pending_free` carries its
     * deferred struct + stack. Reap before continuing. */
    kai_drain_pending_free();
}

/* Park: caller goes PARKED, control swaps to the head of the run
 * queue. The caller must have already linked itself into a wakeup
 * list (awaiter chain, mailbox waiter list) before calling park —
 * otherwise no fiber can ever unpark it.
 *
 * Deadlock detection: if the run queue is empty when we park, no
 * one can wake us up — panic with a diagnostic. */
static void kai_sched_park(void) {
    KaiFiber *current = kai_active_fiber;
    KaiFiber *next    = kai_sched_dequeue();
    if (!next) {
        fprintf(stderr,
            "kai: deadlock — fiber parked with empty run queue (%d parked total)\n",
            kai_parked_count + 1);
        exit(1);
    }
    current->state = KAI_FIBER_PARKED;
    kai_parked_count++;
    next->state = KAI_FIBER_RUNNING;
    kai_active_fiber = next;
    swapcontext(&current->ctx, &next->ctx);
    /* R4 fix — see kai_sched_yield: drain pending free on resume. */
    kai_drain_pending_free();
}

/* Unpark: target PARKED → READY, enqueue at run queue tail. Caller
 * stays RUNNING (does not yield); the unparked fiber runs whenever
 * the scheduler reaches it. No-op if target is not currently
 * PARKED (defensive against double-unpark). */
static void kai_sched_unpark(KaiFiber *target) {
    if (!target || target->state != KAI_FIBER_PARKED) return;
    target->state = KAI_FIBER_READY;
    kai_parked_count--;
    kai_sched_enqueue(target);
}

/* Trampoline: the entry point makecontext installs on every spawned
 * fiber's stack. Reads kai_active_fiber to find itself (set by the
 * dispatcher who swapped in), runs the thunk, walks awaiters, and
 * hands control to the next ready fiber via setcontext.
 *
 * Phase 3 — Cancel landing: setjmp(cancel_pad) before the body runs.
 * The yield-point hook in kai_evidence_lookup* longjmps here when
 * cancel_requested fires; the second-return path skips the body and
 * marks the fiber CANCELLED instead of DONE. cancel_pad_set is the
 * gate the hook reads before attempting the longjmp.
 *
 * RC contract: f->thunk is owned by f for f's entire lifetime;
 * kai_apply borrows. kai_free_value's KAI_FIBER branch decrefs both
 * thunk and result when f's RC drops, so the trampoline does not
 * touch RC on either. */
static void kai_fiber_trampoline(void) {
    KaiFiber *self = kai_active_fiber;
    /* First entry follows a setcontext from another fiber's
     * trampoline tail or a swap from yield/park. Drain any pending
     * struct free left behind by the previous fiber's
     * `kai_decref(self->value)` before we touch our own state. */
    kai_drain_pending_free();
    if (setjmp(self->cancel_pad) == 0) {
        self->cancel_pad_set = 1;
        self->result = kai_apply(self->thunk, 0, NULL);
        self->cancel_pad_set = 0;
        self->state  = KAI_FIBER_DONE;
    } else {
        /* Cancel landing: the yield-point hook (or kai_default_cancel_raise)
         * longjmped here. The body did not finish; result stays NULL. */
        self->cancel_pad_set = 0;
        self->state = KAI_FIBER_CANCELLED;
    }

    /* Phase 5 — Link propagation. Walk the linked chain. For each
     * peer, behaviour depends on the peer's trap_exit flag (Tier 2):
     *   - trap_exit=0 (default): set cancel_requested (current
     *     behaviour, delivered at the peer's next yield-point hook).
     *   - trap_exit=1: push a "Normal"/"Crashed" string into the
     *     peer's mailbox so the peer can react in user code instead
     *     of being cancelled.
     * The exit reason is read from `self->state` at this point —
     * DONE → Normal, CANCELLED → Crashed (any other state would be
     * a runtime invariant violation). */
    kai_link_propagate_terminate(self,
        self->state == KAI_FIBER_DONE ? KAI_EXIT_NORMAL : KAI_EXIT_CRASHED);

    /* Tier 2 Monitor — push our pid into each observer's mailbox.
     * Observers do not get cancel_requested set; monitors are
     * unidirectional and fault-isolated. */
    kai_monitor_propagate_terminate(self);

    /* Wake awaiters. Each was parked in Spawn.await (Phase 2.3) or
     * Spawn.select (Phase 2.3). Walk the chain, clearing each
     * awaiter's next-link before unparking so a re-park does not see
     * stale links. */
    KaiFiber *a = self->awaiters_head;
    self->awaiters_head = NULL;
    while (a) {
        KaiFiber *nx = a->awaiters_next;
        a->awaiters_next = NULL;
        kai_sched_unpark(a);
        a = nx;
    }

    /* R4 fix — drop the scheduler-side incref on our wrapper, taken
     * by `kai_default_spawn_spawn` before enqueue. Pairs `enqueue`
     * with this single decref. If the user already discarded their
     * Fiber[T] handle, this brings RC to 0 and `kai_free_value`
     * defers the struct/stack free into `kai_pending_free` (we are
     * still standing on this fiber's stack); the next fiber's drain
     * hook reaps it. If awaiters or the user still hold the handle,
     * the wrapper survives this decref and is freed later by their
     * own drops. */
    if (self->value) {
        kai_decref(self->value);
    }

    /* Hand control to the next ready fiber. Awaiters we just woke
     * are now in the queue; the FIFO discipline picks the oldest
     * ready (which may be one of them, or main, or a sibling). If
     * the queue is empty here, every other fiber is parked with no
     * wakeup path → deadlock. */
    KaiFiber *next = kai_sched_dequeue();
    if (!next) {
        fprintf(stderr,
            "kai: fiber finished with empty run queue (%d parked) — deadlock\n",
            kai_parked_count);
        exit(1);
    }
    next->state = KAI_FIBER_RUNNING;
    kai_active_fiber = next;
    setcontext(&next->ctx);
    /* setcontext does not return. */
}
#if defined(__APPLE__) || defined(__clang__)
#  pragma clang diagnostic pop
#endif

/* m8.x: default Spawn handlers backed by the cooperative scheduler.
 * spawn(thunk)        — alloc fiber + enqueue; thunk runs later.
 * await(fiber)        — park on the fiber's awaiter chain until DONE.
 * yield()             — rotate run queue (no-op if alone).
 * select([fiber])     — Phase 2 simplification: head-of-list, parking
 *                       on the head if not yet DONE. Real race +
 *                       cancel-losers semantics land in Phase 4
 *                       alongside Cancel delivery (Phase 3).
 * cancel(fiber)       — set cancel_requested; delivery is Phase 3.
 *
 * RC contract: op handlers borrow their KaiValue* args (the call site
 * decrefs after the call per Perceus). Handlers that need to retain
 * an arg `kai_incref` before storing — same pattern the m8 v1 handlers
 * used and that `kai_prelude_map` documents at runtime.h:1031-1040.
 * R1 Phase 3 flipped only the 13 primitives named in CHANGELOG v0.2.0
 * step C; effect-op handlers were not touched, so the call-site / op
 * contract stayed "callee borrows".
 *
 * Spec: docs/fibers-impl.md §*Yield-point list* and §*Trampoline*. */
static KaiValue *kai_default_spawn_yield(void *self, KaiCont *k) {
    (void) self;
    kai_sched_yield();
    return kai_cont_resume(k, kai_unit());
}

static KaiValue *kai_default_spawn_spawn(void *self, KaiValue *thunk, KaiCont *k) {
    (void) self;
    if (!thunk || thunk->tag != KAI_CLOSURE) {
        fprintf(stderr, "kai: Spawn.spawn called with non-closure value\n");
        exit(1);
    }
    KaiFiber *f = (KaiFiber *) calloc(1, sizeof(KaiFiber));
    if (!f) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    f->parent       = kai_current_fiber();
    /* incref to retain a ref the fiber owns; the caller-side ref is
     * still the caller's to manage (Perceus decrefs at call-site exit). */
    f->thunk        = kai_incref(thunk);
    /* Inherit parent's evidence_top as floor. The lexical guarantee
     * (docs/fibers-impl.md §*Balance invariant*) was that a handle
     * scoping the spawn cannot pop while the spawned fiber is still
     * live, so the inherited pointer would stay valid.
     *
     * Issue #104 — that guarantee held only at the level of "stack
     * memory mapping", not "stack memory contents". Once parent's
     * code returns past those frames, any subsequent function call
     * the parent makes (a Stdout.print after closing the inner
     * with_mailbox, the link/monitor propagate walks at trampoline
     * tail) writes its locals into the SAME stack slots the popped
     * KaiEvidence frames lived in. The frames' eff_label / parent
     * fields turn into garbage long before the spawned child runs.
     *
     * Fix: at spawn time clone the inherited chain into heap nodes
     * so navigation from the child reads only memory we own. The
     * clone is shallow — handler / handle_jmp / discard_slot still
     * point at the original *Ev structs, which is fine for default
     * handlers (their *Ev sits in static storage from the main
     * wrapper) and accepted as a known limitation for inherited
     * USER handlers (those *Ev structs live on a parent's stack and
     * face the same overwrite hazard regardless of the chain
     * representation). The chain is owned by this fiber and freed
     * in kai_free_value's KAI_FIBER branch via
     * kai_free_cloned_evidence_chain. */
    f->evidence_top          = kai_clone_evidence_chain(f->parent->evidence_top);
    f->cloned_evidence_chain = f->evidence_top;
    kai_fiber_init_ctx(f);
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
    f->state = KAI_FIBER_READY;
    kai_sched_enqueue(f);
    return kai_cont_resume(k, v);
}

static KaiValue *kai_default_spawn_await(void *self, KaiValue *fib_v, KaiCont *k) {
    (void) self;
    if (!fib_v || fib_v->tag != KAI_FIBER || !fib_v->as.fib) {
        fprintf(stderr, "kai: Spawn.await called on non-fiber value\n");
        exit(1);
    }
    KaiFiber *f = fib_v->as.fib;
    if (f->state != KAI_FIBER_DONE) {
        /* Park self on f's awaiter chain. The trampoline (in
         * kai_fiber_trampoline) walks this chain on DONE and
         * unparks each awaiter — putting us back on the run queue
         * with state READY. */
        KaiFiber *me = kai_active_fiber;
        me->awaiters_next = f->awaiters_head;
        f->awaiters_head  = me;
        kai_sched_park();
        if (f->state != KAI_FIBER_DONE) {
            fprintf(stderr,
                "kai: Spawn.await woken but target not DONE (state=%d)\n",
                (int) f->state);
            exit(1);
        }
    }
    return kai_cont_resume(k, kai_incref(f->result));
}

static KaiValue *kai_default_spawn_select(void *self, KaiValue *fibs_v, KaiCont *k) {
    (void) self;
    if (!fibs_v || (fibs_v->tag != KAI_CONS && fibs_v->tag != KAI_NIL)) {
        fprintf(stderr, "kai: Spawn.select called on non-list value\n");
        exit(1);
    }
    if (fibs_v->tag == KAI_NIL) {
        fprintf(stderr, "kai: Spawn.select called on empty list\n");
        exit(1);
    }
    /* v1 (Phase 2): walk once for an already-DONE fiber. If none,
     * park on the head. Real race + cancel-losers needs Phase 3
     * (Cancel delivery to losers' yield points) and lands in
     * Phase 4 together with the blocking-mailbox primitives. */
    KaiValue *cur   = fibs_v;
    KaiFiber *head  = NULL;
    while (cur && cur->tag == KAI_CONS) {
        KaiValue *elem = cur->as.cons.head;
        if (!elem || elem->tag != KAI_FIBER || !elem->as.fib) {
            fprintf(stderr, "kai: Spawn.select: list element is not a fiber\n");
            exit(1);
        }
        KaiFiber *fib = elem->as.fib;
        if (!head) head = fib;
        if (fib->state == KAI_FIBER_DONE) {
            return kai_cont_resume(k, kai_incref(fib->result));
        }
        cur = cur->as.cons.tail;
    }
    /* No fiber DONE on entry. Park on the head; when woken, head
     * must be DONE. */
    KaiFiber *me = kai_active_fiber;
    me->awaiters_next = head->awaiters_head;
    head->awaiters_head = me;
    kai_sched_park();
    if (head->state != KAI_FIBER_DONE) {
        fprintf(stderr,
            "kai: Spawn.select woken but head fiber not DONE (state=%d)\n",
            (int) head->state);
        exit(1);
    }
    return kai_cont_resume(k, kai_incref(head->result));
}

static KaiValue *kai_default_spawn_cancel(void *self, KaiValue *fib_v, KaiCont *k) {
    (void) self;
    if (fib_v && fib_v->tag == KAI_FIBER && fib_v->as.fib) {
        fib_v->as.fib->cancel_requested = 1;
        /* Delivery (inject Cancel.raise() at the target's next
         * op-call boundary) lands in Phase 3 via the lookup-prologue
         * hook in kai_evidence_lookup_node. The flag-only behaviour
         * is the contract Spawn.cancel commits to; only the *visible
         * effect* changes when Phase 3 lands. */
    }
    return kai_cont_resume(k, kai_unit());
}

/* Tier 2 trap-exit: set the current fiber's trap_exit flag from a
 * Bool argument. With trap_exit=1, a linked peer's termination
 * delivers a "Normal"/"Crashed" string to the fiber's mailbox
 * instead of setting cancel_requested. The fiber must already
 * have a mailbox (typically via with_mailbox) for the delivery to
 * land; without one, the propagation falls back to cancel_requested.
 * Spec: docs/actors.md §*Trap-exit semantics*. */
static KaiValue *kai_default_spawn_set_trap_exit(void *self, KaiValue *on, KaiCont *k) {
    (void) self;
    KaiFiber *f = kai_current_fiber();
    int v = (on && on->tag == KAI_BOOL && on->as.b) ? 1 : 0;
    f->trap_exit = v;
    return kai_cont_resume(k, kai_unit());
}

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
        longjmp(f->cancel_pad, 1);
        /* Unreachable. */
    }
    fputs("kai: Cancel.raise: unhandled (fiber cancelled)\n", stderr);
    exit(0);
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
 *    uses for link/monitor ops; we resolve via `peer->as.mb->owner_fiber`.
 *  - Self-links (a == b) are dropped.
 *  - If the peer mailbox has no owner_fiber (mailbox alloc'd
 *    outside any fiber context, e.g. before runtime init), the
 *    link is silently dropped — caller cannot observe failure
 *    because the op is `Unit`.
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
        fprintf(stderr, "kai: out of memory (link)\n");
        exit(1);
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
static void kai_link_propagate_terminate(KaiFiber *self, KaiExitReason reason) {
    KaiLinkNode *ln = self->linked_head;
    self->linked_head = NULL;
    while (ln) {
        KaiLinkNode *next = ln->next;
        KaiFiber *peer = ln->peer;
        if (peer) {
            if (peer->trap_exit && peer->mailbox) {
                /* Trap-exit delivery: push the reason string into the
                 * peer's mailbox. The mailbox holds an owning ref on
                 * each msg (kai_mailbox_push convention via
                 * mailbox_send's incref). Wake any parked receiver
                 * — that's exactly what kai_mailbox_push already does
                 * via its recv_waiter handoff. */
                const char *txt = (reason == KAI_EXIT_NORMAL) ? "Normal" : "Crashed";
                kai_mailbox_push(peer->mailbox, kai_str(txt));
            } else {
                peer->cancel_requested = 1;
            }
            /* Remove our back-link from peer's chain (linear scan;
             * v1 chains are short — typically 1-2 entries). */
            KaiLinkNode **slot = &peer->linked_head;
            while (*slot) {
                if ((*slot)->peer == self) {
                    KaiLinkNode *rm = *slot;
                    *slot = rm->next;
                    free(rm);
                    break;
                }
                slot = &(*slot)->next;
            }
        }
        free(ln);
        ln = next;
    }
}

static KaiValue *kai_default_link_link(void *self, KaiValue *peer, KaiCont *k) {
    (void) self;
    if (peer && peer->tag == KAI_PID && peer->as.mb && peer->as.mb->owner_fiber) {
        kai_link_add_bidirectional(kai_current_fiber(), peer->as.mb->owner_fiber);
    }
    return kai_cont_resume(k, kai_unit());
}

/* Issue #103 — return 1 if `f` is bidirectionally linked to any
 * peer with `trap_exit=true`. Used by the Cancel-lookup hook below
 * to decide whether `Cancel.raise()` must bypass user handlers and
 * unwind the fiber directly so the trampoline tail can convert
 * termination into a `"Crashed"` mailbox push on the trap-exit'd
 * peer. The walk is short (link chains are typically 1-2 entries
 * per the v1 simplifications above) so the per-op overhead of this
 * check is negligible. */
static int kai_fiber_has_trap_exit_link(KaiFiber *f) {
    if (!f) return 0;
    KaiLinkNode *ln = f->linked_head;
    while (ln) {
        if (ln->peer && ln->peer->trap_exit) return 1;
        ln = ln->next;
    }
    return 0;
}

/* Issue #103 — bypass user-installed Cancel handlers when the
 * current fiber is linked to a trap-exit'd peer. The contract
 * documented in docs/actors.md §*Trap-exit semantics* requires
 * that a supervisor with `fiber_set_trap_exit(true)` observes a
 * linked child's termination through its mailbox, not through a
 * Cancel handler in the call chain between the spawn point and
 * the receive point. Without this hook, the child's `Cancel.raise()`
 * walks the inherited evidence chain (cloned at spawn time per
 * issue #104) and lands in any outer `with Cancel { raise(_) -> ... }`
 * the parent installed before the spawn — short-circuiting the
 * trampoline-tail link propagation that would have pushed
 * `"Crashed"` into the supervisor's mailbox.
 *
 * The check fires only when the cancel_pad is live (so we have
 * somewhere to longjmp) and the fiber actually has a trap-exit'd
 * link (so this is the linkage-relevant case the issue scopes the
 * fix to). Plain Cancel.raise() inside a fiber that holds no
 * trap-exit'd link still dispatches through user handlers as before,
 * preserving the cleanup-on-cancel idiom for non-supervised work. */
static void kai_check_trap_exit_cancel_bypass(void) {
    KaiFiber *f = kai_active_fiber;
    if (!f || !f->cancel_pad_set) return;
    if (!kai_fiber_has_trap_exit_link(f)) return;
    f->cancel_delivered = 1;
    longjmp(f->cancel_pad, 1);
    /* Unreachable. */
}

/* Tier 2 Monitor — append (observer, target_pid) onto target's
 * monitor_head chain. The runtime takes one ref on target_pid via
 * kai_incref so the value can be pushed into the observer's mailbox
 * at termination time even if the user dropped their last
 * reference. v1 does not deduplicate: monitoring the same pid twice
 * produces two MonitorDown deliveries, matching the BEAM spec. */
static void kai_monitor_add(KaiFiber *target, KaiFiber *observer, KaiValue *target_pid) {
    KaiMonitorNode *n = (KaiMonitorNode *) malloc(sizeof(KaiMonitorNode));
    if (!n) {
        fprintf(stderr, "kai: out of memory (monitor)\n");
        exit(1);
    }
    n->observer   = observer;
    n->target_pid = target_pid;
    if (target_pid) kai_incref(target_pid);
    n->next       = target->monitor_head;
    target->monitor_head = n;
}

/* Tier 2 Monitor — remove the *first* entry on target's chain that
 * matches (observer, target_pid). Demonitor v1 takes the same pid
 * value the user originally passed to monitor(...); equality is
 * by KAI_PID mailbox-pointer identity (kai_op_eq_v's KAI_PID branch).
 * Returns 1 on removal, 0 if no match. */
static int kai_monitor_remove(KaiFiber *target, KaiFiber *observer, KaiValue *target_pid) {
    KaiMonitorNode **slot = &target->monitor_head;
    while (*slot) {
        KaiMonitorNode *cur = *slot;
        int pid_match = 1;
        if (target_pid && cur->target_pid) {
            /* both present — match on mailbox pointer. */
            pid_match =
                cur->target_pid->tag == KAI_PID &&
                target_pid->tag == KAI_PID &&
                cur->target_pid->as.mb == target_pid->as.mb;
        }
        if (cur->observer == observer && pid_match) {
            *slot = cur->next;
            if (cur->target_pid) kai_decref(cur->target_pid);
            free(cur);
            return 1;
        }
        slot = &cur->next;
    }
    return 0;
}

/* Tier 2 Monitor — walk the target fiber's monitor chain at
 * termination. For each (observer, target_pid) entry, push
 * target_pid into observer->mailbox and free the node.
 * Observers without a current mailbox silently drop the
 * delivery (matching trap-exit's "no-mailbox falls back" shape;
 * monitors do not propagate faults so there is no cancel-side
 * fallback to take). The cause distinction (Normal / Crashed) is
 * not encoded in the v1 message — observers that need it can pair
 * Monitor with Link+trap_exit, which delivers the
 * "Normal"/"Crashed" string into the same mailbox. */
static void kai_monitor_propagate_terminate(KaiFiber *self) {
    KaiMonitorNode *mn = self->monitor_head;
    self->monitor_head = NULL;
    while (mn) {
        KaiMonitorNode *next = mn->next;
        KaiFiber *observer = mn->observer;
        if (observer && observer->mailbox && mn->target_pid) {
            /* kai_mailbox_push takes ownership of the incref we
             * stamped at kai_monitor_add time — we hand the ref
             * to the mailbox and don't decref locally. */
            kai_mailbox_push(observer->mailbox, mn->target_pid);
        } else if (mn->target_pid) {
            /* No mailbox to deliver into — drop our owning ref so
             * the pid value can be reclaimed. */
            kai_decref(mn->target_pid);
        }
        free(mn);
        mn = next;
    }
}

static KaiValue *kai_default_monitor_monitor(void *self, KaiValue *target, KaiCont *k) {
    (void) self;
    if (target && target->tag == KAI_PID && target->as.mb && target->as.mb->owner_fiber) {
        kai_monitor_add(target->as.mb->owner_fiber, kai_current_fiber(), target);
    }
    /* v1 simplification — return the same Pid as the ref. The
     * spec's `MonitorRef` is opaque; identifying the monitored
     * fiber by its own pid is sufficient for demonitor and for the
     * fixture-level "which fiber died" pattern. The user-facing
     * type alias `MonitorRef = Pid[Nothing]` is pinned in
     * docs/actors.md §*Monitors — unidirectional* (v1 simplification).
     */
    return kai_cont_resume(k, target);
}

static KaiValue *kai_default_monitor_demonitor(void *self, KaiValue *ref, KaiCont *k) {
    (void) self;
    if (ref && ref->tag == KAI_PID && ref->as.mb && ref->as.mb->owner_fiber) {
        kai_monitor_remove(ref->as.mb->owner_fiber, kai_current_fiber(), ref);
    }
    return kai_cont_resume(k, kai_unit());
}

/* (typedef forward-declared above, before KaiFiber.) */
struct KaiEvidence {
    KaiEvidence *parent;
    const char  *eff_label;     /* canonical effect name (literal or interned). */
    void        *handler;       /* *Ev<Eff> struct; opaque to the runtime. */
    /* m7a #6e: handle's setjmp target + slot to deposit the discarded
     * clause's return value. Set by kai_evidence_push_with_jmp; the
     * op-call site reads them via kai_evidence_lookup_node when
     * status stays UNRESUMED after the clause returns, then longjmps
     * with the clause's value as the handle's body result. NULL when
     * the handle didn't allocate a jmp_buf (the m7a #7 default
     * handlers always resume, so they never longjmp). */
    jmp_buf     *handle_jmp;
    KaiValue   **discard_slot;
};

/* Issue #104 — implementations of the forward-declared helpers.
 * `kai_clone_evidence_chain` walks `src` via its .parent links,
 * malloc's a heap KaiEvidence per node, copies the fields shallow
 * (parent overwritten next iteration; eff_label / handler /
 * handle_jmp / discard_slot copied as-is — see issue #104 notes
 * on KaiFiber.cloned_evidence_chain for the *Ev caveat), and
 * returns the head of the heap chain. */
static void kai_free_cloned_evidence_chain(KaiEvidence *head) {
    while (head) {
        KaiEvidence *next = head->parent;
        free(head);
        head = next;
    }
}

static KaiEvidence *kai_clone_evidence_chain(KaiEvidence *src) {
    KaiEvidence *head = NULL;
    KaiEvidence **link = &head;
    while (src != NULL) {
        KaiEvidence *cp = (KaiEvidence *) malloc(sizeof(KaiEvidence));
        if (!cp) {
            fprintf(stderr, "kai: out of memory (spawn evidence clone)\n");
            exit(1);
        }
        *cp = *src;
        *link = cp;
        link = &cp->parent;
        src = src->parent;
    }
    *link = NULL;
    return head;
}

/* Push an Evidence node onto the current fiber's stack. The caller
 * owns the node's storage — typically `alloca`'d inside a compiled
 * `handle` prologue. This primitive only fills its fields and
 * links it as the new top. */
static void kai_evidence_push(KaiEvidence *node, const char *eff_label, void *handler) {
    KaiFiber *f = kai_current_fiber();
    node->parent       = f->evidence_top;
    node->eff_label    = eff_label;
    node->handler      = handler;
    node->handle_jmp   = NULL;
    node->discard_slot = NULL;
    f->evidence_top    = node;
}

/* m7a #6e: like kai_evidence_push but also stamps the handle's
 * setjmp/longjmp landing pad. The op-call site uses the node's
 * `handle_jmp` to abandon the body when the clause discards
 * `resume`. Default handlers (m7a #7) always resume, so they call
 * the simpler push variant. */
static void kai_evidence_push_with_jmp(KaiEvidence *node, const char *eff_label,
                                        void *handler, jmp_buf *jmp,
                                        KaiValue **discard_slot) {
    KaiFiber *f = kai_current_fiber();
    node->parent       = f->evidence_top;
    node->eff_label    = eff_label;
    node->handler      = handler;
    node->handle_jmp   = jmp;
    node->discard_slot = discard_slot;
    f->evidence_top    = node;
}

/* Pop the topmost Evidence node. The compiled `handle` epilogue
 * always pairs with a matching push (Doc C §*Per-fiber isolation*
 * balance invariant). Pop on an empty stack is a compiler bug; it
 * silently no-ops here so a malformed prologue/epilogue cannot
 * corrupt unrelated memory. */
static void kai_evidence_pop(void) {
    KaiFiber *f = kai_current_fiber();
    if (f->evidence_top != NULL) {
        f->evidence_top = f->evidence_top->parent;
    }
}

/* Walk the current fiber's stack and return the innermost handler
 * for `eff_label`. Returns NULL if no matching handler is in
 * scope — which would indicate a compiler bug, since the type
 * checker rejects unhandled effects. The fast path is pointer
 * equality (most labels are literal strings shared at the call
 * site); strcmp is the fallback for edge cases like dynamically-
 * generated label strings. */

/* Phase 3 — Cancel delivery at yield points. Every effect-op call
 * goes through one of the kai_evidence_lookup* functions; we use
 * that as the natural yield-point check. If the current fiber's
 * cancel flag is set and not yet delivered AND its trampoline
 * cancel_pad is live, longjmp to the pad — the trampoline's
 * second-return path marks the fiber CANCELLED and continues with
 * awaiter walk + dispatch. If the pad is not set (main_fiber, or
 * outside trampoline scope), the check falls through and the op
 * dispatches normally; a subsequent user-level Cancel.raise() will
 * still hit kai_default_cancel_raise which exits the program. v1
 * does not run user-installed `with Cancel { raise(_) -> cleanup }`
 * handlers on runtime-triggered cancel; that interaction is queued
 * as a follow-up (`docs/fibers-honesty-targets.md`
 * §*Residual m8.x items*). */
static void kai_check_cancel_yield_point(void) {
    KaiFiber *f = kai_current_fiber();
    if (f->cancel_requested && !f->cancel_delivered && f->cancel_pad_set) {
        f->cancel_delivered = 1;
        longjmp(f->cancel_pad, 1);
        /* Unreachable. */
    }
}

static void *kai_evidence_lookup(const char *eff_label) {
    kai_check_cancel_yield_point();
    /* Issue #103 — Cancel-on-linked-trap-exit'd-peer must bypass
     * user handlers (see kai_check_trap_exit_cancel_bypass). */
    if (eff_label && strcmp(eff_label, "Cancel") == 0) {
        kai_check_trap_exit_cancel_bypass();
    }
    KaiFiber *f = kai_current_fiber();
    KaiEvidence *node = f->evidence_top;
    while (node != NULL) {
        if (node->eff_label == eff_label
            || strcmp(node->eff_label, eff_label) == 0) {
            return node->handler;
        }
        node = node->parent;
    }
    return NULL;
}

/* m7a #6e: same lookup but returns the whole node so the op-call
 * site can reach the handle's jmp_buf if a discard happens. */
static KaiEvidence *kai_evidence_lookup_node(const char *eff_label) {
    kai_check_cancel_yield_point();
    /* Issue #103 — bypass user Cancel handlers when the current
     * fiber is linked to a trap-exit'd peer (see
     * kai_check_trap_exit_cancel_bypass for rationale). */
    if (eff_label && strcmp(eff_label, "Cancel") == 0) {
        kai_check_trap_exit_cancel_bypass();
    }
    KaiFiber *f = kai_current_fiber();
    KaiEvidence *node = f->evidence_top;
    while (node != NULL) {
        /* m8 bug #12: skip a node whose clause body is currently
         * being dispatched on *this* fiber. Per-fiber state because
         * spawned fibers share the parent's evidence stack — a flag
         * on the node would skip for every fiber, breaking m8x_2. */
        if (node != f->in_dispatch_node
            && (node->eff_label == eff_label
                || strcmp(node->eff_label, eff_label) == 0)) {
            return node;
        }
        node = node->parent;
    }
    return NULL;
}

/* m7b #15: per-instance dispatch — find the evidence node whose
 * handler_id matches `id`, no name match required. The codegen
 * uses this when the op call comes from a `with Eff as alias`
 * binding, so an outer alias's op stays reachable even after an
 * inner `with Eff as other` shadows the effect name. */
static KaiEvidence *kai_evidence_lookup_node_by_id(KaiHandlerId id) {
    kai_check_cancel_yield_point();
    KaiFiber *f = kai_current_fiber();
    KaiEvidence *node = f->evidence_top;
    while (node != NULL) {
        /* m8 bug #12: same per-fiber skip rule as the by-name lookup. */
        if (node == f->in_dispatch_node) { node = node->parent; continue; }
        KaiHandlerId nid = ((KaiHandlerId *) node->handler)[0];
        if (nid == id) {
            /* Issue #103 — same Cancel bypass as the by-name path:
             * an aliased `with Cancel as e` op call on a fiber linked
             * to a trap-exit'd peer must unwind directly. */
            if (node->eff_label
                && strcmp(node->eff_label, "Cancel") == 0) {
                kai_check_trap_exit_cancel_bypass();
            }
            return node;
        }
        node = node->parent;
    }
    return NULL;
}

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

#endif /* KAI_RUNTIME_H */
