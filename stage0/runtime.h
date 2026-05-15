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

#include <dirent.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* net-tcp-v1 — sockets API for the NetTcp default handler.
 * POSIX everywhere we ship: macOS, Linux, *BSD. The handler is
 * blocking-only: the m8.x cooperative scheduler (landed v0.4.0)
 * suspends fibers on mailbox / await / yield, but we have no
 * readiness reactor yet, so socket reads/writes park the OS thread
 * rather than the fiber. kqueue / epoll integration is a Tier 2
 * follow-up tracked in docs/fibers-honesty-targets.md §Reactor. */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

/* Issue #611 — Phase R1 reactor support. `poll()` is the wait
 * primitive (POSIX everywhere we ship), `pthread` powers the file
 * I/O worker pool, and `fcntl` toggles O_NONBLOCK on the self-pipes
 * so the SIGCHLD handler and worker threads never block on write.
 * Sockets stay on the blocking path until R2. */
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>

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
    KAI_PID,        /* m8 #7: Actor[Msg] / Pid[Msg] handle (opaque) */
    KAI_BYTE          /* Lane 4 (#473): unsigned 8-bit integer, nominal */
} KaiTag;

typedef struct KaiValue KaiValue;

/* Dynamic-dispatch signature used for closures and higher-order calls. */
typedef KaiValue *(*KaiFn)(KaiValue *self, KaiValue **args, int n_args);

/* Issue #440 — variant payload slot. One machine word. Mask bits in
 * `as.var.slot_mask` discriminate per-slot kind; see the `var`
 * substructure below for the encoding. Binary-compatible with
 * `KaiValue *` so legacy pointer-only callers (stage 0/1 emit,
 * immortal-variant cache hash/match, reuse-in-place memcmp) keep
 * working unchanged when mask=0. */
typedef union {
    KaiValue *ptr;
    int64_t   i64;
    double    r;
    uint32_t  c;
    int8_t    b;
} KaiVarSlot;

/* Issue #440 Phase 2 — slot kind decoder. 2 bits per slot in
 * `slot_mask`; bit pair at position (2*i) encodes slot i's kind:
 *   0 = pointer (KaiValue *) — legacy
 *   1 = Int (int64_t, .i64)
 *   2 = Real (double, .r)
 *   3 = reserved
 * Variants with >16 slots fall back to mask=0 (all pointer) since the
 * encoding exhausts the 32-bit mask. */
#define KAI_VAR_SLOT_PTR  0u
#define KAI_VAR_SLOT_INT  1u
#define KAI_VAR_SLOT_REAL 2u

static inline uint32_t kai_var_slot_kind(uint32_t mask, int i) {
    return (mask >> (2 * (uint32_t) i)) & 3u;
}

