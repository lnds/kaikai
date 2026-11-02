/* Fragment of runtime.h; include runtime.h, never this file. */
/* ---------- cross-thread ledger reduction ----------
 *
 * The counters above are Class A: per-thread TLS ledgers, so any one
 * thread's copy is a partial total. Each scheduler thread registers a
 * block of pointers to its TLS ledger on start; a thread that exits
 * before the report folds its values into the process-wide accumulator
 * (its TLS storage dies with it) and leaves the walk; the exit report
 * reduces the fold plus every still-registered block. The hot
 * incref/decref path never synchronizes. A block whose owner is still
 * running is read without synchronization — tolerable in a diagnostic
 * report, never in program state. */
typedef struct {
    int64_t alloc_total, free_total, live_peak;
    int64_t alloc_by_tag[16];
    int64_t arena_alloc_total, arena_free_total;
    int64_t mbox_alloc_total, mbox_free_total;
    int64_t reuse_total, vec_inplace_total, vec_cow_total;
    int64_t reuse_free_total, tok_unique, tok_null_shared, tok_null_mismatch;
#ifdef KAI_TRACE_RC
    int64_t free_by_tag[16];
#endif
} KaiRcLedgerSum;

typedef struct KaiRcLedgerBlock {
    const int64_t *alloc_total, *free_total, *live_peak;
    const int64_t *alloc_by_tag;
    const int64_t *arena_alloc_total, *arena_free_total;
    const int64_t *mbox_alloc_total, *mbox_free_total;
    const int64_t *reuse_total, *reuse_emitted, *vec_inplace_total, *vec_cow_total;
    const int64_t *reuse_free_total, *tok_unique, *tok_null_shared, *tok_null_mismatch;
#ifdef KAI_TRACE_RC
    const int64_t *free_by_tag;
#endif
    struct KaiRcLedgerBlock *next;
} KaiRcLedgerBlock;

#if defined(KAI_SEPARATE_COMPILATION)
extern KaiRcLedgerBlock *kai_rc_ledger_blocks;
extern KaiRcLedgerSum kai_rc_ledger_folded;
extern pthread_mutex_t kai_rc_ledger_mu;
#  if defined(KAI_RUNTIME_OWNER)
KaiRcLedgerBlock *kai_rc_ledger_blocks = NULL;
KaiRcLedgerSum kai_rc_ledger_folded;
pthread_mutex_t kai_rc_ledger_mu = PTHREAD_MUTEX_INITIALIZER;
#  endif
#else
static KaiRcLedgerBlock *kai_rc_ledger_blocks = NULL;
static KaiRcLedgerSum kai_rc_ledger_folded;
static pthread_mutex_t kai_rc_ledger_mu = PTHREAD_MUTEX_INITIALIZER;
#endif
KAI_RT_COUNTER(KaiRcLedgerBlock *kai_rc_ledger_block, NULL);
KAI_RT_COUNTER(int kai_rc_ledger_done, 0);

/* Pointers are captured on the owning thread — that is what binds each
 * field to THIS thread's TLS instance. */
static void kai_rc_ledger_fill(KaiRcLedgerBlock *b) {
    b->alloc_total       = &kai_rc_hot.alloc_total;
    b->free_total        = &kai_rc_hot.free_total;
    b->live_peak         = &kai_rc_hot.live_peak;
    b->alloc_by_tag      = kai_rc_hot.alloc_by_tag;
    b->arena_alloc_total = &kai_arena_alloc_total;
    b->arena_free_total  = &kai_arena_free_total;
    b->mbox_alloc_total  = &kai_mbox_alloc_total;
    b->mbox_free_total   = &kai_mbox_free_total;
    b->reuse_total       = &kai_rc_hot.reuse_total;
    b->reuse_emitted     = &kai_rc_reuse_total;
    b->vec_inplace_total = &kai_vec_inplace_total;
    b->vec_cow_total     = &kai_vec_cow_total;
    b->reuse_free_total  = &kai_rc_reuse_free_total;
    b->tok_unique        = &kai_rc_tok_unique;
    b->tok_null_shared   = &kai_rc_tok_null_shared;
    b->tok_null_mismatch = &kai_rc_tok_null_mismatch;
#ifdef KAI_TRACE_RC
    b->free_by_tag       = kai_rc_free_by_tag;
#endif
}

