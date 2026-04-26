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

#ifndef KAI_RUNTIME_H
#define KAI_RUNTIME_H

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static int       kai_truthy(KaiValue *v);

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

/* m8 #3: fiber lifecycle states. Inline-eager v1: every spawned
 * fiber goes NEW → DONE inside a single Spawn.spawn call (the thunk
 * runs synchronously). The state machine is here so #4 (Cancel)
 * and a future m8.x ucontext-based scheduler can extend it without
 * a layout change. */
typedef enum {
    KAI_FIBER_NEW       = 0,
    KAI_FIBER_DONE      = 1,
    KAI_FIBER_CANCELLED = 2
} KaiFiberState;

typedef struct KaiFiber KaiFiber;
struct KaiFiber {
    KaiEvidence *evidence_top;
    int             cancel_requested;  /* Spawn.cancel(target) sets this (#4) */
    int             cancel_delivered;  /* Cancel.raise() injected once (#4)   */
    KaiFiber       *sched_next;        /* intrusive ready-queue link          */
    KaiFiber       *parent;            /* spawning fiber, NULL for the root   */
    KaiFiberState   state;
    KaiValue       *thunk;             /* held alive while the fiber runs     */
    KaiValue       *result;            /* set on DONE; what await returns     */
};

/* m7a #5: single implicit fiber. m8 #3's inline-eager scheduler
 * still hands back the implicit fiber as "current" — every spawned
 * thunk runs to completion synchronously inside the spawning fiber,
 * so there is no real distinction yet. m8.x's ucontext or full-CPS
 * scheduler will replace the body of kai_current_fiber to return
 * the head of the run queue. */
static KaiFiber kai_main_fiber = { NULL, 0, 0, NULL, NULL, KAI_FIBER_NEW, NULL, NULL };

static KaiFiber *kai_current_fiber(void) {
    return &kai_main_fiber;
}

/* m8 #1: ready queue. Empty in v1 (every spawned thunk runs
 * immediately); the head/tail pair is here so the data-flow path is
 * the final shape for m8.x's real scheduler. */
static KaiFiber *kai_ready_head = NULL;
static KaiFiber *kai_ready_tail = NULL;

/* m8 #3: wrap a heap-allocated KaiFiber in an opaque KAI_FIBER
 * value. The KaiFiber struct's lifetime is tied to the value's RC
 * (kai_free_value frees both together). */
static KaiValue *kai_fiber_value(KaiFiber *f) {
    KaiValue *v = kai_alloc(KAI_FIBER);
    v->as.fib = f;
    return v;
}

/* m8 #7: mailbox runtime. A KaiMailbox is a singly-linked list of
 * heap-allocated KaiValue messages (head = next-to-pop, tail =
 * next-to-enqueue). Send pushes at the tail; receive pops the head.
 * v1 is unbounded — every send succeeds, every receive on an empty
 * mailbox is a runtime error (the inline-eager scheduler can't
 * suspend the caller until a message arrives). Bounded mailboxes
 * with the three overflow policies (DropOldest / DropNewest /
 * BlockSender) land in m8 #8 once Doc B's policy enum is wired
 * through the typer; BlockSender additionally needs the m8.x
 * cooperative scheduler to actually suspend the sender. */
typedef struct KaiMboxNode KaiMboxNode;
struct KaiMboxNode {
    KaiValue    *msg;
    KaiMboxNode *next;
};

/* m8 #8: mailbox overflow policy codes (matched in stdlib/actor.kai
 * by the MailboxPolicy enum). 0 = Unbounded, 1 = Bounded+DropOldest,
 * 2 = Bounded+DropNewest, 3 = Bounded+BlockSender. v1 ships 0/1/2;
 * BlockSender (3) errors at allocation because the inline-eager
 * scheduler can't suspend the sender on a full mailbox — that
 * lifts together with the m8.x cooperative scheduler. */
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
};

static KaiMailbox *kai_mailbox_alloc(void) {
    KaiMailbox *mb = (KaiMailbox *) calloc(1, sizeof(KaiMailbox));
    if (!mb) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    /* default policy: unbounded — matches m8 #7 behaviour. */
    mb->cap      = 0;
    mb->overflow = KAI_OVERFLOW_UNBOUNDED;
    return mb;
}