struct KaiValue {
    int32_t rc;
    int32_t tag;
#ifdef KAI_TRACE_RC
    /* issue #296 — call-site attribution. Captured at kai_alloc as
     * __builtin_return_address(0); decremented from the per-site
     * histogram at kai_free_value. Only present under -DKAI_TRACE_RC=1
     * (vanilla builds keep the original two-word header — runtime
     * layout is otherwise unchanged). */
    void   *alloc_site;
#endif
#ifdef KAI_TRACE_RC_LEAKSITE
    /* Lane DIAG (#293) — kaikai-source attribution. Captured at
     * kai_alloc as the value of `kai_current_scope_fn`, set by the
     * stage-1 emitter at the top of every generated kaikai function
     * via `kai_set_scope_fn(<name>)`. Pairs with `alloc_site`
     * (return-address) to map every leaked chunk to the kaikai fn
     * whose body emitted (or should have emitted) the matching
     * decref. Only present under -DKAI_TRACE_RC_LEAKSITE=1. */
    const char *scope_fn;
#endif
    union {
        int      b;                                 /* KAI_BOOL */
        int64_t  i;                                 /* KAI_INT */
        double   r;                                 /* KAI_REAL */
        uint32_t c;                                 /* KAI_CHAR */
        uint8_t  byte_val;                                /* KAI_BYTE — Lane 4 (#473) */
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
            /* Issue #440 — variant slot abstraction. Each slot is one
             * machine word (8 bytes on 64-bit). When `slot_mask` is 0
             * (legacy and current default), every slot stores a
             * `KaiValue *` and the runtime walks them as boxed
             * children. Phase 2 of #440 will encode primitive
             * (Int/Bool/Char/Real) payloads inline via 2-bit kinds in
             * `slot_mask` — bits 2i..2i+1 give the kind of slot i:
             *   0 = pointer (KaiValue *)
             *   1 = Int     (int64_t,  read via .i64)
             *   2 = Bool    (int8_t,   read via .b)
             *   3 = Real    (double,   read via .r)
             * Variants with >16 slots fall back to mask=0.
             * For Phase 1 (this lane) every constructor still emits
             * mask=0 and slot.ptr is read identically to the old
             * `args[i]`. KaiVarSlot is binary-compatible with
             * `KaiValue *` so cache hashing, immortal-variant lookup
             * and Perceus reuse continue to compare pointer-by-pointer
             * unchanged. */
            uint32_t    slot_mask;
            KaiVarSlot *slots;
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
        case KAI_BYTE:      return "Byte";
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

/* Strict alloc tracing — Track #2 of issue #291.
 *
 * Build with `-DKAI_TRACE_RC=1` to enable diagnostics that surface RC
 * imbalances on macOS the same way glibc's tcache strict check does on
 * Linux (`malloc(): unaligned tcache chunk detected`). Without the
 * flag, the runtime keeps the cheap always-on counters above and adds
 * zero overhead.
 *
 * Three facilities, layered:
 *
 * 1. Per-tag free counters (kai_rc_free_by_tag). Pairs with
 *    kai_rc_alloc_by_tag so the report can show allocs/frees/live
 *    per tag at exit. live != 0 → leak. frees > allocs → double-free.
 *
 * 2. Sentinel-on-free. Just before free(v) in kai_free_value the
 *    chunk's bytes are stamped with 0xDEADBEEFDEADBEEF. macOS's
 *    malloc happily reuses the chunk, but any read through a stale
 *    pointer that was already decref'd surfaces the recognizable
 *    poison pattern (e.g. `tag = 0xEFBE...`, `rc = 0xDEADBEEF`)
 *    instead of stale-but-plausible content.
 *
 * 3. Optional per-chunk history (KAI_RC_HISTORY=1 env var). Records
 *    a small ring buffer of (chunk, op, tag) tuples covering alloc /
 *    incref / decref / free. Heavy — opt-in only — but lets a post
 *    mortem trace which tag's RC drifted.
 *
 * Reporting:
 * - Without -DKAI_TRACE_RC=1: existing env-gated KAI_TRACE_RC=1
 *   report (allocs only).
 * - With -DKAI_TRACE_RC=1: report fires unconditionally at exit
 *   unless KAI_TRACE_RC_QUIET=1 is set; per-tag allocs, frees, and
 *   live are all printed.
 */
#ifdef KAI_TRACE_RC

static int64_t kai_rc_free_by_tag[16] = {0};

#define KAI_RC_SENTINEL_U64 ((uint64_t) 0xDEADBEEFDEADBEEFULL)

/* Optional per-chunk history. Ring buffer; KAI_RC_HISTORY=1 enables
 * recording. Capacity is generous enough for the kaic2 self-compile
 * working set without ballooning memory. Indexed by (counter %
 * KAI_RC_HISTORY_CAP) so older entries get overwritten. */
#define KAI_RC_HISTORY_CAP 65536
typedef struct {
    void   *chunk;
    int32_t op;     /* 0=alloc, 1=incref, 2=decref, 3=free */
    int32_t tag;
} KaiRcHistoryEntry;
static KaiRcHistoryEntry kai_rc_history[KAI_RC_HISTORY_CAP];
static uint64_t kai_rc_history_count = 0;
static int      kai_rc_history_enabled_cached = -1; /* lazy: -1 unread, 0 off, 1 on */

static int kai_rc_history_enabled(void) {
    if (kai_rc_history_enabled_cached < 0) {
        const char *e = getenv("KAI_RC_HISTORY");
        kai_rc_history_enabled_cached = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return kai_rc_history_enabled_cached;
}

static void kai_rc_history_log(void *chunk, int32_t op, int32_t tag) {
    if (!kai_rc_history_enabled()) return;
    uint64_t i = kai_rc_history_count++ % KAI_RC_HISTORY_CAP;
    kai_rc_history[i].chunk = chunk;
    kai_rc_history[i].op    = op;
    kai_rc_history[i].tag   = tag;
}

static const char *kai_rc_op_name(int32_t op) {
    switch (op) {
        case 0: return "alloc";
        case 1: return "incref";
        case 2: return "decref";
        case 3: return "free";
        default: return "?";
    }
}

static void kai_rc_strict_report(void) {
    if (getenv("KAI_TRACE_RC_QUIET")) return;
    int64_t leaked = kai_rc_alloc_total - kai_rc_free_total;
    fprintf(stderr,
        "[KAI_TRACE_RC] STRICT alloc_total=%lld free_total=%lld leaked=%lld live_peak=%lld\n",
        (long long) kai_rc_alloc_total,
        (long long) kai_rc_free_total,
        (long long) leaked,
        (long long) kai_rc_live_peak);
    for (int i = 0; i < 16; i++) {
        int64_t a = kai_rc_alloc_by_tag[i];
        int64_t f = kai_rc_free_by_tag[i];
        if (a == 0 && f == 0) continue;
        int64_t live = a - f;
        const char *flag =
            (live != 0)              ? " LEAK"     :
            (f > a)                  ? " DOUBLE"   : "";
        fprintf(stderr,
            "[KAI_TRACE_RC] tag=%-7s allocs=%lld frees=%lld live=%lld%s\n",
            kai_rc_tag_name(i),
            (long long) a, (long long) f, (long long) live, flag);
    }
    if (kai_rc_history_enabled() && kai_rc_history_count > 0) {
        uint64_t total = kai_rc_history_count;
        uint64_t shown = total < KAI_RC_HISTORY_CAP ? total : KAI_RC_HISTORY_CAP;
        fprintf(stderr,
            "[KAI_TRACE_RC] history total=%llu showing last=%llu\n",
            (unsigned long long) total, (unsigned long long) shown);
        uint64_t start = total > KAI_RC_HISTORY_CAP ? total - KAI_RC_HISTORY_CAP : 0;
        for (uint64_t k = start; k < total; k++) {
            KaiRcHistoryEntry *e = &kai_rc_history[k % KAI_RC_HISTORY_CAP];
            fprintf(stderr, "[KAI_TRACE_RC] hist %s chunk=%p tag=%s\n",
                kai_rc_op_name(e->op), e->chunk,
                kai_rc_tag_name(e->tag));
        }
    }
}

static int kai_rc_strict_registered = 0;
static void kai_rc_strict_register_once(void) {
    if (kai_rc_strict_registered) return;
    kai_rc_strict_registered = 1;
    atexit(kai_rc_strict_report);
}

/* ---------- issue #296: per-call-site leak attribution ----------
 *
 * Open-addressing hash table from caller return-address to per-site
 * (alloc, free) counters. Keyed by `void *` (the return address of
 * the kai_alloc invocation, captured via __builtin_return_address(0)
 * inside each wrapper that calls kai_alloc — wrappers are marked
 * KAI_RC_NOINLINE so the address is the real emit site, not an
 * inlined parent).
 *
 * Sized at 16 K buckets — empirically the kaic2 selfhost emits
 * fewer than ~3 K distinct alloc sites, so load factor stays under
 * 20 %. Linear probing; the table is never resized. Saturates if
 * full (drops the site silently — the histogram becomes lossy
 * rather than corrupt).
 *
 * Per-chunk attribution: the alloc site is stored in the chunk's
 * `alloc_site` field (added under #ifdef KAI_TRACE_RC at the top
 * of struct KaiValue). On free, kai_free_value reads it back to
 * decrement the matching site's free counter. Cost: one extra word
 * per chunk under KAI_TRACE_RC (vanilla builds keep the original
 * two-word header). */

#define KAI_RC_SITE_BUCKETS 16384

typedef struct {
    void   *site;     /* return address; NULL = empty bucket */
    int32_t tag;      /* dominant tag observed at this site */
    int64_t allocs;
    int64_t frees;
} KaiRcSite;

static KaiRcSite kai_rc_sites[KAI_RC_SITE_BUCKETS];
static int       kai_rc_sites_count = 0;
static int       kai_rc_sites_full  = 0;  /* sticky: a probe overflowed */

/* Hash a pointer using a fast mix (Knuth-style). The high bits of
 * a return address carry the most variance, so we shift before
 * masking. */
static uint32_t kai_rc_site_hash(void *p) {
    uintptr_t x = (uintptr_t) p;
    x ^= x >> 33;
    x *= (uintptr_t) 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (uint32_t) (x & (KAI_RC_SITE_BUCKETS - 1));
}

/* Find or insert. Returns NULL on overflow (table saturated).
 * Otherwise returns a pointer to the bucket (caller may mutate). */
static KaiRcSite *kai_rc_site_lookup(void *site, int32_t tag, int insert) {
    if (site == NULL) return NULL;
    uint32_t mask = KAI_RC_SITE_BUCKETS - 1;
    uint32_t h = kai_rc_site_hash(site);
    for (uint32_t i = 0; i < KAI_RC_SITE_BUCKETS; i++) {
        uint32_t k = (h + i) & mask;
        KaiRcSite *b = &kai_rc_sites[k];
        if (b->site == site) return b;
        if (b->site == NULL) {
            if (!insert) return NULL;
            b->site = site;
            b->tag  = tag;
            kai_rc_sites_count++;
            return b;
        }
    }
    kai_rc_sites_full = 1;
    return NULL;
}

static void kai_rc_site_record_alloc(void *site, int32_t tag) {
    KaiRcSite *b = kai_rc_site_lookup(site, tag, 1);
    if (b) b->allocs++;
}

static void kai_rc_site_record_free(void *site) {
    KaiRcSite *b = kai_rc_site_lookup(site, 0, 0);
    if (b) b->frees++;
}

/* qsort comparator: descending by leak count (allocs - frees). */
static int kai_rc_site_cmp_leak(const void *a, const void *b) {
    const KaiRcSite *x = (const KaiRcSite *) a;
    const KaiRcSite *y = (const KaiRcSite *) b;
    int64_t lx = x->allocs - x->frees;
    int64_t ly = y->allocs - y->frees;
    if (ly > lx) return 1;
    if (ly < lx) return -1;
    return 0;
}

/* On macOS, addresses captured via __builtin_return_address(0) are
 * post-ASLR. Print the dyld slide so post-mortem symbolization with
 * `atos -l <load>` can recover symbol names. On other platforms the
 * slide is treated as 0. */
#ifdef __APPLE__
#include <mach-o/dyld.h>
static intptr_t kai_rc_aslr_slide(void) {
    return _dyld_get_image_vmaddr_slide(0);
}
#else
static intptr_t kai_rc_aslr_slide(void) { return 0; }
#endif

static void kai_rc_site_report(void) {
    if (getenv("KAI_TRACE_RC_QUIET")) return;
    if (kai_rc_sites_count == 0) return;

    /* Default top 20; KAI_TRACE_RC_TOP=N overrides. */
    int top_n = 20;
    const char *e = getenv("KAI_TRACE_RC_TOP");
    if (e && e[0]) {
        int n = atoi(e);
        if (n > 0) top_n = n;
    }

    /* Compact non-empty buckets into a contiguous array for sort. */
    KaiRcSite *flat = (KaiRcSite *) malloc(
        (size_t) kai_rc_sites_count * sizeof(KaiRcSite));
    if (!flat) return;
    int n = 0;
    int64_t total_leak = 0;
    for (int i = 0; i < KAI_RC_SITE_BUCKETS; i++) {
        if (kai_rc_sites[i].site != NULL) {
            flat[n++] = kai_rc_sites[i];
            int64_t live = kai_rc_sites[i].allocs - kai_rc_sites[i].frees;
            if (live > 0) total_leak += live;
        }
    }
    qsort(flat, (size_t) n, sizeof(KaiRcSite), kai_rc_site_cmp_leak);

    fprintf(stderr,
        "[KAI_TRACE_RC] top sites by leak (showing %d of %d distinct sites; total_leak=%lld%s)\n",
        top_n < n ? top_n : n, n, (long long) total_leak,
        kai_rc_sites_full ? "; TABLE SATURATED" : "");
    fprintf(stderr,
        "[KAI_TRACE_RC] aslr_slide=%p (subtract from site addresses for static symbolization)\n",
        (void *) kai_rc_aslr_slide());
    int limit = top_n < n ? top_n : n;
    for (int i = 0; i < limit; i++) {
        KaiRcSite *s = &flat[i];
        int64_t live = s->allocs - s->frees;
        double pct = s->allocs > 0
            ? (100.0 * (double) live / (double) s->allocs) : 0.0;
        double share = total_leak > 0
            ? (100.0 * (double) live / (double) total_leak) : 0.0;
        fprintf(stderr,
            "[KAI_TRACE_RC] site %2d %p tag=%-7s allocs=%lld frees=%lld leak=%lld leak%%=%.1f share%%=%.1f\n",
            i + 1, s->site, kai_rc_tag_name(s->tag),
            (long long) s->allocs, (long long) s->frees, (long long) live,
            pct, share);
    }
    free(flat);
}

static int kai_rc_site_registered = 0;
static void kai_rc_site_register_once(void) {
    if (kai_rc_site_registered) return;
    kai_rc_site_registered = 1;
    atexit(kai_rc_site_report);
}

/* Wrappers that ultimately call kai_alloc must NOT be inlined when
 * tracing is enabled — otherwise __builtin_return_address(0) inside
 * the wrapper points at the wrapper's parent's parent, not the
 * real emit site. Vanilla builds drop the attribute (zero overhead). */
#define KAI_RC_NOINLINE __attribute__((noinline))

/* ---------- Lane DIAG (#293): per-leak-site attribution ----------
 *
 * Aim: map every leaked chunk back to the kaikai source function that
 * allocated it, so the fix lane (Lane FIX) can rank scope_fns by leak
 * volume and patch the missing decrefs in their emit shapes.
 *
 * Two pieces of state at alloc time:
 *  - `kai_current_scope_fn`: the kaikai fn currently executing,
 *    set by the emitter via `kai_set_scope_fn(<name>)` at the top
 *    of every generated body. Static (the kaic2 self-compile is
 *    single-threaded). Inherits from the caller for fns that don't
 *    yet carry the hook (dispatch from prelude / FFI), so the
 *    attribution remains stable across mixed call paths.
 *  - allocator tag (KAI_VARIANT, KAI_RECORD, …): captured from
 *    `kai_alloc_traced`'s `tag` parameter.
 *
 * Aggregation is keyed on `(scope_fn, tag)`. Per-chunk metadata stays
 * to one extra pointer (`scope_fn` field on KaiValue under
 * KAI_TRACE_RC_LEAKSITE); the agg table is a fixed open-addressed
 * hash sized at 8 K buckets — empirically the kaic2 self-compile
 * has ≤ ~600 unique kaikai fn names, so worst-case load factor is
 * ~8 % across (fn × 13 tags). */
#ifdef KAI_TRACE_RC_LEAKSITE

static const char *kai_current_scope_fn = "<root>";

static void kai_set_scope_fn(const char *name) {
    kai_current_scope_fn = (name != NULL) ? name : "<root>";
}

#define KAI_LEAKSITE_BUCKETS 8192

typedef struct {
    const char *scope_fn;   /* NULL = empty bucket */
    int32_t     tag;
    int64_t     allocs;
    int64_t     frees;
} KaiLeakSite;

static KaiLeakSite kai_leaksites[KAI_LEAKSITE_BUCKETS];
static int         kai_leaksites_count = 0;
static int         kai_leaksites_full  = 0;

static uint32_t kai_leaksite_hash(const char *scope, int32_t tag) {
    uintptr_t x = (uintptr_t) scope;
    x ^= (uintptr_t) ((uint32_t) tag * 0x9E3779B1u);
    x ^= x >> 33;
    x *= (uintptr_t) 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (uint32_t) (x & (KAI_LEAKSITE_BUCKETS - 1));
}

static KaiLeakSite *kai_leaksite_lookup(const char *scope, int32_t tag, int insert) {
    if (scope == NULL) scope = "<unknown>";
    uint32_t mask = KAI_LEAKSITE_BUCKETS - 1;
    uint32_t h = kai_leaksite_hash(scope, tag);
    for (uint32_t i = 0; i < KAI_LEAKSITE_BUCKETS; i++) {
        uint32_t k = (h + i) & mask;
        KaiLeakSite *b = &kai_leaksites[k];
        if (b->scope_fn == scope && b->tag == tag) return b;
        if (b->scope_fn == NULL) {
            if (!insert) return NULL;
            b->scope_fn = scope;
            b->tag      = tag;
            kai_leaksites_count++;
            return b;
        }
    }
    kai_leaksites_full = 1;
    return NULL;
}

static void kai_leaksite_record_alloc(const char *scope, int32_t tag) {
    KaiLeakSite *b = kai_leaksite_lookup(scope, tag, 1);
    if (b) b->allocs++;
}

static void kai_leaksite_record_free(const char *scope, int32_t tag) {
    KaiLeakSite *b = kai_leaksite_lookup(scope, tag, 0);
    if (b) b->frees++;
}

static const char *kai_leaksite_alloc_fn(int32_t tag) {
    switch (tag) {
        case KAI_VARIANT: return "kai_variant";
        case KAI_RECORD:  return "kai_record";
        case KAI_CONS:    return "kai_cons";
        case KAI_STR:     return "kai_str";
        case KAI_INT:     return "kai_int";
        case KAI_REAL:    return "kai_real";
        case KAI_CHAR:    return "kai_char";
        case KAI_CLOSURE: return "kai_closure";
        case KAI_ARRAY:   return "kai_array";
        case KAI_BOOL:    return "kai_bool";
        case KAI_UNIT:    return "kai_unit";
        case KAI_NIL:     return "kai_nil";
        case KAI_FIBER:   return "kai_fiber";
        case KAI_PID:     return "kai_pid";
        case KAI_BYTE:      return "kai_byte";
        default:          return "kai_alloc";
    }
}

static int kai_leaksite_cmp_leak(const void *a, const void *b) {
    const KaiLeakSite *x = (const KaiLeakSite *) a;
    const KaiLeakSite *y = (const KaiLeakSite *) b;
    int64_t lx = x->allocs - x->frees;
    int64_t ly = y->allocs - y->frees;
    if (ly > lx) return 1;
    if (ly < lx) return -1;
    return 0;
}

static void kai_leaksite_report(void) {
    if (getenv("KAI_TRACE_RC_QUIET")) return;
    if (kai_leaksites_count == 0) return;

    int top_n = 20;
    const char *e = getenv("KAI_TRACE_RC_LEAKSITE_TOP");
    if (e && e[0]) {
        int n = atoi(e);
        if (n > 0) top_n = n;
    }

    KaiLeakSite *flat = (KaiLeakSite *) malloc(
        (size_t) kai_leaksites_count * sizeof(KaiLeakSite));
    if (!flat) return;
    int n = 0;
    int64_t total_leak = 0;
    int64_t per_tag_leak[16] = {0};
    for (int i = 0; i < KAI_LEAKSITE_BUCKETS; i++) {
        if (kai_leaksites[i].scope_fn != NULL) {
            flat[n++] = kai_leaksites[i];
            int64_t live = kai_leaksites[i].allocs - kai_leaksites[i].frees;
            if (live > 0) {
                total_leak += live;
                int t = kai_leaksites[i].tag;
                if (t >= 0 && t < 16) per_tag_leak[t] += live;
            }
        }
    }
    qsort(flat, (size_t) n, sizeof(KaiLeakSite), kai_leaksite_cmp_leak);

    int limit = top_n < n ? top_n : n;
    fprintf(stderr,
        "[KAI_TRACE_RC_LEAKSITE] top sites by leak (showing %d of %d distinct (scope_fn,tag); total_leak=%lld%s)\n",
        limit, n, (long long) total_leak,
        kai_leaksites_full ? "; TABLE SATURATED" : "");
    fprintf(stderr,
        "[KAI_TRACE_RC_LEAKSITE] columns: rank | alloc_fn | scope_fn | allocs | frees | leak | leak%% | share%%\n");
    for (int i = 0; i < limit; i++) {
        KaiLeakSite *s = &flat[i];
        int64_t live = s->allocs - s->frees;
        double pct = s->allocs > 0
            ? (100.0 * (double) live / (double) s->allocs) : 0.0;
        double share = total_leak > 0
            ? (100.0 * (double) live / (double) total_leak) : 0.0;
        fprintf(stderr,
            "[KAI_TRACE_RC_LEAKSITE] %2d | %-11s | %-40s | %10lld | %10lld | %10lld | %5.1f | %5.1f\n",
            i + 1,
            kai_leaksite_alloc_fn(s->tag),
            s->scope_fn,
            (long long) s->allocs,
            (long long) s->frees,
            (long long) live,
            pct, share);
    }
    /* Per-tag totals so checkpoint 2 (sum-by-alloc_fn vs per-tag totals)
     * can be verified at a glance from the same dump. */
    fprintf(stderr,
        "[KAI_TRACE_RC_LEAKSITE] per-tag leak totals (sum across all scope_fns):\n");
    for (int t = 0; t < 16; t++) {
        if (per_tag_leak[t] != 0) {
            fprintf(stderr,
                "[KAI_TRACE_RC_LEAKSITE]   tag=%-7s leak=%lld\n",
                kai_rc_tag_name(t), (long long) per_tag_leak[t]);
        }
    }
    free(flat);
}

static int kai_leaksite_registered = 0;
static void kai_leaksite_register_once(void) {
    if (kai_leaksite_registered) return;
    kai_leaksite_registered = 1;
    atexit(kai_leaksite_report);
}

#endif /* KAI_TRACE_RC_LEAKSITE */

#endif /* KAI_TRACE_RC */

#ifndef KAI_TRACE_RC
#define KAI_RC_NOINLINE
#endif

/* ---------- KAI_PROFILE_RC — per-category wall breakdown (lane #426)
 *
 * Independent of KAI_TRACE_RC. Times the four hot RC functions with
 * `clock_gettime(CLOCK_MONOTONIC)` and prints a per-category summary
 * at exit. Used to attribute the 16× C wall on the RB-tree benchmark
 * (docs/benchmarks/rb_tree_2026-05-09.md) to alloc / free / non-free
 * RC traffic, so the team can scope Phase 4 (variant-field unboxing)
 * vs drop-specialisation correctly.
 *
 * Build with `-DKAI_PROFILE_RC=1` to compile the wrappers in. Output
 * gates on env var KAI_PROFILE_RC=1 at run time so a single binary
 * can be timed with and without the report. Vanilla builds compile
 * to empty hooks and add zero overhead.
 *
 * Categories:
 *   - alloc    — time inside `kai_alloc` (the leaf calloc + bookkeep).
 *   - free     — time inside `kai_free_value` (the actual free + the
 *                cascading decrefs on contained children).
 *   - decref   — time inside `kai_decref` for every call that reaches
 *                the rc-- path (skips NULL and singleton early exits).
 *                Includes time spent calling `kai_free_value`; subtract
 *                the free total to get pure non-free RC traffic.
 *   - incref   — time inside `kai_incref` for every call that reaches
 *                the rc++ path (skips NULL and singleton early exits).
 *
 * clock_gettime(CLOCK_MONOTONIC) costs ~30 ns per call on macOS — at
 * ~25 M decrefs the instrumentation inflates the wall by ~1.5 s on
 * the RB-tree benchmark. Per-category PROPORTIONS remain robust;
 * absolute milliseconds under -DKAI_PROFILE_RC are not directly
 * comparable to the un-instrumented wall. The exit report prints the
 * instrumented wall alongside the categories so the gap is visible.
 *
 * Match dispatch is NOT a separate category — pattern test and
 * field-extract are emitted inline by the codegen (see
 * `_scr->as.var.variant_tag` reads in compiler-emitted C), so there
 * is no central function to wrap. It folds into the un-attributed
 * "other" bucket alongside everything else (calls, arithmetic,
 * stack management).
 */
#ifdef KAI_PROFILE_RC
#include <time.h>

static int64_t kai_prof_alloc_ns   = 0;
static int64_t kai_prof_free_ns    = 0;
static int64_t kai_prof_incref_ns  = 0;
static int64_t kai_prof_decref_ns  = 0;
static int64_t kai_prof_alloc_n    = 0;
static int64_t kai_prof_free_n     = 0;
static int64_t kai_prof_incref_n   = 0;
static int64_t kai_prof_decref_n   = 0;
static int64_t kai_prof_decref_to_zero_n = 0;
static struct timespec kai_prof_t0;
static int kai_prof_init_done = 0;
static int kai_prof_enabled = 0;

/* Exclusive-time stack. The four hot RC functions call each other
 * (kai_decref → kai_free_value → kai_decref → ...). Naive timing
 * double-counts nested time. To get exclusive (self-only) time per
 * category, every active call records its start_ns and accumulates
 * the gross duration of any child instrumented call into a `child_ns`
 * slot; on exit, exclusive = (now - start) - child_ns is added to the
 * category counter, and gross time is bubbled to the parent's slot.
 *
 * Stack depth 32 is overkill — the deepest realistic chain is decref
 * → free_value → decref → free_value → ... bounded by tree depth (~30
 * for 1M nodes). Static fixed array; saturates on overflow (very
 * defensively — if it ever fires we silently mis-attribute, no crash). */
#define KAI_PROF_STACK_CAP 64
typedef struct {
    int64_t start_ns;
    int64_t child_ns;
} KaiProfFrame;
static KaiProfFrame kai_prof_stack[KAI_PROF_STACK_CAP];
static int kai_prof_sp = 0;

static inline int64_t kai_prof_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000000000LL + (int64_t) ts.tv_nsec;
}

static inline int kai_prof_push(void) {
    if (kai_prof_sp >= KAI_PROF_STACK_CAP) return -1;  /* saturate */
    int idx = kai_prof_sp++;
    kai_prof_stack[idx].start_ns = kai_prof_now_ns();
    kai_prof_stack[idx].child_ns = 0;
    return idx;
}

static inline int64_t kai_prof_pop_exclusive(int idx) {
    if (idx < 0) return 0;
    int64_t end = kai_prof_now_ns();
    int64_t gross = end - kai_prof_stack[idx].start_ns;
    int64_t self  = gross - kai_prof_stack[idx].child_ns;
    if (self < 0) self = 0;  /* clock noise can produce tiny negatives */
    kai_prof_sp = idx;  /* unwind any deeper saturated frames defensively */
    /* Bubble gross time to parent so its exclusive subtraction works. */
    if (idx > 0) kai_prof_stack[idx - 1].child_ns += gross;
    return self;
}

static void kai_prof_report(void) {
    if (!getenv("KAI_PROFILE_RC")) return;
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    int64_t wall_ns =
        ((int64_t) t1.tv_sec - (int64_t) kai_prof_t0.tv_sec) * 1000000000LL
      + ((int64_t) t1.tv_nsec - (int64_t) kai_prof_t0.tv_nsec);
    int64_t alloc_ns  = kai_prof_alloc_ns;
    int64_t free_ns   = kai_prof_free_ns;
    int64_t incref_ns = kai_prof_incref_ns;
    int64_t decref_ns = kai_prof_decref_ns;
    /* Categories are exclusive (self-only) — no overlap. */
    int64_t rc_traffic_ns = incref_ns + decref_ns;
    int64_t accounted_ns  = alloc_ns + free_ns + rc_traffic_ns;
    int64_t other_ns      = wall_ns - accounted_ns;
    fprintf(stderr,
        "[KAI_PROFILE_RC] wall_ms=%lld alloc_ms=%lld free_ms=%lld "
        "rc_traffic_ms=%lld other_ms=%lld\n",
        (long long) (wall_ns / 1000000),
        (long long) (alloc_ns / 1000000),
        (long long) (free_ns  / 1000000),
        (long long) (rc_traffic_ns / 1000000),
        (long long) (other_ns / 1000000));
    fprintf(stderr,
        "[KAI_PROFILE_RC] decref_ms=%lld (self) incref_ms=%lld (self)\n",
        (long long) (decref_ns / 1000000),
        (long long) (incref_ns / 1000000));
    fprintf(stderr,
        "[KAI_PROFILE_RC] calls alloc=%lld free=%lld incref=%lld "
        "decref=%lld decref_to_zero=%lld\n",
        (long long) kai_prof_alloc_n,
        (long long) kai_prof_free_n,
        (long long) kai_prof_incref_n,
        (long long) kai_prof_decref_n,
        (long long) kai_prof_decref_to_zero_n);
    if (wall_ns > 0) {
        fprintf(stderr,
            "[KAI_PROFILE_RC] share alloc=%.1f%% free=%.1f%% "
            "rc_traffic=%.1f%% other=%.1f%%\n",
            100.0 * alloc_ns / wall_ns,
            100.0 * free_ns / wall_ns,
            100.0 * rc_traffic_ns / wall_ns,
            100.0 * other_ns / wall_ns);
    }
}

static void kai_prof_init(void) {
    if (kai_prof_init_done) return;
    kai_prof_init_done = 1;
    const char *e = getenv("KAI_PROFILE_RC");
    kai_prof_enabled = (e && e[0] && e[0] != '0') ? 1 : 0;
    if (!kai_prof_enabled) return;
    clock_gettime(CLOCK_MONOTONIC, &kai_prof_t0);
    atexit(kai_prof_report);
}

#define KAI_PROF_ENTER()           \
    int _kai_prof_idx = -1;        \
    do {                           \
        if (!kai_prof_init_done) kai_prof_init(); \
        if (kai_prof_enabled) _kai_prof_idx = kai_prof_push(); \
    } while (0)

#define KAI_PROF_EXIT(category)                                              \
    do {                                                                     \
        if (kai_prof_enabled) {                                              \
            kai_prof_##category##_ns += kai_prof_pop_exclusive(_kai_prof_idx); \
            kai_prof_##category##_n++;                                       \
        }                                                                    \
    } while (0)

#else /* !KAI_PROFILE_RC */

#define KAI_PROF_ENTER()        ((void) 0)
#define KAI_PROF_EXIT(category) ((void) 0)

#endif /* KAI_PROFILE_RC */

/* ---------- variant_name histogram (issue #300, lane #297/#298 validation)
 *
 * Independent of KAI_TRACE_RC. Counts variant alloc/free traffic keyed on
 * the `variant_name` string-literal pointer (stable across runs, immune to
 * the tail-call ABI bug that contaminates address-keyed attribution).
 *
 * Opt-in: build with `-DKAI_TRACE_VAR_NAMES=1`. Vanilla builds compile to
 * empty hooks; emitted IR (and selfhost byte-identity) is unchanged.
 *
 * Output: at exit a destructor dumps `[VAR_NAME] <name> allocs=A frees=F
 * leak=A-F` lines sorted by leak desc to stderr. Suppressed by
 * KAI_TRACE_RC_QUIET=1 (shared with the address-keyed tracer).
 */
#ifdef KAI_TRACE_VAR_NAMES

#define KAI_VAR_NAME_BUCKETS 4096

typedef struct {
    const char *name;
    int64_t allocs;       /* invocation count: every kai_variant(_, name, ...) call */
    int64_t real_allocs;  /* physical allocs: invocations that miss singleton/immortal cache */
    int64_t frees;
} KaiVarNameBucket;

static KaiVarNameBucket kai_var_names[KAI_VAR_NAME_BUCKETS];
static int kai_var_names_count;

static KaiVarNameBucket *kai_var_name_lookup(const char *name) {
    if (name == NULL) return NULL;
    uintptr_t k = (uintptr_t) name;
    uint32_t h = (uint32_t) ((k ^ (k >> 16)) * 0x9e3779b1u);
    for (int probe = 0; probe < KAI_VAR_NAME_BUCKETS; ++probe) {
        int i = (int) ((h + probe) & (KAI_VAR_NAME_BUCKETS - 1));
        if (kai_var_names[i].name == NULL) {
            kai_var_names[i].name = name;
            kai_var_names_count++;
            return &kai_var_names[i];
        }
        if (kai_var_names[i].name == name) return &kai_var_names[i];
    }
    return NULL; /* table full — silently drop */
}

static void kai_var_name_record_alloc(const char *name) {
    KaiVarNameBucket *b = kai_var_name_lookup(name);
    if (b) b->allocs++;
}

static void kai_var_name_record_real_alloc(const char *name) {
    KaiVarNameBucket *b = kai_var_name_lookup(name);
    if (b) b->real_allocs++;
}

static void kai_var_name_record_free(const char *name) {
    KaiVarNameBucket *b = kai_var_name_lookup(name);
    if (b) b->frees++;
}

static int kai_var_name_cmp_leak_desc(const void *a, const void *b) {
    const KaiVarNameBucket *x = (const KaiVarNameBucket *) a;
    const KaiVarNameBucket *y = (const KaiVarNameBucket *) b;
    int64_t lx = x->real_allocs - x->frees;
    int64_t ly = y->real_allocs - y->frees;
    if (ly != lx) return (ly > lx) - (ly < lx);
    return (y->real_allocs > x->real_allocs) - (y->real_allocs < x->real_allocs);
}

__attribute__((destructor))
static void kai_var_name_report_at_exit(void) {
    if (getenv("KAI_TRACE_RC_QUIET")) return;
    KaiVarNameBucket snapshot[KAI_VAR_NAME_BUCKETS];
    int n = 0;
    for (int i = 0; i < KAI_VAR_NAME_BUCKETS; ++i) {
        if (kai_var_names[i].name != NULL) snapshot[n++] = kai_var_names[i];
    }
    if (n == 0) return;
    qsort(snapshot, (size_t) n, sizeof(snapshot[0]), kai_var_name_cmp_leak_desc);
    int64_t total_invs = 0, total_real = 0, total_frees = 0;
    for (int i = 0; i < n; ++i) {
        total_invs += snapshot[i].allocs;
        total_real += snapshot[i].real_allocs;
        total_frees += snapshot[i].frees;
    }
    fprintf(stderr,
        "[VAR_NAME] total distinct=%d invocations=%lld real_allocs=%lld frees=%lld leak=%lld\n",
        n, (long long) total_invs, (long long) total_real,
        (long long) total_frees, (long long) (total_real - total_frees));
    fprintf(stderr,
        "[VAR_NAME] columns: name | invocations | real_allocs | frees | leak\n");
    for (int i = 0; i < n; ++i) {
        int64_t leak = snapshot[i].real_allocs - snapshot[i].frees;
        fprintf(stderr,
            "[VAR_NAME] %-24s inv=%-12lld real=%-10lld frees=%-10lld leak=%lld\n",
            snapshot[i].name,
            (long long) snapshot[i].allocs,
            (long long) snapshot[i].real_allocs,
            (long long) snapshot[i].frees,
            (long long) leak);
    }
}

#define KAI_VAR_NAME_ALLOC(name)      kai_var_name_record_alloc(name)
#define KAI_VAR_NAME_REAL_ALLOC(name) kai_var_name_record_real_alloc(name)
#define KAI_VAR_NAME_FREE(name)       kai_var_name_record_free(name)

#else /* !KAI_TRACE_VAR_NAMES */

#define KAI_VAR_NAME_ALLOC(name)      ((void) 0)
#define KAI_VAR_NAME_REAL_ALLOC(name) ((void) 0)
#define KAI_VAR_NAME_FREE(name)       ((void) 0)

#endif /* KAI_TRACE_VAR_NAMES */

#ifdef KAI_TRACE_RC
static KaiValue *kai_alloc_traced(KaiTag tag, void *site) {
#else
static KaiValue *kai_alloc(KaiTag tag) {
#endif
    KAI_PROF_ENTER();
    KaiValue *v = (KaiValue *) calloc(1, sizeof(KaiValue));
    if (!v) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    v->rc = 1;
    v->tag = (int32_t) tag;
    /* trace */
    kai_rc_alloc_total++;
    kai_rc_live_now++;
    if (kai_rc_live_now > kai_rc_live_peak) kai_rc_live_peak = kai_rc_live_now;
    if ((int) tag >= 0 && (int) tag < 16) kai_rc_alloc_by_tag[(int) tag]++;
#ifdef KAI_TRACE_RC
    v->alloc_site = site;
    kai_rc_site_record_alloc(site, (int32_t) tag);
    kai_rc_site_register_once();
    kai_rc_history_log(v, /* op=alloc */ 0, (int32_t) tag);
#endif
#ifdef KAI_TRACE_RC_LEAKSITE
    v->scope_fn = kai_current_scope_fn;
    kai_leaksite_record_alloc(kai_current_scope_fn, (int32_t) tag);
    kai_leaksite_register_once();
#endif
    KAI_PROF_EXIT(alloc);
    return v;
}

#ifdef KAI_TRACE_RC
/* Macro shim: every call to kai_alloc(tag) inside a wrapper captures
 * the wrapper's caller (the real emit site) via
 * __builtin_return_address(0). Wrappers must be marked
 * KAI_RC_NOINLINE for this to point at the right frame. */
#define kai_alloc(tag) kai_alloc_traced((tag), __builtin_return_address(0))
#endif

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
static KaiValue kai_singleton_unit  = { .rc = INT32_MAX, .tag = KAI_UNIT, .as = { .b = 0 } };
static KaiValue kai_singleton_true  = { .rc = INT32_MAX, .tag = KAI_BOOL, .as = { .b = 1 } };
static KaiValue kai_singleton_false = { .rc = INT32_MAX, .tag = KAI_BOOL, .as = { .b = 0 } };
static KaiValue kai_singleton_nil   = { .rc = INT32_MAX, .tag = KAI_NIL,  .as = { .b = 0 } };

static KaiValue *kai_incref(KaiValue *v) {
    if (!v || v->rc == INT32_MAX) {
#ifdef KAI_TRACE_RC
        if (v) kai_rc_history_log(v, /* op=incref */ 1, v->tag);
#endif
        return v;
    }
    KAI_PROF_ENTER();
    v->rc++;
#ifdef KAI_TRACE_RC
    kai_rc_history_log(v, /* op=incref */ 1, v->tag);
#endif
    KAI_PROF_EXIT(incref);
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
    NULL,                /* cloned_evidence_chain — main never inherits */
    NULL, 0, 0, 0, NULL  /* reactor_next, _deadline_ns, _wait_pid, _wait_status, _data */
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
static KAI_RC_NOINLINE KaiValue *kai_fiber_value(KaiFiber *f) {
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

/* Issue #611 — Phase R1 reactor forward decls. The default Clock /
 * File / Process handlers live above the reactor implementation
 * but call into it through the small API below to park their
 * fibers. Bodies sit alongside the scheduler primitives a few
 * thousand lines further down. */
typedef struct KaiFilepoolItem KaiFilepoolItem;
static void     kai_reactor_init(void);
static void     kai_reactor_park_timer(KaiFiber *f, uint64_t deadline_ns);
static void     kai_reactor_park_pid(KaiFiber *f, int pid);
static KaiValue *kai_reactor_run_in_pool(KaiValue *(*work)(void *), void *arg);
static uint64_t kai_reactor_now_ns(void);

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
static KAI_RC_NOINLINE KaiValue *kai_pid_value(KaiMailbox *mb) {
    KaiValue *v = kai_alloc(KAI_PID);
    v->as.mb = mb;
    return v;
}

static void kai_free_value(KaiValue *v) {
    KAI_PROF_ENTER();
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
            KAI_VAR_NAME_FREE(v->as.var.variant_name);
            /* Issue #440 — only pointer slots cascade through decref.
             * Phase 2: primitive slots (mask kind != PTR) carry raw
             * scalars and need no RC. The mask==0 hot path skips the
             * kind branch entirely. */
            if (v->as.var.slot_mask == 0) {
                for (int i = 0; i < v->as.var.n_args; ++i) kai_decref(v->as.var.slots[i].ptr);
            } else {
                for (int i = 0; i < v->as.var.n_args; ++i) {
                    if (kai_var_slot_kind(v->as.var.slot_mask, i) == KAI_VAR_SLOT_PTR) {
                        kai_decref(v->as.var.slots[i].ptr);
                    }
                }
            }
            free(v->as.var.slots);
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
        case KAI_BYTE:
            /* Lane 4 (#473): Byte is a 1-byte scalar embedded directly
             * in the KaiValue. No heap payload. The base free(v)
             * below reclaims the whole value. */
            break;
        default: break;
    }
#ifdef KAI_TRACE_RC
    {
        int32_t freed_tag = v->tag;
        if (freed_tag >= 0 && freed_tag < 16) kai_rc_free_by_tag[freed_tag]++;
        /* issue #296 — credit the matching alloc site. */
        kai_rc_site_record_free(v->alloc_site);
        kai_rc_history_log(v, /* op=free */ 3, freed_tag);
#ifdef KAI_TRACE_RC_LEAKSITE
        /* Lane DIAG (#293) — credit the matching scope_fn. Read
         * before the poison stamp below so we don't read back
         * 0xDEADBEEF as the scope pointer. */
        kai_leaksite_record_free(v->scope_fn, freed_tag);
#endif
        /* Stamp poison over the whole struct so a stale read after
         * free surfaces the recognizable 0xDEADBEEF... pattern on
         * macOS instead of stale-but-plausible content. We touch
         * sizeof(KaiValue) bytes; safe because we still own the
         * chunk until the free(v) below. */
        uint64_t *p64 = (uint64_t *) v;
        size_t   nq   = sizeof(KaiValue) / sizeof(uint64_t);
        for (size_t i = 0; i < nq; i++) p64[i] = KAI_RC_SENTINEL_U64;
    }
#endif
    free(v);
    /* trace */
    kai_rc_free_total++;
    kai_rc_live_now--;
    KAI_PROF_EXIT(free);
}

static void kai_decref(KaiValue *v) {
    if (!v) return;
    if (v->rc == INT32_MAX) return;   /* m5 #7 — singleton, saturated */
    KAI_PROF_ENTER();
#ifdef KAI_TRACE_RC
    kai_rc_history_log(v, /* op=decref */ 2, v->tag);
#endif
    if (--v->rc == 0) {
#ifdef KAI_PROFILE_RC
        kai_prof_decref_to_zero_n++;
#endif
        kai_free_value(v);
    }
    KAI_PROF_EXIT(decref);
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

static KAI_RC_NOINLINE KaiValue *kai_int(int64_t i) {
    if (i >= KAI_INT_CACHE_LO && i <= KAI_INT_CACHE_HI) {
        if (!kai_int_cache_init) kai_int_cache_warm();
        return &kai_int_cache[i - KAI_INT_CACHE_LO];
    }
    KaiValue *v = kai_alloc(KAI_INT);
    v->as.i = i;
    return v;
}

static KAI_RC_NOINLINE KaiValue *kai_real(double r) {
    KaiValue *v = kai_alloc(KAI_REAL);
    v->as.r = r;
    return v;
}

/* Lane 4 (#473): Byte nominal scalar. No interning cache for v1 — every
 * `kai_byte(n)` allocates a fresh KaiValue. Cache (analogous to
 * kai_int_cache for 0..127) is a natural Lane-4b/perf optimisation. */
static KAI_RC_NOINLINE KaiValue *kai_byte(uint8_t n) {
    KaiValue *v = kai_alloc(KAI_BYTE);
    v->as.byte_val = n;
    return v;
}

static KAI_RC_NOINLINE KaiValue *kai_char(uint32_t c) {
    if (c <= KAI_CHAR_CACHE_HI) {
        if (!kai_char_cache_init) kai_char_cache_warm();
        return &kai_char_cache[c];
    }
    KaiValue *v = kai_alloc(KAI_CHAR);
    v->as.c = c;
    return v;
}

static KAI_RC_NOINLINE KaiValue *kai_str_from_bytes(const char *bytes, size_t len) {
    KaiValue *v = kai_alloc(KAI_STR);
    v->as.s.len = len;
    v->as.s.bytes = (char *) malloc(len + 1);
    if (!v->as.s.bytes) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    if (len > 0) memcpy(v->as.s.bytes, bytes, len);
    v->as.s.bytes[len] = '\0';
    return v;
}

/* Lane FIX (top-3 leak sites): short-string interning.
 *
 * `prelude_table()` (DIAG rank #1) reallocates ~50 strings per call —
 * every call to `prelude_find` rebuilds the same `.rodata`-pointed
 * literals. The strings are content-deduped by the C compiler, so
 * repeat `kai_str("print")` calls receive the *same* `cstr` pointer.
 * Interning by pointer-then-content gives an immortal singleton per
 * literal, eliminating the per-call alloc/leak the same way
 * #300 / #304 did for nullary and immortal-payload variants.
 *
 * Two-stage lookup (open-addressed, 1024 buckets):
 *   1. Pointer match — hits 100% of compiler-emitted literal calls
 *      because identical literals share a single .rodata entry.
 *   2. Content match (≤ 64 bytes) — backstop for cstr's that arrive
 *      via stack buffers (`int_to_string`, `kai_cat2(...)`) and
 *      happen to repeat content.
 *
 * Misses (long strings, full table, transient content unique per
 * call) fall through to `kai_str_from_bytes` unchanged.
 *
 * Cached values carry `rc = INT32_MAX` so `kai_incref` / `kai_decref`
 * short-circuit; `kai_free_value` is never reached. */
#define KAI_STR_INTERN_BUCKETS 1024
#define KAI_STR_INTERN_MAXLEN  64
typedef struct {
    const char *cstr;       /* NULL ⇒ empty bucket. Owned via strdup. */
    size_t      len;
    KaiValue   *value;
} KaiStrInternBucket;
static KaiStrInternBucket kai_str_intern_table[KAI_STR_INTERN_BUCKETS];

static inline size_t kai_str_intern_hash(const char *cstr, size_t len) {
    /* FNV-1a, len-bounded. */
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t) (uint8_t) cstr[i];
        h *= 1099511628211ull;
    }
    return (size_t) (h & (KAI_STR_INTERN_BUCKETS - 1));
}

static KAI_RC_NOINLINE KaiValue *kai_str(const char *cstr) {
    size_t len = strlen(cstr);
    if (len > KAI_STR_INTERN_MAXLEN) return kai_str_from_bytes(cstr, len);
    size_t i = kai_str_intern_hash(cstr, len);
    for (size_t probe = 0; probe < KAI_STR_INTERN_BUCKETS; probe++) {
        KaiStrInternBucket *b = &kai_str_intern_table[i];
        if (b->cstr == NULL) {
            /* Insert. Allocate the immortal value + own a copy of cstr
             * (cstr arg may be a stack buffer that gets reused). */
            KaiValue *v = kai_str_from_bytes(cstr, len);
            v->rc = INT32_MAX;
            char *owned = (char *) malloc(len + 1);
            if (owned) {
                memcpy(owned, cstr, len);
                owned[len] = '\0';
                b->cstr = owned;
                b->len = len;
                b->value = v;
            }
            /* Even if strdup failed, the value is still valid (just
             * not cacheable). Subsequent calls re-alloc but stay
             * correct. */
            return v;
        }
        if (b->len == len && (b->cstr == cstr || memcmp(b->cstr, cstr, len) == 0)) {
            return b->value;
        }
        i = (i + 1) & (KAI_STR_INTERN_BUCKETS - 1);
    }
    /* Table full — fall back to non-cached. */
    return kai_str_from_bytes(cstr, len);
}

static KaiValue *kai_nil(void) { return &kai_singleton_nil; }

static KAI_RC_NOINLINE KaiValue *kai_cons(KaiValue *head, KaiValue *tail) {
    KaiValue *v = kai_alloc(KAI_CONS);
    v->as.cons.head = head;
    v->as.cons.tail = tail;
    return v;
}

static KAI_RC_NOINLINE KaiValue *kai_record(int n, KaiValue **fields, const char **names) {
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

/* Issue #300 — nullary variant singletons.
 *
 * `None` alone accounts for 50.7M allocations / 50.0M leaked in
 * the kaic2 self-compile (63.8% of total live count). Every
 * `kai_variant(_, "None", 0, NULL)` call freshly allocs a chunk
 * structurally identical to every other `None` chunk: same tag,
 * same name pointer (string literal in .rodata), no payload. They
 * differ only in identity, which kaikai source code never observes
 * (variants don't have referential equality).
 *
 * The fix is the same trick already used for unit/bool/nil/cached
 * ints/chars: keep one immortal chunk per `(tag, name_ptr)` pair,
 * mark it `rc = INT32_MAX`, and return that pointer on every call.
 * `kai_incref` / `kai_decref` already short-circuit on INT32_MAX
 * so the singleton survives every RC operation unchanged and
 * `kai_free_value` is never reached with a singleton.
 *
 * Storage — open-addressed table keyed on (tag, name_ptr). The
 * name pointer is the address of a string literal in the binary's
 * .rodata segment, so it's stable across calls and unique per
 * distinct constructor. kaikai has on the order of 20-30 distinct
 * nullary variants; 64 buckets gives ample headroom.
 */
#define KAI_NULLARY_SINGLETON_BUCKETS 64
typedef struct {
    int32_t       tag;
    const char   *name;     /* NULL ⇒ empty bucket */
    KaiValue     *value;
} KaiNullarySingletonBucket;
static KaiNullarySingletonBucket
    kai_nullary_singletons[KAI_NULLARY_SINGLETON_BUCKETS];

static inline size_t kai_nullary_hash(int32_t tag, const char *name) {
    uintptr_t p = (uintptr_t) name;
    return (size_t) (((p >> 4) ^ (p >> 12) ^ (uintptr_t) tag)
                     & (KAI_NULLARY_SINGLETON_BUCKETS - 1));
}

static KaiValue *kai_nullary_lookup(int32_t tag, const char *name) {
    size_t i = kai_nullary_hash(tag, name);
    for (size_t probe = 0; probe < KAI_NULLARY_SINGLETON_BUCKETS; probe++) {
        KaiNullarySingletonBucket *b = &kai_nullary_singletons[i];
        if (b->name == NULL) return NULL;
        if (b->tag == tag && b->name == name) return b->value;
        i = (i + 1) & (KAI_NULLARY_SINGLETON_BUCKETS - 1);
    }
    return NULL;
}

static void kai_nullary_install(int32_t tag, const char *name, KaiValue *v) {
    size_t i = kai_nullary_hash(tag, name);
    for (size_t probe = 0; probe < KAI_NULLARY_SINGLETON_BUCKETS; probe++) {
        KaiNullarySingletonBucket *b = &kai_nullary_singletons[i];
        if (b->name == NULL) {
            b->tag = tag;
            b->name = name;
            b->value = v;
            return;
        }
        i = (i + 1) & (KAI_NULLARY_SINGLETON_BUCKETS - 1);
    }
    /* Table full — fall back to non-singleton behaviour. With 64
     * buckets and a known cap of ~30 distinct nullary variants
     * this branch is unreachable in practice. */
}

/* Issue #293 next-tier — variants whose every arg is itself an
 * immortal singleton (rc==INT32_MAX) are themselves morally
 * immortal: they can never be mutated and have no observable
 * identity. Cache them just like nullary variants. The dominant
 * win is `Some(<immortal>)` and `Ok(<immortal>)`, which the
 * compiler builds by the millions when threading typer/parser
 * results that are themselves nullary variants or cached scalars.
 *
 * Storage — open-addressed table keyed on (tag, name, n, args[0..n]).
 * Capped at n <= 4 to bound the key size. Once the table fills,
 * subsequent lookups walk every bucket without finding the empty
 * sentinel, so installs silently fail and every later invocation
 * falls back to a fresh alloc. The 16384-bucket sizing originally
 * landed for #293 turned out to be ~16x too small for kaic2's
 * self-compile: the typer's `prelude_table` rebuilds 47 EP variants
 * per call × ~13K calls (issue #297 EP wave, 2026-05-07), and
 * Some/Ok/TyCon-shaped immortal-payload combinations push the
 * working set past 16K well before the prelude entries land. The
 * larger 262144-bucket sizing absorbs the EP table outright (642K
 * leaks → 45 cached chunks, 99.99% drop) and adds ~16 MB of static
 * .bss to the binary. Collisions still degrade to a fresh non-cached
 * alloc — same behaviour as a full nullary table.
 */
#define KAI_IMMORTAL_VAR_BUCKETS 262144
#define KAI_IMMORTAL_VAR_MAXN 4
typedef struct {
    int32_t       tag;
    int           n;
    const char   *name;     /* NULL ⇒ empty bucket */
    KaiValue     *args[KAI_IMMORTAL_VAR_MAXN];
    KaiValue     *value;
} KaiImmortalVarBucket;
static KaiImmortalVarBucket
    kai_immortal_vars[KAI_IMMORTAL_VAR_BUCKETS];

static inline size_t kai_immortal_var_hash(int32_t tag, const char *name,
                                           int n, KaiValue **args) {
    uintptr_t h = (uintptr_t) name;
    h = (h * 1315423911u) ^ ((uintptr_t) tag);
    h = (h * 1315423911u) ^ (uintptr_t) n;
    for (int i = 0; i < n; i++) {
        h = (h * 1315423911u) ^ (uintptr_t) args[i];
    }
    h ^= h >> 13;
    return (size_t) (h & (KAI_IMMORTAL_VAR_BUCKETS - 1));
}

static int kai_immortal_var_match(KaiImmortalVarBucket *b, int32_t tag,
                                  const char *name, int n, KaiValue **args) {
    if (b->name != name || b->tag != tag || b->n != n) return 0;
    for (int i = 0; i < n; i++) {
        if (b->args[i] != args[i]) return 0;
    }
    return 1;
}

static KaiValue *kai_immortal_var_lookup(int32_t tag, const char *name,
                                         int n, KaiValue **args) {
    size_t i = kai_immortal_var_hash(tag, name, n, args);
    for (size_t probe = 0; probe < KAI_IMMORTAL_VAR_BUCKETS; probe++) {
        KaiImmortalVarBucket *b = &kai_immortal_vars[i];
        if (b->name == NULL) return NULL;
        if (kai_immortal_var_match(b, tag, name, n, args)) return b->value;
        i = (i + 1) & (KAI_IMMORTAL_VAR_BUCKETS - 1);
    }
    return NULL;
}

static void kai_immortal_var_install(int32_t tag, const char *name, int n,
                                     KaiValue **args, KaiValue *v) {
    size_t i = kai_immortal_var_hash(tag, name, n, args);
    for (size_t probe = 0; probe < KAI_IMMORTAL_VAR_BUCKETS; probe++) {
        KaiImmortalVarBucket *b = &kai_immortal_vars[i];
        if (b->name == NULL) {
            b->tag = tag;
            b->name = name;
            b->n = n;
            for (int j = 0; j < n; j++) b->args[j] = args[j];
            b->value = v;
            return;
        }
        i = (i + 1) & (KAI_IMMORTAL_VAR_BUCKETS - 1);
    }
    /* Table full — fall back to non-cached behaviour. */
}

static int kai_args_all_immortal(int n, KaiValue **args) {
    if (n <= 0 || n > KAI_IMMORTAL_VAR_MAXN) return 0;
    for (int i = 0; i < n; i++) {
        if (args[i] == NULL || args[i]->rc != INT32_MAX) return 0;
    }
    return 1;
}

static KAI_RC_NOINLINE KaiValue *kai_variant(int32_t tag, const char *name, int n, KaiValue **args) {
    KAI_VAR_NAME_ALLOC(name);
    if (n == 0 && name != NULL) {
        KaiValue *cached = kai_nullary_lookup(tag, name);
        if (cached) return cached;
        KAI_VAR_NAME_REAL_ALLOC(name);
        KaiValue *v = kai_alloc(KAI_VARIANT);
        v->as.var.variant_tag = tag;
        v->as.var.variant_name = name;
        v->as.var.n_args = 0;
        v->as.var.slot_mask = 0;
        v->as.var.slots = NULL;
        v->rc = INT32_MAX;
        kai_nullary_install(tag, name, v);
        return v;
    }
    if (name != NULL && kai_args_all_immortal(n, args)) {
        KaiValue *cached = kai_immortal_var_lookup(tag, name, n, args);
        if (cached) return cached;
        KAI_VAR_NAME_REAL_ALLOC(name);
        KaiValue *v = kai_alloc(KAI_VARIANT);
        v->as.var.variant_tag = tag;
        v->as.var.variant_name = name;
        v->as.var.n_args = n;
        v->as.var.slot_mask = 0;
        v->as.var.slots = (KaiVarSlot *) malloc(n * sizeof(KaiVarSlot));
        for (int i = 0; i < n; ++i) v->as.var.slots[i].ptr = args[i];
        v->rc = INT32_MAX;
        kai_immortal_var_install(tag, name, n, args, v);
        return v;
    }
    KAI_VAR_NAME_REAL_ALLOC(name);
    KaiValue *v = kai_alloc(KAI_VARIANT);
    v->as.var.variant_tag = tag;
    v->as.var.variant_name = name;
    v->as.var.n_args = n;
    v->as.var.slot_mask = 0;
    if (n > 0) {
        v->as.var.slots = (KaiVarSlot *) malloc(n * sizeof(KaiVarSlot));
        for (int i = 0; i < n; ++i) v->as.var.slots[i].ptr = args[i];
    } else {
        v->as.var.slots = NULL;
    }
    return v;
}

/* Issue #440 Phase 2 — variant constructor with typed payload slots.
 * Called by stage 2 emitter when at least one payload slot is a
 * primitive (Int or Real). `slots` is borrowed by the constructor:
 * each slot value is copied verbatim into the new cell. For pointer
 * slots the caller transfers ownership exactly like the legacy
 * `kai_variant` path. Singleton caches (nullary / immortal) are NOT
 * consulted on this path; Phase 2 only takes the typed-construction
 * fast path when there is at least one unboxed primitive — those
 * cells have no boxed-equivalent in the existing cache shape.
 * Returns an owning ref. */
static KAI_RC_NOINLINE KaiValue *kai_variant_u(int32_t tag, const char *name,
                                               int n, uint32_t mask,
                                               KaiVarSlot *slots) {
    KAI_VAR_NAME_ALLOC(name);
    KAI_VAR_NAME_REAL_ALLOC(name);
    KaiValue *v = kai_alloc(KAI_VARIANT);
    v->as.var.variant_tag = tag;
    v->as.var.variant_name = name;
    v->as.var.n_args = n;
    v->as.var.slot_mask = mask;
    if (n > 0) {
        v->as.var.slots = (KaiVarSlot *) malloc(n * sizeof(KaiVarSlot));
        for (int i = 0; i < n; ++i) v->as.var.slots[i] = slots[i];
    } else {
        v->as.var.slots = NULL;
    }
    return v;
}

/* Issue #440 Phase 2 — borrow a slot as a boxed `KaiValue *`. Used by
 * match-arm extraction, by the generic walkers (eq, to_string) and by
 * any other code path that needs the boxed view. For pointer slots
 * this returns the boxed child directly (still owned by the cell).
 * For primitive slots this allocates a fresh boxed temporary that the
 * caller owns and must release. Hot path: pointer slots are the
 * majority by far in legacy code; the branch predictor sees one
 * direction. */
static inline KaiValue *kai_variant_slot_box(KaiValue *v, int i) {
    uint32_t k = kai_var_slot_kind(v->as.var.slot_mask, i);
    if (k == KAI_VAR_SLOT_PTR)  return v->as.var.slots[i].ptr;
    if (k == KAI_VAR_SLOT_INT)  return kai_int(v->as.var.slots[i].i64);
    if (k == KAI_VAR_SLOT_REAL) return kai_real(v->as.var.slots[i].r);
    return v->as.var.slots[i].ptr;
}

/* Issue #440 Phase 2 — consume a boxed Int / Real, return the raw
 * scalar and release the boxed temporary. Used by the stage 2
 * emitter when packing a primitive payload into a typed variant
 * slot: the call expression yields a boxed `KaiValue *` (already
 * possibly cached as a singleton), and we want the raw payload to
 * write into `slot.i64` / `slot.r`. Singleton Ints (rc == INT32_MAX)
 * survive the decref unchanged. */
static inline int64_t kai_take_int(KaiValue *v) {
    int64_t x = v->as.i;
    kai_decref(v);
    return x;
}
static inline double kai_take_real(KaiValue *v) {
    double x = v->as.r;
    kai_decref(v);
    return x;
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
        _scr->as.var.n_args == n && _scr->as.var.slot_mask == 0 &&
        kai_check_unique(_scr)) {
        /* Phase 2: reuse fires only on legacy mask==0 cells. A typed
         * cell (mask != 0) carries unboxed scalars whose slot kinds
         * would have to be re-derived from the rebuild context — the
         * recogniser does not yet ship that path. The Perceus
         * recogniser in stage 2 still only synthesises this call for
         * boxed-arg constructors, so the precondition matches the
         * emit side. */
        for (int i = 0; i < n; ++i) {
            kai_decref(_scr->as.var.slots[i].ptr);
            _scr->as.var.slots[i].ptr = args[i];
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
static KAI_RC_NOINLINE KaiValue *kai_array_make(int64_t len, KaiValue *init) {
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
static KAI_RC_NOINLINE KaiValue *kai_closure(KaiFn fn, int arity, int n_captures, KaiValue **captures) {
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

/* Invoke a closure dynamically.
 *
 * Issue #298 — kai_apply consumes its closure argument. Pre-fix the
 * helper borrowed `clo`; emitters at every callsite (stage0 emit.c,
 * stage1/stage2 compiler.kai, LLVM kaix_apply) handed it the binding
 * raw and assumed the surrounding scope owned the ref. For let-bound
 * closures invoked exactly once (e.g. `let f = make_adder(n); f(10)`),
 * Perceus picked the `LUAt + count == 1 -> raw transfer` branch and
 * never inserted a drop — so the closure leaked one ref per call. The
 * 27.97% closure leak in kaic2 self-compile was driven by this shape.
 *
 * Post-fix the contract is symmetric with the rest of m5.x: every
 * KaiValue * passed to kai_apply is OWNED by the call. The dispatched
 * fn body cannot read `self` after its own return (it cannot — that
 * pointer is freed before the result hits the caller), and runtime
 * helpers (kai_prelude_map / _filter / _flat_map / _reduce / _each,
 * kai_fiber_trampoline) now incref the closure ahead of every loop
 * iteration so each kai_apply gets its own ref to consume; their
 * post-loop decref releases the original ref the helper was handed. */
static KaiValue *kai_apply(KaiValue *clo, int argc, KaiValue **argv) {
    if (!clo || clo->tag != KAI_CLOSURE) {
        fprintf(stderr, "kai: attempted to call a non-callable value\n");
        exit(1);
    }
    KaiValue *r = clo->as.clo.fn(clo, argv, argc);
    kai_decref(clo);
    return r;
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
            /* Phase 2: if either cell carries primitive slots, the
             * masks must agree slot-by-slot (a variant of the same
             * tag/name must have the same layout). Then compare each
             * slot by its kind: pointer slots recurse via kai_op_eq,
             * primitive slots compare raw scalars. The mask==0 hot
             * path stays on the original pointer-by-pointer walk. */
            if (a->as.var.slot_mask == 0 && b->as.var.slot_mask == 0) {
                for (int i = 0; i < a->as.var.n_args; ++i) {
                    if (!kai_op_eq(a->as.var.slots[i].ptr, b->as.var.slots[i].ptr)) return 0;
                }
            } else {
                if (a->as.var.slot_mask != b->as.var.slot_mask) return 0;
                for (int i = 0; i < a->as.var.n_args; ++i) {
                    uint32_t k = kai_var_slot_kind(a->as.var.slot_mask, i);
                    if (k == KAI_VAR_SLOT_PTR) {
                        if (!kai_op_eq(a->as.var.slots[i].ptr, b->as.var.slots[i].ptr)) return 0;
                    } else if (k == KAI_VAR_SLOT_INT) {
                        if (a->as.var.slots[i].i64 != b->as.var.slots[i].i64) return 0;
                    } else if (k == KAI_VAR_SLOT_REAL) {
                        if (a->as.var.slots[i].r != b->as.var.slots[i].r) return 0;
                    }
                }
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
        case KAI_BYTE:      return a->as.byte_val == b->as.byte_val;    /* Lane 4 (#473) */
    }
    return 0;
}

/* ---------- to-string ---------- */

static KAI_RC_NOINLINE KaiValue *kai_string_concat(KaiValue *a, KaiValue *b);

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
                /* Phase 2: primitive slots are boxed on-the-fly via
                 * kai_variant_slot_box; for the mask==0 hot path it
                 * just returns the stored pointer. */
                for (int i = 0; i < v->as.var.n_args; ++i) {
                    if (i) { KaiValue *sep = kai_string_concat(acc, kai_str(", ")); kai_decref(acc); acc = sep; }
                    uint32_t k = kai_var_slot_kind(v->as.var.slot_mask, i);
                    KaiValue *s;
                    if (k == KAI_VAR_SLOT_PTR) {
                        s = kai_to_string(v->as.var.slots[i].ptr);
                    } else {
                        KaiValue *tmp = kai_variant_slot_box(v, i);
                        s = kai_to_string(tmp);
                        kai_decref(tmp);
                    }
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
        case KAI_BYTE:                                       /* Lane 4 (#473) */
            snprintf(buf, sizeof(buf), "%u", (unsigned) v->as.byte_val);
            return kai_str(buf);
    }
    return kai_str("?");
}

static KAI_RC_NOINLINE KaiValue *kai_string_concat(KaiValue *a, KaiValue *b) {
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
static KAI_RC_NOINLINE KaiValue *kai_string_concat_all_impl(KaiValue *xs) {
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
static KAI_RC_NOINLINE KaiValue *kai_string_join_impl(KaiValue *xs, KaiValue *sep) {
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

/* ---------- Lane 4 (#473): Byte nominal scalar conversions + ops ----- */

/* `int_to_byte(n)` returns `Result[String, Byte]` — Err if n is outside
 * 0..255, Ok otherwise. The Result is encoded as a KAI_VARIANT (Ok /
 * Err) with one payload. */
static KaiValue *kai_prelude_int_to_byte(KaiValue *v) {
    if (!v || v->tag != KAI_INT) {
        if (v) kai_decref(v);
        KaiValue *err = kai_str("int_to_byte: not an Int");
        KaiValue **args = (KaiValue **) malloc(sizeof(KaiValue *));
        args[0] = err;
        KaiValue *r = kai_variant(1, "Err", 1, args);
        free(args);
        return r;
    }
    int64_t n = v->as.i;
    kai_decref(v);
    if (n < 0 || n > 255) {
        char buf[64];
        snprintf(buf, sizeof(buf), "int_to_byte: %lld is out of 0..255", (long long) n);
        KaiValue *err = kai_str(buf);
        KaiValue **args = (KaiValue **) malloc(sizeof(KaiValue *));
        args[0] = err;
        KaiValue *r = kai_variant(1, "Err", 1, args);
        free(args);
        return r;
    }
    KaiValue *ok_payload = kai_byte((uint8_t) n);
    KaiValue **args = (KaiValue **) malloc(sizeof(KaiValue *));
    args[0] = ok_payload;
    KaiValue *r = kai_variant(0, "Ok", 1, args);
    free(args);
    return r;
}

static KaiValue *kai_prelude_byte_to_int(KaiValue *v) {
    if (!v || v->tag != KAI_BYTE) { if (v) kai_decref(v); return kai_int(0); }
    int64_t n = (int64_t) v->as.byte_val;
    kai_decref(v);
    return kai_int(n);
}

/* Wrapping arithmetic per uint8_t C semantics. Overflow is defined. */
static KaiValue *kai_prelude_byte_add(KaiValue *a, KaiValue *b) {
    uint8_t av = (a && a->tag == KAI_BYTE) ? a->as.byte_val : 0;
    uint8_t bv = (b && b->tag == KAI_BYTE) ? b->as.byte_val : 0;
    if (a) kai_decref(a);
    if (b) kai_decref(b);
    return kai_byte((uint8_t) (av + bv));
}

static KaiValue *kai_prelude_byte_sub(KaiValue *a, KaiValue *b) {
    uint8_t av = (a && a->tag == KAI_BYTE) ? a->as.byte_val : 0;
    uint8_t bv = (b && b->tag == KAI_BYTE) ? b->as.byte_val : 0;
    if (a) kai_decref(a);
    if (b) kai_decref(b);
    return kai_byte((uint8_t) (av - bv));
}

static KaiValue *kai_prelude_byte_eq(KaiValue *a, KaiValue *b) {
    uint8_t av = (a && a->tag == KAI_BYTE) ? a->as.byte_val : 0;
    uint8_t bv = (b && b->tag == KAI_BYTE) ? b->as.byte_val : 0;
    if (a) kai_decref(a);
    if (b) kai_decref(b);
    return kai_bool(av == bv);
}

static KaiValue *kai_prelude_byte_lt(KaiValue *a, KaiValue *b) {
    uint8_t av = (a && a->tag == KAI_BYTE) ? a->as.byte_val : 0;
    uint8_t bv = (b && b->tag == KAI_BYTE) ? b->as.byte_val : 0;
    if (a) kai_decref(a);
    if (b) kai_decref(b);
    return kai_bool(av < bv);
}

static KaiValue *kai_prelude_byte_to_string(KaiValue *v) {
    if (!v || v->tag != KAI_BYTE) { if (v) kai_decref(v); return kai_str("0"); }
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", (unsigned) v->as.byte_val);
    kai_decref(v);
    return kai_str(buf);
}

/* ---------- prelude: math/real libm bindings (issue #343) ----------
 * IEEE-754 pass-through: NaN / Inf propagate per C99 <math.h>.
 * Inspect with kai_prelude_real_is_nan / kai_prelude_real_is_inf. */

#define KAI_LIBM_REAL1(name, fn)                                         \
    static KaiValue *kai_prelude_real_##name(KaiValue *x) {              \
        double r = (x && x->tag == KAI_REAL) ? fn(x->as.r) : 0.0;        \
        KaiValue *out = kai_real(r);                                     \
        if (x) kai_decref(x);                                            \
        return out;                                                      \
    }

#define KAI_LIBM_REAL2(name, fn)                                         \
    static KaiValue *kai_prelude_real_##name(KaiValue *a, KaiValue *b) { \
        double av = (a && a->tag == KAI_REAL) ? a->as.r : 0.0;           \
        double bv = (b && b->tag == KAI_REAL) ? b->as.r : 0.0;           \
        KaiValue *out = kai_real(fn(av, bv));                            \
        if (a) kai_decref(a);                                            \
        if (b) kai_decref(b);                                            \
        return out;                                                      \
    }

KAI_LIBM_REAL1(sqrt,  sqrt)
KAI_LIBM_REAL1(cbrt,  cbrt)
KAI_LIBM_REAL1(exp,   exp)
KAI_LIBM_REAL1(log,   log)
KAI_LIBM_REAL1(log2,  log2)
KAI_LIBM_REAL1(log10, log10)
KAI_LIBM_REAL1(sin,   sin)
KAI_LIBM_REAL1(cos,   cos)
KAI_LIBM_REAL1(tan,   tan)
KAI_LIBM_REAL1(asin,  asin)
KAI_LIBM_REAL1(acos,  acos)
KAI_LIBM_REAL1(atan,  atan)
KAI_LIBM_REAL1(sinh,  sinh)
KAI_LIBM_REAL1(cosh,  cosh)
KAI_LIBM_REAL1(tanh,  tanh)

KAI_LIBM_REAL2(pow,   pow)
KAI_LIBM_REAL2(atan2, atan2)
KAI_LIBM_REAL2(rem,   fmod)

#undef KAI_LIBM_REAL1
#undef KAI_LIBM_REAL2

/* signum: -1.0 / 0.0 / +1.0; NaN passes through as NaN. */
static KaiValue *kai_prelude_real_signum(KaiValue *x) {
    double r = (x && x->tag == KAI_REAL) ? x->as.r : 0.0;
    double s;
    if (r != r)        s = r;          /* NaN */
    else if (r > 0.0)  s = 1.0;
    else if (r < 0.0)  s = -1.0;
    else               s = 0.0;
    KaiValue *out = kai_real(s);
    if (x) kai_decref(x);
    return out;
}

static KaiValue *kai_prelude_real_is_nan(KaiValue *x) {
    int yes = 0;
    if (x && x->tag == KAI_REAL) { double r = x->as.r; yes = (r != r); }
    KaiValue *out = kai_bool(yes);
    if (x) kai_decref(x);
    return out;
}

static KaiValue *kai_prelude_real_is_inf(KaiValue *x) {
    int yes = 0;
    if (x && x->tag == KAI_REAL) {
        double r = x->as.r;
        yes = (r > 1.7976931348623157e308) || (r < -1.7976931348623157e308);
    }
    KaiValue *out = kai_bool(yes);
    if (x) kai_decref(x);
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

/* ---------- prelude: refs (issue #257) ----------
 *
 * `Ref[T]` is a single-cell mutable reference in the `Mutable`
 * effect (docs/effects-stdlib.md §Mutable). Surface ops:
 *   ref_make[T](init: T)        : Ref[T]
 *   ref_get[T](r: Ref[T])       : T
 *   ref_set[T](r: Ref[T], v: T) : Unit
 *
 * Runtime layout: a Ref is a 1-slot KAI_ARRAY. Reusing the array
 * representation avoids a new tag plus parallel branches in
 * kai_decref / kai_to_string / kai_op_eq while preserving the
 * surface-level distinction enforced by the typer. */
static KaiValue *kai_prelude_ref_make(KaiValue *init) {
    /* kai_array_make increfs `init` once for the slot; we still
     * decref our own input ref under the callee-consumes
     * convention, mirroring kai_prelude_array_make. */
    KaiValue *r = kai_array_make(1, init);
    if (init) kai_decref(init);
    return r;
}

static KaiValue *kai_prelude_ref_get(KaiValue *r) {
    KaiValue *v = kai_array_get_impl(r, 0);
    if (r) kai_decref(r);
    return v;
}

static KaiValue *kai_prelude_ref_set(KaiValue *r, KaiValue *v) {
    /* kai_array_set_impl steals `v` (inserts into the slot) and
     * returns kai_incref(r). Surface contract is Unit-typed, so
     * drop the returned array ref and synthesise unit. */
    KaiValue *a = kai_array_set_impl(r, 0, kai_incref(v));
    if (a) kai_decref(a);
    if (r) kai_decref(r);
    if (v) kai_decref(v);
    return kai_unit();
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
        /* kai_apply consumes (#298): give it its own ref each iter. */
        KaiValue *head = kai_apply(kai_incref(f), 1, &arg0);
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
        /* kai_apply consumes (#298): incref f for each iter. */
        KaiValue *piece = kai_apply(kai_incref(f), 1, &arg0);
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
        /* kai_apply consumes (#298): incref pred for each iter. */
        KaiValue *keep = kai_apply(kai_incref(pred), 1, &arg0);
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
        /* kai_apply consumes (#298): incref f for each iter. */
        acc = kai_apply(kai_incref(f), 2, args);    /* closure produces fresh acc */
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
        /* kai_apply consumes (#298): incref f for each iter. */
        KaiValue *r = kai_apply(kai_incref(f), 1, &arg0);
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
#ifdef KAI_TRACE_RC
    kai_rc_strict_register_once();
#endif
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

static KAI_RC_NOINLINE KaiValue *kai_prelude_read_file(KaiValue *path) {
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

/* Issue #345: file_exists / file_delete / file_rename. Each consumes
 * its String args linearly (kai_decref before allocating the result),
 * matching the prelude convention used by read_file/write_file above. */

static KaiValue *kai_prelude_file_exists(KaiValue *path) {
    int present = 0;
    if (path && path->tag == KAI_STR) {
        char pbuf[4096];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        present = (access(pbuf, F_OK) == 0) ? 1 : 0;
    }
    if (path) kai_decref(path);
    return kai_bool(present);
}

static KaiValue *kai_prelude_file_delete(KaiValue *path) {
    KaiValue *r = NULL;
    if (!path || path->tag != KAI_STR) {
        KaiValue *msg = kai_str("file_delete: path is not a String");
        r = kai_variant(0, "Err", 1, &msg);
    } else {
        char pbuf[4096];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        if (unlink(pbuf) == 0) {
            KaiValue *u = kai_unit();
            r = kai_variant(0, "Ok", 1, &u);
        } else {
            KaiValue *msg = kai_str("file_delete: unlink failed");
            r = kai_variant(0, "Err", 1, &msg);
        }
    }
    if (path) kai_decref(path);
    return r;
}

static KaiValue *kai_prelude_file_rename(KaiValue *from, KaiValue *to) {
    KaiValue *r = NULL;
    if (!from || from->tag != KAI_STR) {
        KaiValue *msg = kai_str("file_rename: from is not a String");
        r = kai_variant(0, "Err", 1, &msg);
    } else if (!to || to->tag != KAI_STR) {
        KaiValue *msg = kai_str("file_rename: to is not a String");
        r = kai_variant(0, "Err", 1, &msg);
    } else {
        char fbuf[4096];
        char tbuf[4096];
        size_t flen = from->as.s.len < sizeof(fbuf) - 1 ? from->as.s.len : sizeof(fbuf) - 1;
        size_t tlen = to->as.s.len   < sizeof(tbuf) - 1 ? to->as.s.len   : sizeof(tbuf) - 1;
        memcpy(fbuf, from->as.s.bytes, flen); fbuf[flen] = '\0';
        memcpy(tbuf, to->as.s.bytes,   tlen); tbuf[tlen] = '\0';
        if (rename(fbuf, tbuf) == 0) {
            KaiValue *u = kai_unit();
            r = kai_variant(0, "Ok", 1, &u);
        } else {
            KaiValue *msg = kai_str("file_rename: rename failed");
            r = kai_variant(0, "Err", 1, &msg);
        }
    }
    if (from) kai_decref(from);
    if (to)   kai_decref(to);
    return r;
}

/* Issue #482 (follow-up to #345) + Array[Byte] refactor (prereq for
 * #452): binary file IO. Operates on `Array[Byte]` so the buffer
 * lines up with BinSerialize post-#488 (O(1) reads, contiguous
 * storage). Both primitives consume their args linearly (kai_decref
 * before allocating the result), matching the convention used by the
 * text variants. */

static KaiValue *kai_prelude_file_read_bytes(KaiValue *path) {
    KaiValue *r = NULL;
    if (!path || path->tag != KAI_STR) {
        KaiValue *msg = kai_str("file_read_bytes: argument is not a String");
        r = kai_variant(0, "Err", 1, &msg);
    } else {
        char pbuf[4096];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        FILE *fp = fopen(pbuf, "rb");
        if (!fp) {
            KaiValue *msg = kai_str("file_read_bytes: cannot open file");
            r = kai_variant(0, "Err", 1, &msg);
        } else if (fseek(fp, 0, SEEK_END) != 0) {
            fclose(fp);
            KaiValue *msg = kai_str("file_read_bytes: seek failed");
            r = kai_variant(0, "Err", 1, &msg);
        } else {
            long n = ftell(fp);
            if (n < 0) {
                fclose(fp);
                KaiValue *msg = kai_str("file_read_bytes: tell failed");
                r = kai_variant(0, "Err", 1, &msg);
            } else if (fseek(fp, 0, SEEK_SET) != 0) {
                fclose(fp);
                KaiValue *msg = kai_str("file_read_bytes: rewind failed");
                r = kai_variant(0, "Err", 1, &msg);
            } else {
                /* Read into a flat C buffer, then publish into a
                 * fresh KAI_ARRAY one KAI_BYTE allocation per slot.
                 * Allocating the kai_array directly (instead of
                 * kai_array_make + array_set in a loop) avoids the
                 * redundant default-incref/decref pair on every
                 * position. */
                unsigned char *buf = (unsigned char *) malloc((size_t) n + 1);
                if (!buf) { fclose(fp); fprintf(stderr, "kai: out of memory\n"); exit(1); }
                size_t got = fread(buf, 1, (size_t) n, fp);
                fclose(fp);
                KaiValue *arr = kai_alloc(KAI_ARRAY);
                arr->as.arr.len = (int64_t) got;
                arr->as.arr.cap = got > 0 ? (int64_t) got : 1;
                arr->as.arr.items = (KaiValue **) malloc((size_t) arr->as.arr.cap * sizeof(KaiValue *));
                if (!arr->as.arr.items) { free(buf); fprintf(stderr, "kai: out of memory\n"); exit(1); }
                for (size_t i = 0; i < got; ++i) arr->as.arr.items[i] = kai_byte(buf[i]);
                free(buf);
                r = kai_variant(0, "Ok", 1, &arr);
            }
        }
    }
    if (path) kai_decref(path);
    return r;
}

static KaiValue *kai_prelude_file_write_bytes(KaiValue *path, KaiValue *bytes) {
    KaiValue *r = NULL;
    if (!path || path->tag != KAI_STR) {
        KaiValue *msg = kai_str("file_write_bytes: path is not a String");
        r = kai_variant(0, "Err", 1, &msg);
    } else if (!bytes || bytes->tag != KAI_ARRAY) {
        KaiValue *msg = kai_str("file_write_bytes: buffer is not an Array");
        r = kai_variant(0, "Err", 1, &msg);
    } else {
        char pbuf[4096];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        FILE *fp = fopen(pbuf, "wb");
        if (!fp) {
            KaiValue *msg = kai_str("file_write_bytes: cannot open file");
            r = kai_variant(0, "Err", 1, &msg);
        } else {
            /* Pack the kai_array into a contiguous C buffer, then
             * issue a single fwrite. Cheaper than per-slot fwrite
             * even with stdio buffering, and lets us fail fast on
             * a non-Byte slot before any IO. */
            int64_t n = bytes->as.arr.len;
            int ok = 1;
            unsigned char *out = NULL;
            if (n > 0) {
                out = (unsigned char *) malloc((size_t) n);
                if (!out) { fclose(fp); fprintf(stderr, "kai: out of memory\n"); exit(1); }
                for (int64_t i = 0; i < n; ++i) {
                    KaiValue *e = bytes->as.arr.items[i];
                    if (!e || e->tag != KAI_BYTE) { ok = 0; break; }
                    out[i] = e->as.byte_val;
                }
            }
            if (ok && n > 0) {
                if (fwrite(out, 1, (size_t) n, fp) != (size_t) n) ok = 0;
            }
            if (out) free(out);
            fclose(fp);
            if (!ok) {
                KaiValue *msg = kai_str("file_write_bytes: write failed");
                r = kai_variant(0, "Err", 1, &msg);
            } else {
                KaiValue *u = kai_unit();
                r = kai_variant(0, "Ok", 1, &u);
            }
        }
    }
    if (path)  kai_decref(path);
    if (bytes) kai_decref(bytes);
    return r;
}

/* Issue #344: directory ops on top of the `File` effect. Each consumes
 * its String args linearly (kai_decref before allocating the result),
 * mirroring the file_exists/_delete/_rename convention. POSIX only
 * (macOS + Linux). Return shapes:
 *
 *   dir_list_dir(path)    : [String]              — entries (no . / ..)
 *   dir_create_dir(path)  : Result[String, Unit]  — Err message first
 *   dir_remove_dir(path)  : Result[String, Unit]
 *   dir_walk(path)        : [String]              — files (not dirs),
 *                                                   depth-first; symlinks
 *                                                   are NOT followed in v1
 *
 * dir_list_dir returns the empty list on read errors (matching how
 * args() can be empty); use dir_create_dir / dir_remove_dir when an
 * explicit Result is wanted. */

static KaiValue *kai_prelude_dir_list_dir(KaiValue *path) {
    KaiValue *acc = kai_nil();
    if (path && path->tag == KAI_STR) {
        char pbuf[4096];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        DIR *d = opendir(pbuf);
        if (d) {
            /* Buffer entries then prepend in reverse so the caller sees
             * them in readdir order. readdir order is filesystem-defined
             * but stable for a given mount, which keeps test output
             * deterministic enough for fixtures. */
            size_t cap = 16, n = 0;
            char **names = (char **) malloc(cap * sizeof(char *));
            if (!names) { closedir(d); fprintf(stderr, "kai: out of memory\n"); exit(1); }
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                const char *nm = e->d_name;
                if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0'))) continue;
                if (n + 1 > cap) {
                    cap *= 2;
                    names = (char **) realloc(names, cap * sizeof(char *));
                    if (!names) { closedir(d); fprintf(stderr, "kai: out of memory\n"); exit(1); }
                }
                size_t len = strlen(nm);
                char *copy = (char *) malloc(len + 1);
                if (!copy) { closedir(d); fprintf(stderr, "kai: out of memory\n"); exit(1); }
                memcpy(copy, nm, len + 1);
                names[n++] = copy;
            }
            closedir(d);
            for (size_t i = n; i > 0;) {
                --i;
                acc = kai_cons(kai_str(names[i]), acc);
                free(names[i]);
            }
            free(names);
        }
    }
    if (path) kai_decref(path);
    return acc;
}

static KaiValue *kai_prelude_dir_create_dir(KaiValue *path) {
    KaiValue *r = NULL;
    if (!path || path->tag != KAI_STR) {
        KaiValue *msg = kai_str("dir_create_dir: path is not a String");
        r = kai_variant(0, "Err", 1, &msg);
    } else {
        char pbuf[4096];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        if (mkdir(pbuf, 0755) == 0) {
            KaiValue *u = kai_unit();
            r = kai_variant(0, "Ok", 1, &u);
        } else {
            KaiValue *msg = kai_str("dir_create_dir: mkdir failed");
            r = kai_variant(0, "Err", 1, &msg);
        }
    }
    if (path) kai_decref(path);
    return r;
}

static KaiValue *kai_prelude_dir_remove_dir(KaiValue *path) {
    KaiValue *r = NULL;
    if (!path || path->tag != KAI_STR) {
        KaiValue *msg = kai_str("dir_remove_dir: path is not a String");
        r = kai_variant(0, "Err", 1, &msg);
    } else {
        char pbuf[4096];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        if (rmdir(pbuf) == 0) {
            KaiValue *u = kai_unit();
            r = kai_variant(0, "Ok", 1, &u);
        } else {
            KaiValue *msg = kai_str("dir_remove_dir: rmdir failed");
            r = kai_variant(0, "Err", 1, &msg);
        }
    }
    if (path) kai_decref(path);
    return r;
}

/* Iterative depth-first walk. Push subdirectories onto an explicit stack
 * so a deep tree doesn't blow the C stack. Symlinks are NOT followed
 * (v1 contract): use lstat + S_ISLNK skip so a symlink loop can't
 * trap the walker. Only regular files are emitted; directories are
 * traversed but not reported. */
static KaiValue *kai_prelude_dir_walk(KaiValue *root) {
    KaiValue *acc = kai_nil();
    if (!root || root->tag != KAI_STR) {
        if (root) kai_decref(root);
        return acc;
    }

    size_t cap = 16, top = 0;
    char **stack = (char **) malloc(cap * sizeof(char *));
    if (!stack) { fprintf(stderr, "kai: out of memory\n"); exit(1); }

    char rbuf[4096];
    size_t rlen = root->as.s.len < sizeof(rbuf) - 1 ? root->as.s.len : sizeof(rbuf) - 1;
    memcpy(rbuf, root->as.s.bytes, rlen);
    rbuf[rlen] = '\0';
    {
        char *copy = (char *) malloc(rlen + 1);
        if (!copy) { free(stack); fprintf(stderr, "kai: out of memory\n"); exit(1); }
        memcpy(copy, rbuf, rlen + 1);
        stack[top++] = copy;
    }

    /* Collect files first, then prepend in reverse so the cons list
     * reads in walk order. */
    size_t fcap = 32, fn = 0;
    char **files = (char **) malloc(fcap * sizeof(char *));
    if (!files) { free(stack[0]); free(stack); fprintf(stderr, "kai: out of memory\n"); exit(1); }

    while (top > 0) {
        char *dir_path = stack[--top];
        DIR *d = opendir(dir_path);
        if (!d) { free(dir_path); continue; }
        /* Buffer child entries so we can push subdirs in reverse for
         * deterministic depth-first order. */
        size_t ccap = 16, cn = 0;
        char **child_paths = (char **) malloc(ccap * sizeof(char *));
        if (!child_paths) { closedir(d); free(dir_path); fprintf(stderr, "kai: out of memory\n"); exit(1); }
        char *child_kinds = (char *) malloc(ccap); /* 'f' / 'd' / 's' (skip) */
        if (!child_kinds) { free(child_paths); closedir(d); free(dir_path); fprintf(stderr, "kai: out of memory\n"); exit(1); }
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            const char *nm = e->d_name;
            if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0'))) continue;
            size_t dlen = strlen(dir_path);
            size_t nlen = strlen(nm);
            char *full = (char *) malloc(dlen + 1 + nlen + 1);
            if (!full) { closedir(d); free(dir_path); fprintf(stderr, "kai: out of memory\n"); exit(1); }
            memcpy(full, dir_path, dlen);
            int needs_sep = (dlen > 0 && dir_path[dlen - 1] != '/');
            size_t off = dlen;
            if (needs_sep) { full[off++] = '/'; }
            memcpy(full + off, nm, nlen + 1);
            struct stat st;
            char kind = 's';
            if (lstat(full, &st) == 0) {
                if (S_ISLNK(st.st_mode))      kind = 's'; /* skip symlinks */
                else if (S_ISDIR(st.st_mode)) kind = 'd';
                else if (S_ISREG(st.st_mode)) kind = 'f';
                else                          kind = 's'; /* sockets, fifos, etc. */
            }
            if (cn + 1 > ccap) {
                ccap *= 2;
                child_paths = (char **) realloc(child_paths, ccap * sizeof(char *));
                child_kinds = (char *)  realloc(child_kinds, ccap);
                if (!child_paths || !child_kinds) { closedir(d); free(dir_path); fprintf(stderr, "kai: out of memory\n"); exit(1); }
            }
            child_paths[cn] = full;
            child_kinds[cn] = kind;
            cn++;
        }
        closedir(d);
        free(dir_path);
        /* Emit files in encountered order. */
        for (size_t i = 0; i < cn; ++i) {
            char k = child_kinds[i];
            if (k == 'f') {
                if (fn + 1 > fcap) {
                    fcap *= 2;
                    files = (char **) realloc(files, fcap * sizeof(char *));
                    if (!files) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
                }
                files[fn++] = child_paths[i];
            }
        }
        /* Push subdirs in reverse onto the stack so they pop in natural order. */
        for (size_t i = cn; i > 0;) {
            --i;
            char k = child_kinds[i];
            if (k == 'd') {
                if (top + 1 > cap) {
                    cap *= 2;
                    stack = (char **) realloc(stack, cap * sizeof(char *));
                    if (!stack) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
                }
                stack[top++] = child_paths[i];
            } else if (k != 'f') {
                /* skipped — free the path string */
                free(child_paths[i]);
            }
        }
        free(child_paths);
        free(child_kinds);
    }
    free(stack);

    /* Build the cons list in reverse so consumers see files in walk order. */
    for (size_t i = fn; i > 0;) {
        --i;
        acc = kai_cons(kai_str(files[i]), acc);
        free(files[i]);
    }
    free(files);

    kai_decref(root);
    return acc;
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

/* Issue #453: byte-oriented stdin read. Reads up to `n` raw bytes
 * from stdin (including '\n') and returns them as a String. On EOF
 * the returned String is shorter than `n` — possibly empty. Used
 * by LSP-style framed protocols where the body length is known up
 * front and may contain newlines. */
static KaiValue *kai_prelude_read_bytes(KaiValue *n) {
    int64_t want = 0;
    if (n && n->tag == KAI_INT && n->as.i > 0) want = n->as.i;
    if (n) kai_decref(n);
    if (want <= 0) return kai_str_from_bytes("", 0);
    char *buf = (char *) malloc((size_t) want);
    if (!buf) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    size_t got = 0;
    while (got < (size_t) want) {
        size_t r = fread(buf + got, 1, (size_t) want - got, stdin);
        if (r == 0) break;
        got += r;
    }
    KaiValue *s = kai_str_from_bytes(buf, got);
    free(buf);
    return s;
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

/* Build a 1-byte String holding the low 8 bits of `n`. Unlike
 * `"#{int_to_char(n)}"` interpolation, which routes through
 * `kai_to_string(KAI_CHAR)` + `Show for Char` and escape-renders any
 * byte outside ASCII [32,126] (and truncates NUL via `strlen`), this
 * builtin constructs the string via `kai_str_from_bytes` so every
 * value 0..255 round-trips byte-exact. Cache layer (issue #592) uses
 * it for the KAB2 binary on-disk format. Documented limitation
 * sidebar in `stdlib/protocols.kai` (BinSerialize String/Real ASCII-
 * only) closes once Phase A/B serdes routes through this. */
static KaiValue *kai_prelude_int_to_byte_string(KaiValue *n) {
    int64_t v = (n && n->tag == KAI_INT) ? n->as.i : 0;
    if (n) kai_decref(n);
    unsigned char b = (unsigned char) (v & 0xff);
    return kai_str_from_bytes((const char *) &b, 1);
}

/* Read one byte at index `i` of String `s`, returning it as an Int
 * 0..255. Returns -1 on out-of-bounds or wrong type. Faster than
 * `match char_at(s, i) { Some(c) -> char_to_int(c); None -> -1 }`
 * because it avoids the Option allocation + Char alloc + decref
 * chain (KAI_CHAR is cached for value < 128 but still hits a load
 * + tag check). The KAB2 cache decoder (#592) calls this ~1M times
 * per warm load; replacing Option-wrapped char_at cuts decoder wall
 * from ~0.26s to under 0.04s. */
static KaiValue *kai_prelude_string_byte_at_int(KaiValue *s, KaiValue *i) {
    int64_t v = -1;
    if (s && s->tag == KAI_STR && i && i->tag == KAI_INT) {
        int64_t idx = i->as.i;
        if (idx >= 0 && (size_t) idx < s->as.s.len) {
            v = (int64_t)(unsigned char) s->as.s.bytes[idx];
        }
    }
    if (s) kai_decref(s);
    if (i) kai_decref(i);
    return kai_int(v);
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
static KaiValue *_kai_prelude_real_sqrt_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_prelude_real_sqrt(a[0]); }
static KaiValue *_kai_prelude_real_cbrt_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_prelude_real_cbrt(a[0]); }
static KaiValue *_kai_prelude_real_exp_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_prelude_real_exp(a[0]); }
static KaiValue *_kai_prelude_real_log_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_prelude_real_log(a[0]); }
static KaiValue *_kai_prelude_real_log2_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_prelude_real_log2(a[0]); }
static KaiValue *_kai_prelude_real_log10_thunk(KaiValue *s, KaiValue **a, int n)     { (void) s; (void) n; return kai_prelude_real_log10(a[0]); }
static KaiValue *_kai_prelude_real_sin_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_prelude_real_sin(a[0]); }
static KaiValue *_kai_prelude_real_cos_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_prelude_real_cos(a[0]); }
static KaiValue *_kai_prelude_real_tan_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_prelude_real_tan(a[0]); }
static KaiValue *_kai_prelude_real_asin_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_prelude_real_asin(a[0]); }
static KaiValue *_kai_prelude_real_acos_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_prelude_real_acos(a[0]); }
static KaiValue *_kai_prelude_real_atan_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_prelude_real_atan(a[0]); }
static KaiValue *_kai_prelude_real_sinh_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_prelude_real_sinh(a[0]); }
static KaiValue *_kai_prelude_real_cosh_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_prelude_real_cosh(a[0]); }
static KaiValue *_kai_prelude_real_tanh_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_prelude_real_tanh(a[0]); }
static KaiValue *_kai_prelude_real_signum_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_real_signum(a[0]); }
static KaiValue *_kai_prelude_real_is_nan_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_real_is_nan(a[0]); }
static KaiValue *_kai_prelude_real_is_inf_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_real_is_inf(a[0]); }
static KaiValue *_kai_prelude_real_pow_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_prelude_real_pow(a[0], a[1]); }
static KaiValue *_kai_prelude_real_atan2_thunk(KaiValue *s, KaiValue **a, int n)     { (void) s; (void) n; return kai_prelude_real_atan2(a[0], a[1]); }
static KaiValue *_kai_prelude_real_rem_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_prelude_real_rem(a[0], a[1]); }
static KaiValue *_kai_prelude_string_length_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_prelude_string_length(a[0]); }
static KaiValue *_kai_prelude_string_concat_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_prelude_string_concat(a[0], a[1]); }
static KaiValue *_kai_prelude_string_concat_all_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_string_concat_all(a[0]); }
static KaiValue *_kai_prelude_string_join_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_prelude_string_join(a[0], a[1]); }
static KaiValue *_kai_prelude_array_make_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_array_make(a[0], a[1]); }
static KaiValue *_kai_prelude_array_length_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_prelude_array_length(a[0]); }
static KaiValue *_kai_prelude_array_get_thunk(KaiValue *s, KaiValue **a, int n)     { (void) s; (void) n; return kai_prelude_array_get(a[0], a[1]); }
static KaiValue *_kai_prelude_array_set_thunk(KaiValue *s, KaiValue **a, int n)     { (void) s; (void) n; return kai_prelude_array_set(a[0], a[1], a[2]); }
static KaiValue *_kai_prelude_array_grow_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_array_grow(a[0], a[1], a[2]); }
static KaiValue *_kai_prelude_ref_make_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_prelude_ref_make(a[0]); }
static KaiValue *_kai_prelude_ref_get_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_prelude_ref_get(a[0]); }
static KaiValue *_kai_prelude_ref_set_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_prelude_ref_set(a[0], a[1]); }
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
static KaiValue *_kai_prelude_file_exists_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_file_exists(a[0]); }
static KaiValue *_kai_prelude_file_delete_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_file_delete(a[0]); }
static KaiValue *_kai_prelude_file_rename_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_file_rename(a[0], a[1]); }
static KaiValue *_kai_prelude_file_read_bytes_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_prelude_file_read_bytes(a[0]); }
static KaiValue *_kai_prelude_file_write_bytes_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_file_write_bytes(a[0], a[1]); }
static KaiValue *_kai_prelude_dir_list_dir_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_prelude_dir_list_dir(a[0]); }
static KaiValue *_kai_prelude_dir_create_dir_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_dir_create_dir(a[0]); }
static KaiValue *_kai_prelude_dir_remove_dir_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_dir_remove_dir(a[0]); }
static KaiValue *_kai_prelude_dir_walk_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_prelude_dir_walk(a[0]); }
static KaiValue *_kai_prelude_read_line_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) a; (void) n; return kai_prelude_read_line(); }
static KaiValue *_kai_prelude_read_bytes_thunk(KaiValue *s, KaiValue **a, int n)     { (void) s; (void) n; return kai_prelude_read_bytes(a[0]); }
static KaiValue *_kai_prelude_string_to_int_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_prelude_string_to_int(a[0]); }
static KaiValue *_kai_prelude_string_to_real_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_string_to_real(a[0]); }
static KaiValue *_kai_prelude_char_at_thunk(KaiValue *s, KaiValue **a, int n)        { (void) s; (void) n; return kai_prelude_char_at(a[0], a[1]); }
static KaiValue *_kai_prelude_string_split_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_prelude_string_split(a[0], a[1]); }
static KaiValue *_kai_prelude_string_contains_thunk(KaiValue *s, KaiValue **a, int n){ (void) s; (void) n; return kai_prelude_string_contains(a[0], a[1]); }
static KaiValue *_kai_prelude_string_slice_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_prelude_string_slice(a[0], a[1], a[2]); }
static KaiValue *_kai_prelude_char_to_int_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_char_to_int(a[0]); }
static KaiValue *_kai_prelude_int_to_char_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_int_to_char(a[0]); }
static KaiValue *_kai_prelude_int_to_byte_string_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_int_to_byte_string(a[0]); }
static KaiValue *_kai_prelude_string_byte_at_int_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_string_byte_at_int(a[0], a[1]); }
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
 * bench v1.x (issue #437): per-iteration timings collected into a
 * sample buffer; on finalize we sort and report median + MAD + mean
 * + range. The emitted bench wrapper runs KAI_BENCH_WARMUP untimed
 * iterations first to take JIT/cache effects out of the timed
 * window, then KAI_BENCH_ITERS timed iterations. Both knobs read
 * from getenv() at startup so the user can override without
 * recompiling. KAI_BENCH_ITERS_DEFAULT is the compile-time fallback.
 *
 * Selfhost-bench (the compiler measuring itself) deferred to v1.y;
 * see issue #40 for the split plan.
 */

#define KAI_BENCH_ITERS_DEFAULT  1000
#define KAI_BENCH_WARMUP_DEFAULT 50

static int kai_bench_count_total = 0;

static int kai_bench_iters_cached = -1;
static int kai_bench_warmup_cached = -1;

static int kai_bench_parse_int_env(const char *name, int fallback) {
    const char *raw = getenv(name);
    if (!raw || !*raw) return fallback;
    long v = 0;
    const char *p = raw;
    int sign = 1;
    if (*p == '-') { sign = -1; p++; }
    if (!*p) return fallback;
    while (*p) {
        if (*p < '0' || *p > '9') return fallback;
        v = v * 10 + (*p - '0');
        if (v > 100000000L) return fallback;
        p++;
    }
    v *= sign;
    if (v < 0) return fallback;
    return (int)v;
}

static int kai_bench_iters(void) {
    if (kai_bench_iters_cached < 0) {
        int v = kai_bench_parse_int_env("KAI_BENCH_ITERS", KAI_BENCH_ITERS_DEFAULT);
        if (v < 1) v = 1;
        kai_bench_iters_cached = v;
    }
    return kai_bench_iters_cached;
}

static int kai_bench_warmup(void) {
    if (kai_bench_warmup_cached < 0) {
        int v = kai_bench_parse_int_env("KAI_BENCH_WARMUP", KAI_BENCH_WARMUP_DEFAULT);
        if (v < 0) v = 0;
        kai_bench_warmup_cached = v;
    }
    return kai_bench_warmup_cached;
}

static long long kai_bench_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

/* Per-bench sample buffer. One bench at a time runs to completion
   before the next one starts (the emitted main calls them in
   sequence), so a single shared buffer is enough — we just realloc
   it lazily to the high-water mark. */
static long long *kai_bench_samples = NULL;
static int kai_bench_samples_cap = 0;

static void kai_bench_ensure_capacity(int n) {
    if (n <= kai_bench_samples_cap) return;
    long long *r = (long long *)realloc(kai_bench_samples, (size_t)n * sizeof(long long));
    if (!r) {
        fprintf(stderr, "kai_bench: out of memory reserving %d samples\n", n);
        exit(1);
    }
    kai_bench_samples = r;
    kai_bench_samples_cap = n;
}

static void kai_bench_record(int idx, long long sample_ns) {
    /* Caller has already ensured capacity. Out-of-range index
       silently drops — defensive against emitter bugs that miscount
       iterations. */
    if (idx < 0 || idx >= kai_bench_samples_cap) return;
    kai_bench_samples[idx] = sample_ns;
}

static int kai_bench_ll_cmp(const void *a, const void *b) {
    long long la = *(const long long *)a;
    long long lb = *(const long long *)b;
    if (la < lb) return -1;
    if (la > lb) return 1;
    return 0;
}

static long long kai_bench_median_sorted(const long long *xs, int n) {
    if (n <= 0) return 0;
    return xs[n / 2];
}

static void kai_bench_finalize(const char *desc, int iters) {
    /* Samples already populated by kai_bench_record(0..iters-1). */
    if (iters <= 0) {
        fprintf(stderr, "  %s: 0 iter / median 0 ns / MAD 0 ns / mean 0 ns / range [0, 0]\n",
                desc ? desc : "(unnamed)");
        kai_bench_count_total++;
        return;
    }
    long long *xs = kai_bench_samples;
    long long total = 0;
    long long mn = xs[0];
    long long mx = xs[0];
    for (int i = 0; i < iters; i++) {
        long long s = xs[i];
        total += s;
        if (s < mn) mn = s;
        if (s > mx) mx = s;
    }
    long long mean = total / (long long)iters;
    qsort(xs, (size_t)iters, sizeof(long long), kai_bench_ll_cmp);
    long long median = kai_bench_median_sorted(xs, iters);
    /* MAD: median(|x_i - median|). Reuse the sample buffer for the
       deviations — we're done with the sorted samples. */
    for (int i = 0; i < iters; i++) {
        long long d = xs[i] - median;
        if (d < 0) d = -d;
        xs[i] = d;
    }
    qsort(xs, (size_t)iters, sizeof(long long), kai_bench_ll_cmp);
    long long mad = kai_bench_median_sorted(xs, iters);
    fprintf(stderr,
            "  %s: %d iter / median %lld ns / MAD %lld ns / mean %lld ns / range [%lld, %lld]\n",
            desc ? desc : "(unnamed)", iters, median, mad, mean, mn, mx);
    kai_bench_count_total++;
}

static int kai_bench_summary(void) {
    fprintf(stderr, "\n%d benches\n", kai_bench_count_total);
    if (kai_bench_samples) {
        free(kai_bench_samples);
        kai_bench_samples = NULL;
        kai_bench_samples_cap = 0;
    }
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

/* ---------- shrinkers (issue #438) ---------------------------------
 * Each kai_shrink_<T>(v) returns a NEW KaiValue* (rc=1) that is one
 * greedy step closer to the canonical minimum, or NULL when no smaller
 * candidate exists. Callers own both the input (untouched, still must
 * be decref'd by the caller as before) and the returned value.
 *
 * Strategy is greedy and per-type:
 *   Int    : halve toward 0 (sign-preserving).
 *   Bool   : true → false; false → NULL.
 *   Char   : bisect toward 'a'; if c == 'a' then NULL.
 *   String : delete first byte; else if non-empty, shrink first non-'a'
 *            byte toward 'a'; else NULL.
 *   List   : delete head element; else shrink first element via the
 *            per-T element shrinker; else NULL.
 *
 * Greedy means each step produces ONE candidate; the runner accepts it
 * only if the predicate still fails. If accepted the runner shrinks
 * again from the new candidate; if rejected the runner asks for the
 * NEXT alternative — encoded by re-calling the shrinker on the same
 * input, which is fine because each shrinker is deterministic and the
 * runner falls through to a different param after one rejection (the
 * only branch we care about for v1.x; integrated multi-strategy
 * backtracking is post-1.0 per #438 out-of-scope). */

static KaiValue *kai_shrink_int(KaiValue *v) {
    if (!v || v->tag != KAI_INT) return NULL;
    int64_t x = v->as.i;
    if (x == 0) return NULL;
    /* Halve toward zero. Integer division on negatives in C is
       truncation toward zero (C99 §6.5.5/6), so x/2 already does the
       right thing for both signs (e.g. -8/2 = -4, -1/2 = 0). */
    return kai_int(x / 2);
}

static KaiValue *kai_shrink_bool(KaiValue *v) {
    if (!v || v->tag != KAI_BOOL) return NULL;
    if (v->as.b == 0) return NULL;
    return kai_bool(0);
}

static KaiValue *kai_shrink_char(KaiValue *v) {
    if (!v || v->tag != KAI_CHAR) return NULL;
    uint32_t c = v->as.c;
    if (c == (uint32_t) 'a') return NULL;
    /* Bisect toward 'a'. Converges in O(log) steps. */
    uint32_t a = (uint32_t) 'a';
    uint32_t next = (c > a) ? (a + (c - a) / 2) : (c + (a - c) / 2);
    if (next == c) next = a;  /* defensive: ensure progress */
    return kai_char(next);
}

static KaiValue *kai_shrink_string(KaiValue *v) {
    if (!v || v->tag != KAI_STR) return NULL;
    size_t len = v->as.s.len;
    if (len > 0) {
        /* Delete first byte. */
        return kai_str_from_bytes(v->as.s.bytes + 1, len - 1);
    }
    return NULL;
}

/* List shrinker factory. Two strategies:
   A) drop head — biggest single reduction, collapses toward nil.
   B) elem shrink — walks the spine looking for the FIRST element
      that still has a smaller form via ELEM_SHRINK and rebuilds the
      list with that one element replaced. Returns NULL if every
      element is at its own minimum.
   The walk-and-replace pattern gives strategy B a deterministic
   linear sweep across the full list, not just the head, so e.g.
   [0, -30] still has a productive shrink (rebuild to [0, -15]). */
#define KAI_DEFINE_SHRINK_LIST(NAME, ELEM_SHRINK)                          \
    static KaiValue *NAME(KaiValue *v) {                                   \
        if (!v) return NULL;                                               \
        if (v->tag == KAI_NIL) return NULL;                                \
        if (v->tag != KAI_CONS) return NULL;                               \
        KaiValue *tail = v->as.cons.tail;                                  \
        kai_incref(tail);                                                  \
        return tail;                                                       \
    }                                                                      \
    /* Rebuild prefix (in reverse) onto a new tail. Helper for strategy B. \
       Each element in `rev_prefix` is a borrowed pointer and is incref'd  \
       as it lands in the new spine; the new spine takes ownership of the  \
       passed-in `tail` (no extra incref). */                              \
    static KaiValue *NAME##_rebuild(KaiValue **rev_prefix, int n,          \
                                    KaiValue *tail) {                      \
        KaiValue *acc = tail;                                              \
        for (int i = 0; i < n; i++) {                                      \
            KaiValue *h = rev_prefix[i];                                   \
            kai_incref(h);                                                 \
            acc = kai_cons(h, acc);                                        \
        }                                                                  \
        return acc;                                                        \
    }                                                                      \
    static KaiValue *NAME##_head(KaiValue *v) {                            \
        if (!v || v->tag != KAI_CONS) return NULL;                         \
        /* First pass: walk to find the first shrinkable element. */       \
        KaiValue *prefix[64];                                              \
        int prefix_n = 0;                                                  \
        KaiValue *cur = v;                                                 \
        while (cur != NULL && cur->tag == KAI_CONS) {                      \
            KaiValue *h = cur->as.cons.head;                               \
            KaiValue *h2 = ELEM_SHRINK(h);                                 \
            if (h2 != NULL) {                                              \
                /* Build new list: h2 :: cur->tail (incref'd) prepended    \
                   by the walked prefix in reverse. */                     \
                KaiValue *new_tail = cur->as.cons.tail;                    \
                kai_incref(new_tail);                                      \
                KaiValue *acc = kai_cons(h2, new_tail);                    \
                acc = NAME##_rebuild(prefix, prefix_n, acc);               \
                return acc;                                                \
            }                                                              \
            if (prefix_n >= 64) return NULL; /* bound spine walk */         \
            prefix[prefix_n++] = h;                                        \
            cur = cur->as.cons.tail;                                       \
        }                                                                  \
        return NULL;                                                       \
    }