static void kai_rc_ledger_add(KaiRcLedgerSum *s, const KaiRcLedgerBlock *b) {
    s->alloc_total       += *b->alloc_total;
    s->free_total        += *b->free_total;
    s->live_peak         += *b->live_peak;
    for (int i = 0; i < 16; i++) s->alloc_by_tag[i] += b->alloc_by_tag[i];
    s->arena_alloc_total += *b->arena_alloc_total;
    s->arena_free_total  += *b->arena_free_total;
    s->mbox_alloc_total  += *b->mbox_alloc_total;
    s->mbox_free_total   += *b->mbox_free_total;
    s->reuse_total       += *b->reuse_total + *b->reuse_emitted;
    s->vec_inplace_total += *b->vec_inplace_total;
    s->vec_cow_total     += *b->vec_cow_total;
    s->reuse_free_total  += *b->reuse_free_total;
    s->tok_unique        += *b->tok_unique;
    s->tok_null_shared   += *b->tok_null_shared;
    s->tok_null_mismatch += *b->tok_null_mismatch;
#ifdef KAI_TRACE_RC
    for (int i = 0; i < 16; i++) s->free_by_tag[i] += b->free_by_tag[i];
#endif
}

static __attribute__((noinline)) void kai_rc_ledger_register(void) {
    if (kai_rc_ledger_block || kai_rc_ledger_done) return;
    KaiRcLedgerBlock *b = (KaiRcLedgerBlock *) calloc(1, sizeof *b);
    if (!b) return;
    kai_rc_ledger_fill(b);
    pthread_mutex_lock(&kai_rc_ledger_mu);
    b->next = kai_rc_ledger_blocks;
    kai_rc_ledger_blocks = b;
    pthread_mutex_unlock(&kai_rc_ledger_mu);
    kai_rc_ledger_block = b;
}

/* Thread teardown: the ledger folds into the accumulator and the block
 * leaves the walk, so an exited thread's counts survive it. */
static __attribute__((noinline)) void kai_rc_ledger_fold(void) {
    if (kai_rc_ledger_done) return;
    kai_rc_ledger_done = 1;
    KaiRcLedgerBlock own;
    kai_rc_ledger_fill(&own);
    KaiRcLedgerBlock *b = kai_rc_ledger_block;
    pthread_mutex_lock(&kai_rc_ledger_mu);
    kai_rc_ledger_add(&kai_rc_ledger_folded, &own);
    if (b) {
        KaiRcLedgerBlock **p = &kai_rc_ledger_blocks;
        while (*p && *p != b) p = &(*p)->next;
        if (*p) *p = b->next;
    }
    pthread_mutex_unlock(&kai_rc_ledger_mu);
    free(b);
    kai_rc_ledger_block = NULL;
}

static __attribute__((noinline)) void kai_rc_ledger_sum(KaiRcLedgerSum *s) {
    kai_rc_settle();
    kai_rc_ledger_register();
    pthread_mutex_lock(&kai_rc_ledger_mu);
    *s = kai_rc_ledger_folded;
    for (KaiRcLedgerBlock *b = kai_rc_ledger_blocks; b; b = b->next) {
        kai_rc_ledger_add(s, b);
    }
    pthread_mutex_unlock(&kai_rc_ledger_mu);
}

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
        case KAI_VEC:     return "vec";
        case KAI_REF:     return "ref";
        case KAI_BYTE:      return "Byte";
        case KAI_INT32:     return "Int32";
        case KAI_UINT32:    return "UInt32";
        case KAI_UINT64:    return "UInt64";
        case KAI_INT128:    return "Int128";
        case KAI_RANGE:     return "range";
        case KAI_CONT:      return "continuation";
        case KAI_SEQ:       return "seq";
        default:          return "?";
    }
}

typedef struct { int unstarted, started; } KaiExitFibers;
static KaiExitFibers kai_rc_settle_exit_fibers(void);