static KaiMailbox *kai_mailbox_alloc_bounded(int cap, int overflow) {
    KaiMailbox *mb = (KaiMailbox *) calloc(1, sizeof(KaiMailbox));
    if (!mb) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    if (overflow == KAI_OVERFLOW_BLOCK_SENDER) {
        fprintf(stderr, "kai: BlockSender mailbox policy requires the m8.x cooperative scheduler\n");
        exit(1);
    }
    mb->cap      = cap;
    mb->overflow = overflow;
    return mb;
}

static void kai_mailbox_push(KaiMailbox *mb, KaiValue *msg) {
    /* m8 #8: enforce policy on full. */
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
        }
        /* BlockSender is rejected at alloc, so no case here. */
    }
    KaiMboxNode *node = (KaiMboxNode *) calloc(1, sizeof(KaiMboxNode));
    if (!node) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    node->msg  = msg;
    node->next = NULL;
    if (mb->tail) { mb->tail->next = node; }
    else          { mb->head       = node; }
    mb->tail = node;
    mb->len++;
}

static KaiValue *kai_mailbox_pop(KaiMailbox *mb) {
    if (!mb->head) {
        fprintf(stderr, "kai: Actor.receive on empty mailbox (v1 inline-eager: no blocking)\n");
        exit(1);
    }
    KaiMboxNode *node = mb->head;
    mb->head = node->next;
    if (!mb->head) { mb->tail = NULL; }
    KaiValue *msg = node->msg;
    free(node);
    mb->len--;
    return msg;
}