KAI_DEFINE_SHRINK_LIST(kai_shrink_list_int,    kai_shrink_int)
KAI_DEFINE_SHRINK_LIST(kai_shrink_list_bool,   kai_shrink_bool)
KAI_DEFINE_SHRINK_LIST(kai_shrink_list_char,   kai_shrink_char)
KAI_DEFINE_SHRINK_LIST(kai_shrink_list_string, kai_shrink_string)
#undef KAI_DEFINE_SHRINK_LIST

/* Per-process shrink-iteration cap. KAI_CHECK_SHRINK_ITERS in the env
   overrides; default 200 per #438. Read once and memoised so we don't
   pay getenv cost per shrink step. */
#define KAI_CHECK_SHRINK_ITERS_DEFAULT 200
static int kai_check_shrink_iters_cached = -1;
static int kai_check_shrink_iters_limit(void) {
    if (kai_check_shrink_iters_cached >= 0) return kai_check_shrink_iters_cached;
    const char *env = getenv("KAI_CHECK_SHRINK_ITERS");
    int n = KAI_CHECK_SHRINK_ITERS_DEFAULT;
    if (env && *env) {
        int parsed = atoi(env);
        if (parsed >= 0) n = parsed;
    }
    kai_check_shrink_iters_cached = n;
    return n;
}