static void kai_rc_report(void) {
    if (!getenv("KAI_TRACE_RC")) return;
    KaiExitFibers ef = kai_rc_settle_exit_fibers();
    KaiRcLedgerSum s;
    kai_rc_ledger_sum(&s);
    int64_t leaked = s.alloc_total - s.free_total;
    kai_eprintf(
        "[KAI_TRACE_RC] alloc_total=%lld free_total=%lld leaked=%lld live_peak=%lld\n",
        (long long) s.alloc_total,
        (long long) s.free_total,
        (long long) leaked,
        (long long) s.live_peak);
    if (ef.unstarted > 0)
        kai_eprintf("[KAI_TRACE_RC]   fibers_unstarted_at_exit=%d\n", ef.unstarted);
    if (ef.started > 0)
        kai_eprintf("[KAI_TRACE_RC]   fibers_started_at_exit=%d\n", ef.started);
    int64_t chosen = atomic_load(&kai_chosen_exit_plus1);
    if (chosen > 0) kai_eprintf("[KAI_TRACE_RC]   exit_code=%lld\n", (long long) (chosen - 1));
    else            kai_eprintf("[KAI_TRACE_RC]   exit_code=abort\n");
    for (int i = 0; i < 16; i++) {
        if (s.alloc_by_tag[i] > 0) {
            kai_eprintf("[KAI_TRACE_RC]   tag %-7s allocs=%lld\n",
                    kai_rc_tag_name(i),
                    (long long) s.alloc_by_tag[i]);
        }
    }
    /* issue #118 — Perceus reuse-in-place counter. */
    if (s.reuse_total > 0) {
        kai_eprintf("[KAI_TRACE_RC]   reuse_in_place=%lld\n",
                (long long) s.reuse_total);
    }
    if (s.vec_inplace_total > 0 || s.vec_cow_total > 0) {
        kai_eprintf("[KAI_TRACE_RC]   vec_inplace=%lld vec_cow=%lld\n",
                (long long) s.vec_inplace_total,
                (long long) s.vec_cow_total);
    }
    /* #2 parity probe — tokens captured but freed (could not donate in
     * frame). Upper bound on interprocedural-token-pass recovery. */
    if (s.reuse_free_total > 0) {
        kai_eprintf("[KAI_TRACE_RC]   reuse_freed=%lld\n",
                (long long) s.reuse_free_total);
    }
    /* #2 parity probe — token-drop outcome split. */
    if (s.tok_unique > 0 || s.tok_null_shared > 0 ||
        s.tok_null_mismatch > 0) {
        kai_eprintf(
            "[KAI_TRACE_RC]   tok_unique=%lld tok_null_shared=%lld tok_null_mismatch=%lld\n",
            (long long) s.tok_unique,
            (long long) s.tok_null_shared,
            (long long) s.tok_null_mismatch);
    }
    /* Phase 1.B.1 — RC traffic (rc-touching incref/decref calls). */
    kai_eprintf("[KAI_TRACE_RC]   incref_total=%lld decref_total=%lld\n",
            (long long) kai_rc_incref_total,
            (long long) kai_rc_decref_total);
    /* issue #120 — region arena lifecycle. arena_live != 0 at exit
     * flags a wrong-codegen silent leak (a non-region value mistakenly
     * arena-allocated, which never frees and which ASAN cannot see). */
    if (s.arena_alloc_total > 0 || s.arena_free_total > 0) {
        kai_eprintf(
            "[KAI_TRACE_RC]   arena_alloc=%lld arena_free=%lld arena_live=%lld\n",
            (long long) s.arena_alloc_total,
            (long long) s.arena_free_total,
            (long long) (s.arena_alloc_total - s.arena_free_total));
    }
    if (s.mbox_alloc_total > 0) {
        kai_eprintf(
            "[KAI_TRACE_RC]   mailbox_alloc=%lld mailbox_free=%lld mailbox_live=%lld\n",
            (long long) s.mbox_alloc_total,
            (long long) s.mbox_free_total,
            (long long) (s.mbox_alloc_total - s.mbox_free_total));
    }
}