static void kai_mailbox_free(KaiMailbox *mb) {
    if (!mb) return;
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
                kai_decref(v->as.fib->thunk);
                kai_decref(v->as.fib->result);
                free(v->as.fib);
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

static KaiValue *kai_int(int64_t i) {
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

static KaiValue *kai_field(KaiValue *rec, const char *name) {
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

/* ---------- equality ---------- */

static int kai_eq(KaiValue *a, KaiValue *b) {
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
            return kai_eq(a->as.cons.head, b->as.cons.head) &&
                   kai_eq(a->as.cons.tail, b->as.cons.tail);
        case KAI_VARIANT:
            if (a->as.var.variant_tag != b->as.var.variant_tag) return 0;
            if (a->as.var.n_args != b->as.var.n_args) return 0;
            for (int i = 0; i < a->as.var.n_args; ++i) {
                if (!kai_eq(a->as.var.args[i], b->as.var.args[i])) return 0;
            }
            return 1;
        case KAI_RECORD:
            if (a->as.rec.n_fields != b->as.rec.n_fields) return 0;
            for (int i = 0; i < a->as.rec.n_fields; ++i) {
                if (!kai_eq(a->as.rec.fields[i], b->as.rec.fields[i])) return 0;
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
    return kai_str(buf);
}

static KaiValue *kai_prelude_real_to_string(KaiValue *v) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", v->as.r);
    return kai_str(buf);
}

/* ---------- prelude: strings ---------- */

static KaiValue *kai_prelude_string_length(KaiValue *s) {
    int64_t n = (s && s->tag == KAI_STR) ? (int64_t) s->as.s.len : 0;
    return kai_int(n);
}

static KaiValue *kai_prelude_string_concat(KaiValue *a, KaiValue *b) {
    return kai_string_concat(a, b);
}

static KaiValue *kai_prelude_string_concat_all(KaiValue *xs) {
    return kai_string_concat_all_impl(xs);
}

static KaiValue *kai_prelude_string_join(KaiValue *xs, KaiValue *sep) {
    return kai_string_join_impl(xs, sep);
}

/* ---------- prelude: arrays ---------- */

static KaiValue *kai_prelude_array_make(KaiValue *n, KaiValue *init) {
    int64_t len = (n && n->tag == KAI_INT) ? n->as.i : 0;
    return kai_array_make(len, init);
}

static KaiValue *kai_prelude_array_length(KaiValue *a) {
    int64_t len = (a && a->tag == KAI_ARRAY) ? a->as.arr.len : 0;
    return kai_int(len);
}

static KaiValue *kai_prelude_array_get(KaiValue *a, KaiValue *i) {
    int64_t idx = (i && i->tag == KAI_INT) ? i->as.i : 0;
    return kai_array_get_impl(a, idx);
}

static KaiValue *kai_prelude_array_set(KaiValue *a, KaiValue *i, KaiValue *v) {
    int64_t idx = (i && i->tag == KAI_INT) ? i->as.i : 0;
    return kai_array_set_impl(a, idx, kai_incref(v));
}

static KaiValue *kai_prelude_array_grow(KaiValue *a, KaiValue *n, KaiValue *init) {
    int64_t new_len = (n && n->tag == KAI_INT) ? n->as.i : 0;
    return kai_array_grow_impl(a, new_len, init);
}

/* ---------- prelude: lists ---------- */

static KaiValue *kai_prelude_list_length(KaiValue *xs) {
    int64_t n = 0;
    KaiValue *p = xs;
    while (p && p->tag == KAI_CONS) { n++; p = p->as.cons.tail; }
    return kai_int(n);
}

static KaiValue *kai_prelude_list_append(KaiValue *xs, KaiValue *ys) {
    if (!xs || xs->tag == KAI_NIL) return kai_incref(ys);
    KaiValue *rest = kai_prelude_list_append(xs->as.cons.tail, ys);
    return kai_cons(kai_incref(xs->as.cons.head), rest);
}

static KaiValue *kai_prelude_list_reverse(KaiValue *xs) {
    KaiValue *acc = kai_nil();
    KaiValue *p   = xs;
    while (p && p->tag == KAI_CONS) {
        acc = kai_cons(kai_incref(p->as.cons.head), acc);
        p   = p->as.cons.tail;
    }
    return acc;
}

/* ---------- prelude: higher order ---------- */

static KaiValue *kai_prelude_map(KaiValue *xs, KaiValue *f) {
    if (!xs || xs->tag == KAI_NIL) return kai_nil();
    KaiValue *arg0 = xs->as.cons.head;
    KaiValue *head = kai_apply(f, 1, &arg0);
    KaiValue *rest = kai_prelude_map(xs->as.cons.tail, f);
    return kai_cons(head, rest);
}

static KaiValue *kai_prelude_filter(KaiValue *xs, KaiValue *p) {
    if (!xs || xs->tag == KAI_NIL) return kai_nil();
    KaiValue *arg0 = xs->as.cons.head;
    KaiValue *keep = kai_apply(p, 1, &arg0);
    int yes = kai_truthy(keep);
    kai_decref(keep);
    KaiValue *rest = kai_prelude_filter(xs->as.cons.tail, p);
    if (yes) return kai_cons(kai_incref(xs->as.cons.head), rest);
    return rest;
}

static KaiValue *kai_prelude_reduce(KaiValue *xs, KaiValue *init, KaiValue *f) {
    KaiValue *acc = kai_incref(init);
    KaiValue *p   = xs;
    while (p && p->tag == KAI_CONS) {
        KaiValue *args[2];
        args[0] = acc;
        args[1] = p->as.cons.head;
        KaiValue *next = kai_apply(f, 2, args);
        kai_decref(acc);
        acc = next;
        p = p->as.cons.tail;
    }
    return acc;
}

static KaiValue *kai_prelude_each(KaiValue *xs, KaiValue *f) {
    KaiValue *p = xs;
    while (p && p->tag == KAI_CONS) {
        KaiValue *arg0 = p->as.cons.head;
        KaiValue *r = kai_apply(f, 1, &arg0);
        kai_decref(r);
        p = p->as.cons.tail;
    }
    return kai_unit();
}

/* ---------- binary and unary operators ---------- */

static KaiValue *kai_add(KaiValue *a, KaiValue *b) {
    if (a->tag == KAI_INT  && b->tag == KAI_INT)  return kai_int(a->as.i + b->as.i);
    if (a->tag == KAI_REAL && b->tag == KAI_REAL) return kai_real(a->as.r + b->as.r);
    fprintf(stderr, "kai: type mismatch in +\n"); exit(1);
}

static KaiValue *kai_sub(KaiValue *a, KaiValue *b) {
    if (a->tag == KAI_INT  && b->tag == KAI_INT)  return kai_int(a->as.i - b->as.i);
    if (a->tag == KAI_REAL && b->tag == KAI_REAL) return kai_real(a->as.r - b->as.r);
    fprintf(stderr, "kai: type mismatch in -\n"); exit(1);
}

static KaiValue *kai_mul(KaiValue *a, KaiValue *b) {
    if (a->tag == KAI_INT  && b->tag == KAI_INT)  return kai_int(a->as.i * b->as.i);
    if (a->tag == KAI_REAL && b->tag == KAI_REAL) return kai_real(a->as.r * b->as.r);
    fprintf(stderr, "kai: type mismatch in *\n"); exit(1);
}

static KaiValue *kai_div(KaiValue *a, KaiValue *b) {
    if (a->tag == KAI_INT && b->tag == KAI_INT) {
        if (b->as.i == 0) { fprintf(stderr, "kai: divide by zero\n"); exit(1); }
        return kai_int(a->as.i / b->as.i);
    }
    if (a->tag == KAI_REAL && b->tag == KAI_REAL) return kai_real(a->as.r / b->as.r);
    fprintf(stderr, "kai: type mismatch in /\n"); exit(1);
}

static KaiValue *kai_idiv(KaiValue *a, KaiValue *b) {
    int64_t av = 0, bv = 0;
    if      (a->tag == KAI_INT)  av = a->as.i;
    else if (a->tag == KAI_REAL) av = (int64_t) a->as.r;
    else { fprintf(stderr, "kai: type mismatch in //\n"); exit(1); }
    if      (b->tag == KAI_INT)  bv = b->as.i;
    else if (b->tag == KAI_REAL) bv = (int64_t) b->as.r;
    else { fprintf(stderr, "kai: type mismatch in //\n"); exit(1); }
    if (bv == 0) { fprintf(stderr, "kai: divide by zero\n"); exit(1); }
    return kai_int(av / bv);
}

static KaiValue *kai_mod(KaiValue *a, KaiValue *b) {
    if (a->tag == KAI_INT && b->tag == KAI_INT) {
        if (b->as.i == 0) { fprintf(stderr, "kai: mod by zero\n"); exit(1); }
        return kai_int(a->as.i % b->as.i);
    }
    fprintf(stderr, "kai: type mismatch in %%\n"); exit(1);
}

static KaiValue *kai_lt(KaiValue *a, KaiValue *b) {
    if (a->tag == KAI_INT  && b->tag == KAI_INT)  return kai_bool(a->as.i < b->as.i);
    if (a->tag == KAI_REAL && b->tag == KAI_REAL) return kai_bool(a->as.r < b->as.r);
    if (a->tag == KAI_CHAR && b->tag == KAI_CHAR) return kai_bool(a->as.c < b->as.c);
    if (a->tag == KAI_STR  && b->tag == KAI_STR) {
        size_t n = a->as.s.len < b->as.s.len ? a->as.s.len : b->as.s.len;
        int c = memcmp(a->as.s.bytes, b->as.s.bytes, n);
        if (c != 0) return kai_bool(c < 0);
        return kai_bool(a->as.s.len < b->as.s.len);
    }
    fprintf(stderr, "kai: type mismatch in <\n"); exit(1);
}

static KaiValue *kai_gt(KaiValue *a, KaiValue *b) {
    if (a->tag == KAI_INT  && b->tag == KAI_INT)  return kai_bool(a->as.i > b->as.i);
    if (a->tag == KAI_REAL && b->tag == KAI_REAL) return kai_bool(a->as.r > b->as.r);
    if (a->tag == KAI_CHAR && b->tag == KAI_CHAR) return kai_bool(a->as.c > b->as.c);
    if (a->tag == KAI_STR  && b->tag == KAI_STR) {
        size_t n = a->as.s.len < b->as.s.len ? a->as.s.len : b->as.s.len;
        int c = memcmp(a->as.s.bytes, b->as.s.bytes, n);
        if (c != 0) return kai_bool(c > 0);
        return kai_bool(a->as.s.len > b->as.s.len);
    }
    fprintf(stderr, "kai: type mismatch in >\n"); exit(1);
}

static KaiValue *kai_le(KaiValue *a, KaiValue *b) {
    KaiValue *g = kai_gt(a, b);
    KaiValue *r = kai_bool(!g->as.b);
    kai_decref(g);
    return r;
}

static KaiValue *kai_ge(KaiValue *a, KaiValue *b) {
    KaiValue *l = kai_lt(a, b);
    KaiValue *r = kai_bool(!l->as.b);
    kai_decref(l);
    return r;
}

static KaiValue *kai_eq_v(KaiValue *a, KaiValue *b) { return kai_bool(kai_eq(a, b)); }
static KaiValue *kai_ne_v(KaiValue *a, KaiValue *b) { return kai_bool(!kai_eq(a, b)); }

static KaiValue *kai_neg(KaiValue *a) {
    if (a->tag == KAI_INT)  return kai_int(-a->as.i);
    if (a->tag == KAI_REAL) return kai_real(-a->as.r);
    fprintf(stderr, "kai: type mismatch in unary -\n"); exit(1);
}

static KaiValue *kai_boolnot(KaiValue *a) {
    if (a->tag == KAI_BOOL) return kai_bool(!a->as.b);
    fprintf(stderr, "kai: type mismatch in `not`\n"); exit(1);
}

static int kai_truthy(KaiValue *v) {
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

/* ---------- prelude: mailbox runtime (m8 #7) ---------- */

/* User code reaches the mailbox runtime through these prelude
 * functions. They are wrapped in stdlib/actor.kai's `with_mailbox`
 * helper, which also installs the user-facing Actor[Msg] handler.
 * The polymorphic surface uses Nothing → TyAny so a single set of
 * runtime entries serves every Msg type. */

static KaiValue *kai_prelude_mailbox_alloc(void) {
    return kai_pid_value(kai_mailbox_alloc());
}

static KaiValue *kai_prelude_mailbox_alloc_bounded(KaiValue *cap, KaiValue *overflow) {
    int c = (cap && cap->tag == KAI_INT) ? (int) cap->as.i : 0;
    int o = (overflow && overflow->tag == KAI_INT) ? (int) overflow->as.i : 0;
    return kai_pid_value(kai_mailbox_alloc_bounded(c, o));
}

static KaiValue *kai_prelude_mailbox_send(KaiValue *pid, KaiValue *msg) {
    if (!pid || pid->tag != KAI_PID || !pid->as.mb) {
        fprintf(stderr, "kai: mailbox_send: argument is not a Pid\n");
        exit(1);
    }
    kai_mailbox_push(pid->as.mb, kai_incref(msg));
    return kai_unit();
}

static KaiValue *kai_prelude_mailbox_recv(KaiValue *pid) {
    if (!pid || pid->tag != KAI_PID || !pid->as.mb) {
        fprintf(stderr, "kai: mailbox_recv: argument is not a Pid\n");
        exit(1);
    }
    return kai_mailbox_pop(pid->as.mb);
}

/* Free the mailbox attached to a Pid. Called by `with_mailbox` when
 * the scope exits; the Pid value itself is RC-managed independently. */
static KaiValue *kai_prelude_mailbox_free(KaiValue *pid) {
    if (pid && pid->tag == KAI_PID && pid->as.mb) {
        kai_mailbox_free(pid->as.mb);
        pid->as.mb = NULL;
    }
    return kai_unit();
}

/* ---------- prelude: file io ---------- */

static KaiValue *kai_prelude_read_file(KaiValue *path) {
    if (!path || path->tag != KAI_STR) {
        KaiValue *msg = kai_str("read_file: argument is not a String");
        KaiValue *err = kai_variant(0, "Err", 1, &msg);
        return err;
    }
    char pbuf[4096];
    size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
    memcpy(pbuf, path->as.s.bytes, plen);
    pbuf[plen] = '\0';
    FILE *fp = fopen(pbuf, "rb");
    if (!fp) {
        KaiValue *msg = kai_str("read_file: cannot open file");
        return kai_variant(0, "Err", 1, &msg);
    }
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp);
        KaiValue *msg = kai_str("read_file: seek failed");
        return kai_variant(0, "Err", 1, &msg); }
    long n = ftell(fp);
    if (n < 0) { fclose(fp);
        KaiValue *msg = kai_str("read_file: tell failed");
        return kai_variant(0, "Err", 1, &msg); }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp);
        KaiValue *msg = kai_str("read_file: rewind failed");
        return kai_variant(0, "Err", 1, &msg); }
    KaiValue *v = kai_alloc(KAI_STR);
    v->as.s.len = (size_t) n;
    v->as.s.bytes = (char *) malloc((size_t) n + 1);
    if (!v->as.s.bytes) { fclose(fp); fprintf(stderr, "kai: out of memory\n"); exit(1); }
    size_t got = fread(v->as.s.bytes, 1, (size_t) n, fp);
    fclose(fp);
    v->as.s.bytes[got] = '\0';
    v->as.s.len = got;
    return kai_variant(0, "Ok", 1, &v);
}