/* Counterexample-buffer side channel for shrinking. When a failure
   triggers shrinking, the runner first snapshots the original cx
   buffer here so the final report can show "<orig> shrunk to <min>".
   Bounded by the same KAI_CHECK_CX_BUF cap as the live buffer. */
static char kai_check_orig_cx_buf[KAI_CHECK_CX_BUF];
static int  kai_check_has_orig_cx = 0;

static void kai_check_cx_save_orig(void) {
    size_t n = kai_check_cx_len;
    if (n >= KAI_CHECK_CX_BUF) n = KAI_CHECK_CX_BUF - 1;
    memcpy(kai_check_orig_cx_buf, kai_check_cx_buf, n);
    kai_check_orig_cx_buf[n] = '\0';
    kai_check_has_orig_cx = 1;
}

/* Reports a shrunk counterexample. If the shrink loop produced a
   strictly smaller value, prints both forms; otherwise (no progress)
   degrades to the v1 single-form output so noise stays low. */
static void kai_check_fail_shrunk(int iter_at) {
    if (kai_check_has_orig_cx
        && strcmp(kai_check_orig_cx_buf, kai_check_cx_buf) != 0) {
        fprintf(stderr,
                "  %s: counterexample at iter %d: %s, shrunk to %s\n",
                kai_check_current_desc ? kai_check_current_desc : "(unnamed)",
                iter_at, kai_check_orig_cx_buf, kai_check_cx_buf);
    } else {
        fprintf(stderr, "  %s: counterexample at iter %d: %s\n",
                kai_check_current_desc ? kai_check_current_desc : "(unnamed)",
                iter_at, kai_check_cx_buf);
    }
    kai_check_has_orig_cx = 0;
}

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