KAI_RT_COUNTER(int kai_rc_registered, 0);
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
static KAI_TLS KaiRcHistoryEntry kai_rc_history[KAI_RC_HISTORY_CAP];
static KAI_TLS uint64_t kai_rc_history_count = 0;
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
    (void) kai_rc_settle_exit_fibers();
    KaiRcLedgerSum s;
    kai_rc_ledger_sum(&s);
    int64_t leaked = s.alloc_total - s.free_total;
    kai_eprintf(
        "[KAI_TRACE_RC] STRICT alloc_total=%lld free_total=%lld leaked=%lld live_peak=%lld\n",
        (long long) s.alloc_total,
        (long long) s.free_total,
        (long long) leaked,
        (long long) s.live_peak);
    for (int i = 0; i < 16; i++) {
        int64_t a = s.alloc_by_tag[i];
        int64_t f = s.free_by_tag[i];
        if (a == 0 && f == 0) continue;
        int64_t live = a - f;
        const char *flag =
            (live != 0)              ? " LEAK"     :
            (f > a)                  ? " DOUBLE"   : "";
        kai_eprintf(
            "[KAI_TRACE_RC] tag=%-7s allocs=%lld frees=%lld live=%lld%s\n",
            kai_rc_tag_name(i),
            (long long) a, (long long) f, (long long) live, flag);
    }
    /* issue #120 — region arena lifecycle under strict tracing. A
     * non-zero arena_live is the ONLY visible signal of a wrong-codegen
     * silent leak: arena memory is reclaimed in bulk without a free
     * walk, so ASAN sees no use-after-free and the per-tag table above
     * never moves for arena values. */
    if (s.arena_alloc_total > 0 || s.arena_free_total > 0) {
        int64_t arena_live = s.arena_alloc_total - s.arena_free_total;
        const char *aflag = (arena_live != 0) ? " LEAK"
                          : (s.arena_free_total > s.arena_alloc_total) ? " DOUBLE"
                          : "";
        kai_eprintf(
            "[KAI_TRACE_RC] arena   allocs=%lld frees=%lld live=%lld%s\n",
            (long long) s.arena_alloc_total,
            (long long) s.arena_free_total,
            (long long) arena_live, aflag);
    }
    if (kai_rc_history_enabled() && kai_rc_history_count > 0) {
        uint64_t total = kai_rc_history_count;
        uint64_t shown = total < KAI_RC_HISTORY_CAP ? total : KAI_RC_HISTORY_CAP;
        kai_eprintf(
            "[KAI_TRACE_RC] history total=%llu showing last=%llu\n",
            (unsigned long long) total, (unsigned long long) shown);
        uint64_t start = total > KAI_RC_HISTORY_CAP ? total - KAI_RC_HISTORY_CAP : 0;
        for (uint64_t k = start; k < total; k++) {
            KaiRcHistoryEntry *e = &kai_rc_history[k % KAI_RC_HISTORY_CAP];
            kai_eprintf("[KAI_TRACE_RC] hist %s chunk=%p tag=%s\n",
                kai_rc_op_name(e->op), e->chunk,
                kai_rc_tag_name(e->tag));
        }
    }
}

static KAI_TLS int kai_rc_strict_registered = 0;
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

static KAI_TLS KaiRcSite kai_rc_sites[KAI_RC_SITE_BUCKETS];
static KAI_TLS int       kai_rc_sites_count = 0;
static KAI_TLS int       kai_rc_sites_full  = 0;  /* sticky: a probe overflowed */

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

    kai_eprintf(
        "[KAI_TRACE_RC] top sites by leak (showing %d of %d distinct sites; total_leak=%lld%s)\n",
        top_n < n ? top_n : n, n, (long long) total_leak,
        kai_rc_sites_full ? "; TABLE SATURATED" : "");
    kai_eprintf(
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
        kai_eprintf(
            "[KAI_TRACE_RC] site %2d %p tag=%-7s allocs=%lld frees=%lld leak=%lld leak%%=%.1f share%%=%.1f\n",
            i + 1, s->site, kai_rc_tag_name(s->tag),
            (long long) s->allocs, (long long) s->frees, (long long) live,
            pct, share);
    }
    free(flat);
}

static KAI_TLS int kai_rc_site_registered = 0;
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

#endif /* KAI_TRACE_RC */

#ifndef KAI_TRACE_RC
#define KAI_RC_NOINLINE
#endif

#define KAI_SCOPE_LEAVE(v) (v)