static KaiValue *kai_prelude_write_file(KaiValue *path, KaiValue *content) {
    if (!path || path->tag != KAI_STR) {
        KaiValue *msg = kai_str("write_file: path is not a String");
        return kai_variant(0, "Err", 1, &msg);
    }
    if (!content || content->tag != KAI_STR) {
        KaiValue *msg = kai_str("write_file: content is not a String");
        return kai_variant(0, "Err", 1, &msg);
    }
    char pbuf[4096];
    size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
    memcpy(pbuf, path->as.s.bytes, plen);
    pbuf[plen] = '\0';
    FILE *fp = fopen(pbuf, "wb");
    if (!fp) {
        KaiValue *msg = kai_str("write_file: cannot open file");
        return kai_variant(0, "Err", 1, &msg);
    }
    size_t wrote = fwrite(content->as.s.bytes, 1, content->as.s.len, fp);
    fclose(fp);
    if (wrote != content->as.s.len) {
        KaiValue *msg = kai_str("write_file: short write");
        return kai_variant(0, "Err", 1, &msg);
    }
    KaiValue *u = kai_unit();
    return kai_variant(0, "Ok", 1, &u);
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

static KaiValue *kai_prelude_string_to_int(KaiValue *s) {
    if (!s || s->tag != KAI_STR || s->as.s.len == 0) {
        return kai_variant(0, "None", 0, NULL);
    }
    char buf[64];
    if (s->as.s.len >= sizeof(buf)) {
        return kai_variant(0, "None", 0, NULL);
    }
    memcpy(buf, s->as.s.bytes, s->as.s.len);
    buf[s->as.s.len] = '\0';
    char *end = NULL;
    long long v = strtoll(buf, &end, 10);
    if (!end || *end != '\0' || end == buf) {
        return kai_variant(0, "None", 0, NULL);
    }
    KaiValue *iv = kai_int((int64_t) v);
    return kai_variant(0, "Some", 1, &iv);
}

static KaiValue *kai_prelude_string_to_real(KaiValue *s) {
    if (!s || s->tag != KAI_STR || s->as.s.len == 0) {
        return kai_variant(0, "None", 0, NULL);
    }
    char buf[64];
    if (s->as.s.len >= sizeof(buf)) {
        return kai_variant(0, "None", 0, NULL);
    }
    memcpy(buf, s->as.s.bytes, s->as.s.len);
    buf[s->as.s.len] = '\0';
    char *end = NULL;
    double v = strtod(buf, &end);
    if (!end || *end != '\0' || end == buf) {
        return kai_variant(0, "None", 0, NULL);
    }
    KaiValue *rv = kai_real(v);
    return kai_variant(0, "Some", 1, &rv);
}

static KaiValue *kai_prelude_char_at(KaiValue *s, KaiValue *i) {
    if (!s || s->tag != KAI_STR || !i || i->tag != KAI_INT) {
        return kai_variant(0, "None", 0, NULL);
    }
    int64_t idx = i->as.i;
    if (idx < 0 || (size_t) idx >= s->as.s.len) {
        return kai_variant(0, "None", 0, NULL);
    }
    /* Byte-at semantics for now; multi-byte UTF-8 can return surrogate-like
       codepoints once we need them. Stage 0 keeps it simple. */
    KaiValue *cv = kai_char((uint32_t)(unsigned char) s->as.s.bytes[idx]);
    return kai_variant(0, "Some", 1, &cv);
}

static KaiValue *kai_prelude_string_split(KaiValue *s, KaiValue *sep) {
    if (!s || s->tag != KAI_STR) return kai_nil();
    if (!sep || sep->tag != KAI_STR || sep->as.s.len == 0) {
        return kai_cons(kai_incref(s), kai_nil());
    }
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
    KaiValue *acc = kai_nil();
    for (size_t k = n; k > 0;) {
        --k;
        acc = kai_cons(kai_str_from_bytes(pieces[k].b, pieces[k].l), acc);
    }
    free(pieces);
    return acc;
}

static KaiValue *kai_prelude_string_slice(KaiValue *s, KaiValue *from, KaiValue *len) {
    if (!s || s->tag != KAI_STR) return kai_str("");
    int64_t f = (from && from->tag == KAI_INT) ? from->as.i : 0;
    int64_t l = (len  && len->tag  == KAI_INT) ? len->as.i  : 0;
    if (f < 0) f = 0;
    if (l < 0) l = 0;
    if ((size_t) f > s->as.s.len) f = (int64_t) s->as.s.len;
    size_t avail = s->as.s.len - (size_t) f;
    size_t take  = ((size_t) l > avail) ? avail : (size_t) l;
    return kai_str_from_bytes(s->as.s.bytes + f, take);
}

static KaiValue *kai_prelude_char_to_int(KaiValue *c) {
    if (!c || c->tag != KAI_CHAR) return kai_int(0);
    return kai_int((int64_t) c->as.c);
}

static KaiValue *kai_prelude_int_to_char(KaiValue *n) {
    if (!n || n->tag != KAI_INT) return kai_char(0);
    return kai_char((uint32_t) n->as.i);
}

static KaiValue *kai_prelude_string_contains(KaiValue *s, KaiValue *sub) {
    if (!s || s->tag != KAI_STR || !sub || sub->tag != KAI_STR) return kai_bool(0);
    if (sub->as.s.len == 0) return kai_bool(1);
    if (sub->as.s.len > s->as.s.len) return kai_bool(0);
    for (size_t i = 0; i + sub->as.s.len <= s->as.s.len; ++i) {
        if (memcmp(s->as.s.bytes + i, sub->as.s.bytes, sub->as.s.len) == 0) return kai_bool(1);
    }
    return kai_bool(0);
}

/* ---------- prelude thunks for first-class function refs ---------- */

static KaiValue *_kai_prelude_print_thunk(KaiValue *s, KaiValue **a, int n)          { (void) s; (void) n; return kai_prelude_print(a[0]); }
static KaiValue *_kai_prelude_eprint_thunk(KaiValue *s, KaiValue **a, int n)         { (void) s; (void) n; return kai_prelude_eprint(a[0]); }
static KaiValue *_kai_prelude_panic_thunk(KaiValue *s, KaiValue **a, int n)          { (void) s; (void) n; return kai_prelude_panic(a[0]); }
static KaiValue *_kai_prelude_exit_thunk(KaiValue *s, KaiValue **a, int n)           { (void) s; (void) n; return kai_prelude_exit(a[0]); }
static KaiValue *_kai_prelude_int_to_string_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_prelude_int_to_string(a[0]); }
static KaiValue *_kai_prelude_real_to_string_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_real_to_string(a[0]); }
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
static KaiValue *_kai_prelude_filter_thunk(KaiValue *s, KaiValue **a, int n)         { (void) s; (void) n; return kai_prelude_filter(a[0], a[1]); }
static KaiValue *_kai_prelude_reduce_thunk(KaiValue *s, KaiValue **a, int n)         { (void) s; (void) n; return kai_prelude_reduce(a[0], a[1], a[2]); }
static KaiValue *_kai_prelude_each_thunk(KaiValue *s, KaiValue **a, int n)           { (void) s; (void) n; return kai_prelude_each(a[0], a[1]); }
static KaiValue *_kai_prelude_args_thunk(KaiValue *s, KaiValue **a, int n)           { (void) s; (void) a; (void) n; return kai_prelude_args(); }
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