/* Issue #453: default Stdin.read_bytes handler. Reuses the flat
 * prelude helper which returns a String (possibly shorter than `n`
 * on EOF). No Result wrapper — the LSP framing use case treats a
 * short read as end-of-stream. */
static KaiValue *kai_default_stdin_read_bytes(void *self, KaiValue *n, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_prelude_read_bytes(n));
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

/* m7a #7: default File handlers. Each reuses the prelude helper —
 * which already produces `Result[T, String]` shapes per Doc B
 * §`File` error model — but routes the call through the Phase R1
 * reactor's file-pool worker (issue #611) so the blocking `fopen`
 * / `fread` / `fwrite` syscall runs off the scheduler thread.
 * Other fibers therefore make forward progress while one is mid
 * file op.
 *
 * Each `_arg` struct lives on the calling fiber's stack for the
 * lifetime of the work; the `_thunk` runs on a pool worker thread
 * and returns the KaiValue * the prelude produced. */
typedef struct {
    KaiValue *path;
} KaiFileReadFileArg;
static KaiValue *_kai_file_read_file_thunk(void *arg) {
    KaiFileReadFileArg *a = (KaiFileReadFileArg *) arg;
    return kai_prelude_read_file(a->path);
}
static KaiValue *kai_default_file_read_file(void *self, KaiValue *path, KaiCont *k) {
    (void) self;
    kai_reactor_init();
    KaiFileReadFileArg a = { path };
    KaiValue *r = kai_reactor_run_in_pool(_kai_file_read_file_thunk, &a);
    return kai_cont_resume(k, r);
}

typedef struct {
    KaiValue *path;
    KaiValue *contents;
} KaiFileWriteFileArg;
static KaiValue *_kai_file_write_file_thunk(void *arg) {
    KaiFileWriteFileArg *a = (KaiFileWriteFileArg *) arg;
    return kai_prelude_write_file(a->path, a->contents);
}
static KaiValue *kai_default_file_write_file(void *self, KaiValue *path,
                                              KaiValue *contents, KaiCont *k) {
    (void) self;
    kai_reactor_init();
    KaiFileWriteFileArg a = { path, contents };
    KaiValue *r = kai_reactor_run_in_pool(_kai_file_write_file_thunk, &a);
    return kai_cont_resume(k, r);
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

/* Issue #257: Ref[T] ops in the default Mutable handler. Each
 * trampolines to its prelude helper and resumes with the result,
 * mirroring the array_* clauses above. */
static KaiValue *kai_default_mutable_ref_make(void *self, KaiValue *init, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_prelude_ref_make(init));
}

static KaiValue *kai_default_mutable_ref_get(void *self, KaiValue *r, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_prelude_ref_get(r));
}

static KaiValue *kai_default_mutable_ref_set(void *self, KaiValue *r,
                                              KaiValue *v, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_prelude_ref_set(r, v));
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
 *   sleep_ns(ns)     -> Unit     via the Phase R1 reactor timer wheel
 *
 * Issue #611 — Phase R1: sleep_ns parks the calling fiber on the
 * reactor's timer wheel (sorted by CLOCK_MONOTONIC deadline) and
 * yields. The scheduler's poll() loop blocks until the next
 * deadline fires (or another wake source arrives) and promotes
 * the sleeper back to READY. A previous EINTR-based busy loop
 * around nanosleep(2) blocked the OS thread; the new path leaves
 * the rest of the scheduler free to run other fibers while one is
 * asleep.
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
        kai_reactor_init();
        uint64_t deadline = kai_reactor_now_ns() + (uint64_t) ns_v;
        kai_reactor_park_timer(kai_current_fiber(), deadline);
        /* Resumed by the timer-wheel drain. Cancel delivered while
         * the fiber is parked still arrives at the next yield-point
         * hook after we resume. */
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
 *   - All ops are blocking: `Process.wait` parks the OS thread
 *     inside `waitpid` rather than the calling fiber. The m8.x
 *     cooperative scheduler (landed v0.4.0) provides the suspend /
 *     resume primitives, but reactor-driven cancellation-aware
 *     waiting (`wait_or_kill`) still needs a SIGCHLD-aware reactor
 *     plug that registers the pid and wakes the fiber when the
 *     child terminates. Tier 2 follow-up tracked in
 *     docs/fibers-honesty-targets.md §Reactor.
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
 * runtime contract with `builtin_exit_decl`. Issue #440 Phase 2:
 * the typer-declared shape is `Exited(Int)` / `Signaled(Int)`, so
 * stage 2 emits typed-slot match readers (`slots[0].i64`). The
 * runtime constructors must therefore mint cells with `slot_mask`
 * = KAI_VAR_SLOT_INT on slot 0 (= 1) and store the raw scalar
 * directly. The legacy boxed path would mismatch the emitted
 * match read and silently corrupt every Process.wait dispatch. */