/* Runtime-aware assert: called from every emitted `assert`. Inside an
   active test (kai_test_in_progress) it prints a failure and longjmps
   back to the test harness so the next test can run. Otherwise it
   aborts the process via kai_prelude_panic, matching stage 0's
   non-test-mode behaviour. */
static void kai_assert_check(KaiValue *cond, const char *msg) {
    int ok = kai_truthy(cond);
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

/* m7a #7: default Console handler clauses. Both write the string
 * + a trailing '\n' and resume with `()`. *Self is opaque to the
 * runtime because `EvConsole` is a compiler-emitted type — the
 * cast happens at the assignment in the main wrapper.
 *
 * EPIPE absorption (Doc B §`Console`/Default handler) is later
 * polish — for now any write fault propagates through fputs's
 * default behaviour. The minimal-but-useful path lands first. */
static KaiValue *kai_default_console_print(void *self, KaiValue *s, KaiCont *k) {
    (void) self;
    if (s && s->tag == KAI_STR) {
        fwrite(s->as.s.bytes, 1, s->as.s.len, stdout);
    }
    fputc('\n', stdout);
    return kai_cont_resume(k, kai_unit());
}

static KaiValue *kai_default_console_eprint(void *self, KaiValue *s, KaiCont *k) {
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

/* m8 #2/#3: default Spawn handlers. Inline-eager v1: spawn(thunk)
 * runs the thunk synchronously, stores the result in a heap KaiFiber,
 * and returns a KAI_FIBER handle; await(fib) returns the cached
 * result; yield is a no-op (the implicit single fiber has nowhere to
 * yield to); select picks the first fiber's result; cancel sets a
 * flag (delivery in m8 #4). A future m8.x replaces this with a real
 * cooperative scheduler whose surface is identical (same evidence
 * struct, same op signatures). */
static KaiValue *kai_default_spawn_yield(void *self, KaiCont *k) {
    (void) self;
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
    f->parent = kai_current_fiber();
    f->state  = KAI_FIBER_NEW;
    f->thunk  = kai_incref(thunk);
    /* Inline-eager: invoke the thunk now. m8.x replaces this with
     * an enqueue + scheduler dispatch. */
    f->result = kai_apply(thunk, 0, NULL);
    f->state  = KAI_FIBER_DONE;
    return kai_cont_resume(k, kai_fiber_value(f));
}

static KaiValue *kai_default_spawn_await(void *self, KaiValue *fib_v, KaiCont *k) {
    (void) self;
    if (!fib_v || fib_v->tag != KAI_FIBER || !fib_v->as.fib) {
        fprintf(stderr, "kai: Spawn.await called on non-fiber value\n");
        exit(1);
    }
    KaiFiber *f = fib_v->as.fib;
    if (f->state != KAI_FIBER_DONE) {
        /* Should be unreachable in v1: spawn always finishes before
         * returning. m8.x will block the caller and resume on
         * completion. */
        fprintf(stderr, "kai: Spawn.await on incomplete fiber (v1 inline-eager)\n");
        exit(1);
    }
    return kai_cont_resume(k, kai_incref(f->result));
}

static KaiValue *kai_default_spawn_select(void *self, KaiValue *fibs_v, KaiCont *k) {
    (void) self;
    /* Argument is a [Fiber[T]] — a KAI_CONS list of KAI_FIBER. v1:
     * pick the head and return its result. m8.x cancels the losers
     * and picks whichever finishes first. */
    if (!fibs_v || fibs_v->tag != KAI_CONS) {
        fprintf(stderr, "kai: Spawn.select called on empty or non-list value\n");
        exit(1);
    }
    KaiValue *first = fibs_v->as.cons.head;
    if (!first || first->tag != KAI_FIBER || !first->as.fib) {
        fprintf(stderr, "kai: Spawn.select: list head is not a fiber\n");
        exit(1);
    }
    return kai_cont_resume(k, kai_incref(first->as.fib->result));
}

static KaiValue *kai_default_spawn_cancel(void *self, KaiValue *fib_v, KaiCont *k) {
    (void) self;
    if (fib_v && fib_v->tag == KAI_FIBER && fib_v->as.fib) {
        fib_v->as.fib->cancel_requested = 1;
        /* m8 #4 records the flag; the cooperative-yield delivery
         * (inject Cancel.raise() at the next op-call boundary on
         * that fiber) lands together with the m8.x ucontext or
         * full-CPS scheduler. In the inline-eager v1, the target
         * fiber is already DONE by the time cancel runs, so the
         * flag is harmless — Spawn.cancel + Cancel still type-
         * check correctly so user code can be written against the
         * final API. */
    }
    return kai_cont_resume(k, kai_unit());
}

/* m8 #4: default Cancel.raise handler. Doc B §`Cancel`/Default
 * handler: an unhandled Cancel.raise() unwinds the fiber cleanly
 * (the runtime delivers "no silent survivors"). v1 mirrors Fail's
 * default: write a banner + exit(0). A user-installed handler
 * (`with Cancel { raise(resume) -> cleanup }`) discards resume and
 * the m7a #6e setjmp/longjmp pair unwinds to the wrapping handle —
 * that is the actual "fiber unwinds out of the wrapped block"
 * semantics from Doc B §*Handling for cleanup*. The exit code is 0
 * (not 1 like Fail) because cancellation is an expected termination
 * path, not a programmer error. */
static KaiValue *kai_default_cancel_raise(void *self, KaiCont *k) {
    (void) self;
    (void) k;
    fputs("kai: Cancel.raise: unhandled (fiber cancelled)\n", stderr);
    exit(0);
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
static void *kai_evidence_lookup(const char *eff_label) {
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
    KaiFiber *f = kai_current_fiber();
    KaiEvidence *node = f->evidence_top;
    while (node != NULL) {
        if (node->eff_label == eff_label
            || strcmp(node->eff_label, eff_label) == 0) {
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
    KaiFiber *f = kai_current_fiber();
    KaiEvidence *node = f->evidence_top;
    while (node != NULL) {
        KaiHandlerId nid = ((KaiHandlerId *) node->handler)[0];
        if (nid == id) {
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