static KaiValue *_kai_process_make_exit_exited(int code) {
    KaiVarSlot s; s.i64 = (int64_t) code;
    return kai_variant_u(0, "Exited", 1, KAI_VAR_SLOT_INT, &s);
}

static KaiValue *_kai_process_make_exit_signaled(int signo) {
    KaiVarSlot s; s.i64 = (int64_t) signo;
    return kai_variant_u(1, "Signaled", 1, KAI_VAR_SLOT_INT, &s);
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
 * v1 (see the divergence note in builtin_process_decl).
 *
 * Issue #611 — install the SIGCHLD handler before fork() so that
 * a fast-exiting child cannot deliver SIGCHLD to the kernel's
 * default disposition (zombie reaper only) before the reactor is
 * armed. Idempotent. */
static KaiValue *kai_default_process_start(void *self, KaiValue *cmd, KaiValue *args, KaiCont *k) {
    (void) self;
    if (!cmd || cmd->tag != KAI_STR) {
        fputs("kai: Process.start: cmd must be a String\n", stderr);
        exit(1);
    }
    kai_reactor_init();
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

/* wait(c) -> Result[Exit, String]. Issue #611 Phase R1: the wait
 * parks the calling fiber on the reactor's pid waiter map and the
 * SIGCHLD self-pipe drives the wake. WIFEXITED → Exited(status);
 * WIFSIGNALED → Signaled(signo). The reactor's waitpid drain uses
 * `-1` to harvest any pending child, so a SIGCHLD that fires while
 * the parking fiber is mid-park is handled by the drain helper at
 * the next reactor wait (or immediately if the byte was already
 * pending in the self-pipe). */
static KaiValue *kai_default_process_wait(void *self, KaiValue *child, KaiCont *k) {
    (void) self;
    int pid = _kai_process_record_pid(child);
    if (pid <= 0) {
        KaiValue *m = kai_str("wait: invalid Child");
        KaiValue *err = kai_variant(0, "Err", 1, &m);
        return kai_cont_resume(k, err);
    }
    kai_reactor_init();
    /* Race: the child may have terminated before we registered the
     * waiter. Try a non-blocking waitpid first; if it succeeds we
     * report the status without parking. */
    int status = 0;
    pid_t rc = waitpid((pid_t) pid, &status, WNOHANG);
    if (rc == 0) {
        /* Child still running; park on the pid map and let the
         * SIGCHLD drain wake us with the status. */
        KaiFiber *me = kai_current_fiber();
        kai_reactor_park_pid(me, pid);
        status = me->reactor_wait_status;
        me->reactor_wait_pid    = 0;
        me->reactor_wait_status = 0;
    } else if (rc < 0) {
        return _kai_process_err(k, errno);
    }
    KaiValue *exit_v;
    if (WIFEXITED(status)) {
        exit_v = _kai_process_make_exit_exited(WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        exit_v = _kai_process_make_exit_signaled(WTERMSIG(status));
    } else {
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

/* =================================================================
 * Phase R1 reactor — issue #611
 * =================================================================
 *
 * Single-threaded readiness reactor wired into the cooperative
 * scheduler. Three surfaces park the calling fiber instead of the
 * OS thread:
 *   - Spawn.sleep(ns)        — timer wheel keyed on CLOCK_MONOTONIC.
 *   - File defaults          — thread-pool offload (4 workers).
 *   - Process.wait(child)    — SIGCHLD self-pipe + pid waiter map.
 *
 * Wait primitive is `poll()` on two self-pipes (one for SIGCHLD,
 * one for file-pool completions) plus a deadline-derived timeout.
 * The kqueue/epoll path the parent issue (#474) sketched is queued
 * for R2 when TCP sockets need readiness notifications; R1's three
 * surfaces never wait directly on regular FDs, so the simpler
 * `poll()` shape covers them portably without per-platform
 * branching.
 *
 * Sockets (`NetTcp`) intentionally stay on the blocking syscall
 * path until R2 ships in Orongo together with the Cancel redesign.
 * The `NetTcp` runtime note in this file is left untouched and the
 * `docs/effects-stdlib.md` sidebar for the surface continues to
 * advertise the blocking shape.
 */

/* `kai_sched_unpark` is forward-declared earlier (right after the
 * KaiFiber typedef) so the reactor drain helpers below can wake
 * promoted fibers without needing a second prototype. */

/* Self-pipe halves. The SIGCHLD handler writes one byte to
 * kai_reactor_sigchld_pipe[1] from signal context (write(2) is
 * async-signal-safe per POSIX). The reactor poll watches the read
 * half. The file-pool worker threads write one byte to
 * kai_reactor_filepool_pipe[1] on completion to wake the main
 * thread from poll(). Both pipes are O_NONBLOCK so neither writer
 * ever blocks; the reactor drains them with read() in a loop. */
static int kai_reactor_sigchld_pipe[2]  = { -1, -1 };
static int kai_reactor_filepool_pipe[2] = { -1, -1 };

/* Sorted timer-wheel head (intrusive list of parked fibers chained
 * through f->reactor_next, ordered by ascending deadline). Insertion
 * is O(n); for v1 with handfuls of concurrent sleepers this stays
 * well under the noise floor of poll() itself. A heap is queued for
 * Orongo if the wheel ever shows up on a profile. */
static KaiFiber *kai_reactor_timer_head = NULL;

/* Process-wait map and file-pool waiter list. Both are intrusive
 * single-linked through f->reactor_next, so a fiber can sit on at
 * most one reactor structure at a time (asserted by the parking
 * call sites — a fiber awaiting a pid cannot simultaneously sleep
 * or sit on a file-pool completion). */
static KaiFiber *kai_reactor_pid_waiters     = NULL;
static KaiFiber *kai_reactor_filepool_waiters = NULL;

/* Aggregate count of fibers parked on any reactor structure.
 * kai_sched_park reads this to decide between "no one can wake us
 * up — deadlock" (count == 0) and "block on the reactor until a
 * timer/SIGCHLD/file-pool event arrives" (count > 0). */
static int kai_reactor_parked_count = 0;

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

static pthread_mutex_t  kai_filepool_mu    = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   kai_filepool_cv    = PTHREAD_COND_INITIALIZER;
static KaiFilepoolItem *kai_filepool_q_head = NULL;
static KaiFilepoolItem *kai_filepool_q_tail = NULL;
static int              kai_filepool_started = 0;
#define KAI_FILEPOOL_WORKERS 4
static pthread_t        kai_filepool_threads[KAI_FILEPOOL_WORKERS];

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
        kai_sched_unpark(f);
        woken++;
    }
    return woken;
}

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
        KaiFiber **link = &kai_reactor_pid_waiters;
        while (*link) {
            if ((*link)->reactor_wait_pid == (int) pid) {
                KaiFiber *f = *link;
                *link = f->reactor_next;
                f->reactor_next = NULL;
                f->reactor_wait_status = status;
                /* Leave reactor_wait_pid intact so the wait op
                 * can confirm it matches on resume; clear elsewhere. */
                kai_reactor_parked_count--;
                kai_sched_unpark(f);
                woken++;
                break;
            }
            link = &(*link)->reactor_next;
        }
        /* If no fiber was parked on this pid the exit status is
         * lost. v1 contract: callers always spawn-then-wait. A
         * future "wait_any" or detached spawn would need a small
         * pending-exits buffer here; not on the R1 critical path. */
    }
    return woken;
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
            kai_sched_unpark(f);
            woken++;
        } else {
            link = &(*link)->reactor_next;
        }
    }
    return woken;
}

/* File-pool worker loop. Pops items off the FIFO queue, runs the
 * work function on the worker thread (so the blocking syscall stays
 * off the scheduler thread), publishes the result, and writes one
 * byte to the completion pipe to wake the scheduler. */
static void *kai_filepool_worker(void *arg) {
    (void) arg;
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
        if (!item->work) return NULL;

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

/* Lazy initialisation of the reactor on first use. Idempotent;
 * safe to call from every parking site. Installs SIGCHLD additively
 * (only if no existing handler is registered) so the stack-guard
 * SIGSEGV path and any future signal users continue to operate.
 *
 * The file-pool workers (4 OS threads) are deferred to the first
 * actual file op via kai_reactor_init_filepool — sleep-only and
 * process-only workloads should not pay the pthread_create cost. */
static void kai_reactor_init_filepool(void);
static void kai_reactor_init(void) {
    static int initialized = 0;
    if (initialized) return;
    initialized = 1;

    /* Self-pipes for the two wake sources. O_NONBLOCK so the
     * SIGCHLD handler and worker threads never block; O_CLOEXEC
     * so a forked child does not inherit them. */
    if (pipe(kai_reactor_sigchld_pipe) != 0 ||
        pipe(kai_reactor_filepool_pipe) != 0) {
        fprintf(stderr, "kai: reactor pipe() failed: %s\n", strerror(errno));
        exit(1);
    }
    for (int i = 0; i < 2; i++) {
        int fds[2][2] = {
            { kai_reactor_sigchld_pipe[0],  kai_reactor_sigchld_pipe[1]  },
            { kai_reactor_filepool_pipe[0], kai_reactor_filepool_pipe[1] },
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
            fprintf(stderr,
                "kai: reactor cannot install SIGCHLD — slot already taken\n");
            exit(1);
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
}

/* Spin up the 4-worker file pool on the first file op. SIGCHLD is
 * blocked in every worker thread's mask so the signal always lands
 * on the scheduler (main) thread, which is the one draining the
 * self-pipe in kai_reactor_wait. The pool runs detached for the
 * lifetime of the process; the OS reclaims the threads on exit. */
static void kai_reactor_init_filepool(void) {
    if (kai_filepool_started) return;
    kai_filepool_started = 1;
    sigset_t block_set, prev_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &block_set, &prev_set);
    for (int i = 0; i < KAI_FILEPOOL_WORKERS; i++) {
        if (pthread_create(&kai_filepool_threads[i], NULL,
                           kai_filepool_worker, NULL) != 0) {
            fprintf(stderr, "kai: reactor pthread_create failed: %s\n",
                    strerror(errno));
            exit(1);
        }
        pthread_detach(kai_filepool_threads[i]);
    }
    pthread_sigmask(SIG_SETMASK, &prev_set, NULL);
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
static void kai_reactor_park_timer(KaiFiber *f, uint64_t deadline_ns) {
    f->reactor_deadline_ns = deadline_ns;
    kai_reactor_timer_insert(f);
    kai_reactor_parked_count++;
    kai_sched_park();
}

static void kai_reactor_park_pid(KaiFiber *f, int pid) {
    f->reactor_wait_pid    = pid;
    f->reactor_wait_status = 0;
    /* Push onto the head; pid lookup walks the list so order is
     * irrelevant. The drain helper splices the matching node out. */
    f->reactor_next = kai_reactor_pid_waiters;
    kai_reactor_pid_waiters = f;
    kai_reactor_parked_count++;
    kai_sched_park();
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
    /* Queue order is irrelevant for the waiter list; insert at head. */
    me->reactor_next = kai_reactor_filepool_waiters;
    kai_reactor_filepool_waiters = me;
    kai_reactor_parked_count++;

    kai_filepool_submit(&item);
    kai_sched_park();

    /* On resume the drain helper has spliced us out of
     * kai_reactor_filepool_waiters. The result slot is set by the
     * worker prior to the pipe write. */
    KaiValue *r = item.result;
    me->reactor_data = NULL;
    return r ? r : kai_unit();
}

/* Block in poll() until either the SIGCHLD pipe or the file-pool
 * completion pipe fires, or the next timer deadline arrives.
 * Promotes every newly-ready fiber to the run queue. Called by
 * kai_sched_park when the ready queue is empty but reactor waiters
 * exist — the dispatch loop's substitute for a dedicated event
 * loop thread. */
static void kai_reactor_wait(void) {
    /* Compute the timeout in ms (poll's resolution). A negative
     * timeout is "wait forever"; a zero timeout polls. */
    int timeout_ms = -1;
    if (kai_reactor_timer_head) {
        uint64_t now = kai_reactor_now_ns();
        uint64_t dl  = kai_reactor_timer_head->reactor_deadline_ns;
        if (dl <= now) {
            timeout_ms = 0;
        } else {
            uint64_t diff_ns = dl - now;
            uint64_t ms = (diff_ns + 999999ULL) / 1000000ULL;  /* ceil */
            if (ms > (uint64_t) INT_MAX) ms = (uint64_t) INT_MAX;
            timeout_ms = (int) ms;
        }
    }

    struct pollfd pfds[2];
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
    int rc = poll(pfds, (nfds_t) nfds, timeout_ms);
    if (rc < 0 && errno != EINTR) {
        fprintf(stderr, "kai: reactor poll() failed: %s\n", strerror(errno));
        exit(1);
    }

    /* Drain in a fixed order. Even on EINTR (rc < 0) the timer
     * wheel must be drained because a stray signal could have
     * coincided with a deadline expiry. */
    uint64_t now = kai_reactor_now_ns();
    kai_reactor_timer_drain(now);
    if (rc > 0) {
        for (int i = 0; i < nfds; i++) {
            if (pfds[i].revents & POLLIN) {
                if (pfds[i].fd == kai_reactor_sigchld_pipe[0]) {
                    kai_reactor_sigchld_drain();
                } else if (pfds[i].fd == kai_reactor_filepool_pipe[0]) {
                    kai_reactor_filepool_drain();
                }
            }
        }
    }
    /* A SIGCHLD delivered before we entered poll() may not appear
     * in revents (the signal handler ran but the byte arrived
     * after the kernel snapshot). Always attempt a non-blocking
     * waitpid drain too so children that exited during the
     * micro-window before poll() do not strand their fibers. */
    if (kai_reactor_pid_waiters) {
        kai_reactor_sigchld_drain();
    }
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
 * list (awaiter chain, mailbox waiter list, or a reactor structure)
 * before calling park — otherwise no fiber can ever unpark it.
 *
 * Issue #611 — Phase R1 reactor integration: when the ready queue
 * is empty but at least one fiber is parked on the reactor (timer
 * wheel, pid waiter, file-pool waiter), block in `kai_reactor_wait`
 * until an event promotes someone, then redequeue. Deadlock is now
 * `ready queue empty AND reactor empty` — only that combination
 * means no path to forward progress. */
static void kai_sched_park(void) {
    KaiFiber *current = kai_active_fiber;
    /* Mark the caller PARKED up front so the reactor drain helpers
     * (which may run inside kai_reactor_wait below) can promote us
     * back to READY via kai_sched_unpark. The unpark path bails out
     * when state != PARKED, so a fiber whose state is still RUNNING
     * when its deadline fires would be lost. */
    current->state = KAI_FIBER_PARKED;
    kai_parked_count++;

    KaiFiber *next = kai_sched_dequeue();
    while (!next) {
        if (kai_reactor_parked_count > 0) {
            kai_reactor_wait();
            next = kai_sched_dequeue();
            continue;
        }
        fprintf(stderr,
            "kai: deadlock — fiber parked with empty run queue (%d parked total)\n",
            kai_parked_count);
        exit(1);
    }
    if (next == current) {
        /* The reactor wake promoted us before we picked anyone else.
         * Skip the swapcontext (we are still on our own stack) and
         * unwind the parked accounting we just bumped. */
        current->state = KAI_FIBER_RUNNING;
        kai_parked_count--;
        return;
    }
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
 * kai_apply consumes (#298), so the trampoline incref's the thunk
 * before each invocation to keep f->thunk's lifetime independent of
 * the call. kai_free_value's KAI_FIBER branch decrefs both thunk and
 * result when f's RC drops. */
static void kai_fiber_trampoline(void) {
    KaiFiber *self = kai_active_fiber;
    /* First entry follows a setcontext from another fiber's
     * trampoline tail or a swap from yield/park. Drain any pending
     * struct free left behind by the previous fiber's
     * `kai_decref(self->value)` before we touch our own state. */
    kai_drain_pending_free();
    if (setjmp(self->cancel_pad) == 0) {
        self->cancel_pad_set = 1;
        /* kai_apply consumes (#298): give it its own ref, keep f->thunk. */
        self->result = kai_apply(kai_incref(self->thunk), 0, NULL);
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
     * ready (which may be one of them, or main, or a sibling).
     *
     * Issue #611 — if the queue is empty here but reactor waiters
     * remain (timer wheel, pid map, file-pool list), block in
     * kai_reactor_wait until a wake event promotes someone before
     * declaring deadlock. */
    KaiFiber *next = kai_sched_dequeue();
    while (!next) {
        if (kai_reactor_parked_count > 0) {
            kai_reactor_wait();
            next = kai_sched_dequeue();
            continue;
        }
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
