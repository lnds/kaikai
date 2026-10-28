/* Fragment of runtime.h; include runtime.h, never this file. */

/* A constructor's name and per-slot kind bits live in a process-wide
 * table indexed by tag, not in the node. A tag is recorded the first time
 * it is constructed, on whichever thread gets there first.
 *
 * The table spans the whole tag space (`variant_tag` is a uint16_t), so it
 * never grows or moves. Each field is one atomic word and every store to
 * it writes the same value, so relaxed ordering suffices. Tags are hashed
 * across the span: name and mask share an entry so a tag touches one page. */
#define KAI_VARIANT_TAG_SPACE 65536
#define KAI_SLOTMASK_RECORDED (UINT64_C(1) << 32)
typedef struct {
    _Atomic(const char *) name;
    _Atomic uint64_t      mask;   /* KAI_SLOTMASK_RECORDED | mask once recorded */
} KaiCtorMeta;
#if defined(KAI_SEPARATE_COMPILATION)
extern KaiCtorMeta kai_ctor_meta[KAI_VARIANT_TAG_SPACE];
#  if defined(KAI_RUNTIME_OWNER)
KaiCtorMeta kai_ctor_meta[KAI_VARIANT_TAG_SPACE];
#  endif
#else
static KaiCtorMeta kai_ctor_meta[KAI_VARIANT_TAG_SPACE];
#endif

static inline int kai_variant_tag_in_space(int32_t tag) {
    return (uint32_t) tag < KAI_VARIANT_TAG_SPACE;
}

static void kai_varname_register(int32_t tag, const char *name) {
    if (!kai_variant_tag_in_space(tag) || name == NULL) return;
    if (atomic_load_explicit(&kai_ctor_meta[tag].name, memory_order_relaxed) == NULL) {
        atomic_store_explicit(&kai_ctor_meta[tag].name, name, memory_order_relaxed);
    }
}

static const char *kai_variant_name_of(int32_t tag) {
    if (!kai_variant_tag_in_space(tag)) return "";
    const char *name = atomic_load_explicit(&kai_ctor_meta[tag].name, memory_order_relaxed);
    return name != NULL ? name : "";
}

/* The first mask recorded for a tag wins. The recorded bit and the mask
 * share a word so a thread that sees the tag as recorded sees its mask. */
static void kai_slotmask_register(int32_t tag, uint32_t mask) {
    if (!kai_variant_tag_in_space(tag)) return;
    if (atomic_load_explicit(&kai_ctor_meta[tag].mask, memory_order_relaxed) == 0) {
        atomic_store_explicit(&kai_ctor_meta[tag].mask, KAI_SLOTMASK_RECORDED | mask,
                              memory_order_relaxed);
    }
}

/* A tag never recorded reads as 0: every slot a pointer. */
static uint32_t kai_slot_mask_of(int32_t tag) {
    if (!kai_variant_tag_in_space(tag)) return 0;
    return (uint32_t) atomic_load_explicit(&kai_ctor_meta[tag].mask, memory_order_relaxed);
}

/* ---------- KAI_MAX_HEAP: process heap ceiling (host containment) ----------
 *
 * Caps the live committed heap so a runaway aborts clean instead of dragging
 * the host into an OOM hang (on macOS the RAM compressor masks exhaustion;
 * RLIMIT_AS / `ulimit -v` are no-ops, so there is no OS ceiling at all).
 * Set `KAI_MAX_HEAP` to a byte count or a k/m/g-suffixed size (e.g. `4g`).
 * Unset/empty/unparseable -> no cap: every grow point is one flag test and
 * nothing is accounted.
 *
 * The counter is one process-wide atomic. Slabs, cells and arena chunks stay
 * committed once taken, so they only add. String, array and vec payloads are
 * plain malloc blocks: they add on allocation and subtract on free, by the
 * allocator's own usable size so a block always credits what it charged. */
#if defined(KAI_SEPARATE_COMPILATION)
extern size_t kai_heap_cap;
extern size_t kai_heap_live;
#  if defined(KAI_RUNTIME_OWNER)
size_t kai_heap_cap  = 0;   /* 0 = no cap */
size_t kai_heap_live = 0;
#  endif
#else
static size_t kai_heap_cap  = 0;
static size_t kai_heap_live = 0;
#endif

/* Declared here rather than included: the allocator headers cost more to
 * parse than the whole declaration. */
#if defined(__APPLE__)
extern size_t malloc_size(const void *ptr);
#  define kai_heap_usable(p) malloc_size(p)
#else
extern size_t malloc_usable_size(void *ptr);
#  define kai_heap_usable(p) malloc_usable_size(p)
#endif

#if !defined(KAI_SEPARATE_COMPILATION) || defined(KAI_RUNTIME_OWNER)
__attribute__((constructor)) static void kai_heap_cap_init(void) {
    const char *raw = getenv("KAI_MAX_HEAP");
    if (!raw || !*raw) return;
    char *end = NULL;
    unsigned long long n = strtoull(raw, &end, 10);
    unsigned long long mul = 1;
    if (end && *end) {
        switch (*end) {
            case 'k': case 'K': mul = 1024ULL; break;
            case 'm': case 'M': mul = 1024ULL * 1024ULL; break;
            case 'g': case 'G': mul = 1024ULL * 1024ULL * 1024ULL; break;
            default: mul = 0; break;   /* junk suffix -> no cap */
        }
        if (mul && end[1]) mul = 0;    /* trailing junk after suffix -> no cap */
    }
    if (mul && n > 0 && n <= (unsigned long long) ((size_t) -1) / mul) {
        kai_heap_cap = (size_t) (n * mul);
    }
}
#endif

__attribute__((noinline, cold))
KAI_RT_SHARED void kai_heap_charge_slow(size_t sz) {
    size_t used = __atomic_add_fetch(&kai_heap_live, sz, __ATOMIC_RELAXED);
    if (used > kai_heap_cap) {
        kai_eprintf(
            "kai: heap limit exceeded (KAI_MAX_HEAP=%s, used %zu bytes)\n",
            getenv("KAI_MAX_HEAP"), used - sz);
        kai_exit(1);
    }
}

__attribute__((noinline, cold))
KAI_RT_SHARED void kai_heap_credit_slow(size_t sz) {
    size_t cur = __atomic_load_n(&kai_heap_live, __ATOMIC_RELAXED);
    size_t next;
    do { next = cur > sz ? cur - sz : 0; }
    while (!__atomic_compare_exchange_n(&kai_heap_live, &cur, next, 1,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

/* A grow point that stays committed. */
static inline void kai_heap_charge(size_t sz) {
    if (__builtin_expect(kai_heap_cap != 0, 0)) kai_heap_charge_slow(sz);
}

/* The capped paths stay out of line so the uncapped one is a flag test
 * and a libc call at every payload site. */
__attribute__((noinline, cold))
KAI_RT_SHARED void *kai_heap_malloc_capped(size_t sz) {
    kai_heap_charge_slow(sz);
    void *p = malloc(sz);
    if (!p) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
    size_t usable = kai_heap_usable(p);
    if (usable > sz) __atomic_add_fetch(&kai_heap_live, usable - sz, __ATOMIC_RELAXED);
    return p;
}

__attribute__((noinline, cold))
KAI_RT_SHARED void *kai_heap_realloc_capped(void *ptr, size_t old_sz, size_t new_sz) {
    size_t old_usable = ptr ? kai_heap_usable(ptr) : 0;
    if (new_sz > old_sz) kai_heap_charge_slow(new_sz - old_sz);
    void *p = realloc(ptr, new_sz);
    if (!p) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
    size_t new_usable = kai_heap_usable(p);
    size_t charged = new_sz > old_sz ? old_usable + (new_sz - old_sz) : old_usable;
    if (new_usable > charged) __atomic_add_fetch(&kai_heap_live, new_usable - charged, __ATOMIC_RELAXED);
    else if (new_usable < charged) kai_heap_credit_slow(charged - new_usable);
    return p;
}

/* malloc that charges the payload against the ceiling and folds in the OOM
 * check shared by every value-heap allocation. */
static void *kai_heap_malloc(size_t sz) {
    if (__builtin_expect(kai_heap_cap != 0, 0)) return kai_heap_malloc_capped(sz);
    void *p = malloc(sz);
    if (!p) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
    return p;
}

__attribute__((noinline, cold))
KAI_RT_SHARED void kai_heap_free_capped(void *p) {
    if (p) kai_heap_credit_slow(kai_heap_usable(p));
    free(p);
}

static void kai_heap_free(void *p) {
    if (__builtin_expect(kai_heap_cap != 0, 0)) { kai_heap_free_capped(p); return; }
    free(p);
}

static void *kai_heap_realloc(void *ptr, size_t old_sz, size_t new_sz) {
    if (__builtin_expect(kai_heap_cap != 0, 0)) return kai_heap_realloc_capped(ptr, old_sz, new_sz);
    void *p = realloc(ptr, new_sz);
    if (!p) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
    return p;
}

#ifdef KAI_TRACE_RC
static void kai_slab_register_teardown(void) {}
static KaiValue *kai_alloc_traced(KaiTag tag, void *site) {
#else
/* Issue #118 follow-up #3 — fixed-size cell free-list (Koka/mimalloc
 * model). Every KaiValue is the same size, so a freed cell can host the
 * next allocation without a malloc/free round-trip. This is the
 * size-keyed reuse Koka gets implicitly from its heap: the functional
 * rebuild's discarded spine cells are recycled by the very next
 * constructor of the same size, WITHOUT any compiler-level token
 * threading. Bounded so a transient spike does not pin memory forever.
 *
 * Disabled under KAI_TRACE_RC: that mode poisons freed
 * cells and relies on exact malloc/free pairing for leak attribution, so
 * recycling would corrupt its bookkeeping. The free-list is a pure
 * production-path allocator optimisation — RC semantics and emitted code
 * are byte-identical; only the malloc/free traffic changes. */
#if !defined(KAI_TRACE_RC) && !defined(KAI_NO_CELL_POOL)
#define KAI_CELL_POOL_ACTIVE 1
/* Cap sized for the functional-rebuild working set. The rb-tree churns
 * a ~1M-cell live set with a deep free/realloc cycle; a cap below the
 * peak free-list depth spills to libc and the wall destabilises around
 * the 10× target (measured: 262Ki oscillates 9.9–10.3× by run, 1Mi
 * holds a stable 9.8–9.9×). 1Mi entries (8 MB of pointers, lazy .bss)
 * keep the recycle hit-rate high; beyond it we fall through to
 * malloc/free. */
#define KAI_CELL_POOL_CAP 1048576
/* The free-list pools are multi-MB per thread. As a `_Thread_local` ARRAY
 * the whole table joins the TLS image, which the thread runtime
 * zero-materialises (faulting in every page) at thread creation — so the
 * full 8/72/72 MiB became resident per thread even for a program that
 * never allocates (issue #1212/#1213: hello-world 2 MiB → 162 MiB). A
 * `_Thread_local` POINTER instead is 8 bytes of TLS; the backing store is
 * a single `calloc` on first push (kai_cell_pool_ensure), which the OS
 * backs with lazy zero pages — RSS grows only with entries actually
 * written, restoring the pre-M:N footprint. Separate-compilation sharing
 * is unchanged: every TU sees the `extern` pointer; the owner TU defines
 * it; the index counter moves with the pointer. */
#if defined(KAI_SEPARATE_COMPILATION)
extern KAI_TLS KaiValue **kai_cell_pool;
extern KAI_TLS int kai_cell_pool_n;
#  if defined(KAI_RUNTIME_OWNER)
KAI_TLS KaiValue **kai_cell_pool = NULL;
KAI_TLS int kai_cell_pool_n = 0;
#  endif
#else
static KAI_TLS KaiValue **kai_cell_pool = NULL;
static KAI_TLS int kai_cell_pool_n = 0;
#endif

/* FAM lane: free-list for whole variant BLOCKS (header + n inline slots
 * in one allocation), keyed by arity. Replaces the cell_pool + slot_pool
 * pair for variants — one push/pop instead of two, one cache line per
 * node instead of a header here and a slots[] array somewhere else.
 * A pooled block already has the right byte size for its arity, so reuse
 * is size-matched by construction (the reuse recognisers only ever
 * rewrite same-arity). */
#define KAI_VAR_BLOCK_POOL_MAXN 255   /* every arity var_n_args can hold */
/* Per-arity free list of whole variant blocks, threaded through the dead
 * blocks themselves: a freed block's first word holds the next one. It has
 * no cap, because a block is slab memory that cannot go back to libc; a
 * capped pool would strand every block past the cap. The allocator
 * rewrites the whole header of a block it hands out. */
#if defined(KAI_SEPARATE_COMPILATION)
extern KAI_TLS KaiValue *kai_var_block_free_head[KAI_VAR_BLOCK_POOL_MAXN + 1];
#  if defined(KAI_RUNTIME_OWNER)
KAI_TLS KaiValue *kai_var_block_free_head[KAI_VAR_BLOCK_POOL_MAXN + 1];
#  endif
#else
static KAI_TLS KaiValue *kai_var_block_free_head[KAI_VAR_BLOCK_POOL_MAXN + 1];
#endif

/* Lazy backing-store materialisation for the free-list pools. Each pool's
 * table (CAP pointers) is calloc'd on first push into it, not at thread
 * startup — the OS backs the calloc with zero pages that only become
 * resident as entries are written, so a program that allocates little
 * pays little (issue #1212/#1213). calloc failure degrades to "no pool"
 * (the caller falls through to libc malloc/free), never a crash. */
static inline int kai_cell_pool_ensure(void) {
    if (!kai_cell_pool) {
        kai_cell_pool = (KaiValue **) calloc(KAI_CELL_POOL_CAP, sizeof(KaiValue *));
    }
    return kai_cell_pool != NULL;
}

/* Free-list pool ops routed through noinline accessors: the pool base and
 * index are thread-local, so their addresses must be materialised and consumed
 * inside one activation. Inlined into an emitted kaikai frame that spans a
 * park, the optimiser could hoist the slot address across the swap and let a
 * work-stolen fiber push a freed cell onto — or pop from — the parking
 * thread's free list, corrupting the heap. Out of line the slot is re-resolved
 * on whatever thread now runs. Each returns/takes only the value, never a slot
 * address, so nothing escapes the frame. */
__attribute__((noinline))
KAI_RT_SHARED KaiValue *kai_cell_pool_pop(void) {
    if (kai_cell_pool_n > 0) return kai_cell_pool[--kai_cell_pool_n];
    return NULL;
}
__attribute__((noinline))
KAI_RT_SHARED int kai_cell_pool_push(KaiValue *v) {
    if (kai_cell_pool_n < KAI_CELL_POOL_CAP && kai_cell_pool_ensure()) {
        kai_cell_pool[kai_cell_pool_n++] = v;
        return 1;
    }
    return 0;
}
__attribute__((noinline))
KAI_RT_SHARED KaiValue *kai_var_block_pool_pop(int n) {
    if (n < 0 || n > KAI_VAR_BLOCK_POOL_MAXN) return NULL;
    KaiValue *v = kai_var_block_free_head[n];
    if (v) kai_var_block_free_head[n] = *(KaiValue **) v;
    return v;
}
__attribute__((noinline))
KAI_RT_SHARED void kai_var_block_pool_push(KaiValue *v, int n) {
    *(KaiValue **) v = kai_var_block_free_head[n];
    kai_var_block_free_head[n] = v;
}

/* ---------- variant-block slab allocator (issue: malloc 6.2% of bench) ----------
 *
 * A fresh variant block used to be `malloc(kai_var_block_size(n))` — one
 * libc allocation per node, 6.96M of them on the rb-tree bench (the tree
 * grows monotonically, so the block pool stays empty during the insert
 * phase and every new node is a real malloc). Idea adapted from Koka's
 * mimalloc segments — amortise the allocator, never hand individual cells
 * back to libc — but WITHOUT a dependency: a plain bump allocator over
 * malloc'd slabs.
 *
 * Design (slab-only, per asu): EVERY variant block comes from a slab.
 * `kai_slab_alloc(sz)` bump-allocates `sz` bytes (8-aligned) from the
 * current slab, growing a new slab when the bump pointer would overrun.
 * The free path (kai_var_block_free) NEVER calls libc free on an
 * individual block — a slab-interior pointer is not a malloc'd address,
 * so free() on it is UB/corruption. Instead a freed block goes to the
 * arity-keyed block free list, which has no cap and covers every
 * arity, so no block is stranded. The slabs themselves are tracked and freed in kai_slab_teardown
 * (registered via atexit) so ASAN sees still-reachable == 0.
 *
 * Soundness net: if any free() ever reaches a slab-interior pointer, ASAN
 * fires "free on non-malloc'd address" immediately. The self-compile gate
 * (kaic2b.c == kaic2c.c) proves no AST node — of any arity — was
 * corrupted by the size-classing. */
#define KAI_SLAB_SIZE (256u * 1024u)   /* 256 KiB per slab */
/* One shared slab allocator across a separate-compilation build: the native-
 * modular merge inlines slab-touching helpers into partitions, so the slab
 * state must be ONE instance (external, owner-defined) — a per-partition copy
 * would bump-allocate into disjoint slabs. */
#if defined(KAI_SEPARATE_COMPILATION)
extern KAI_TLS char  *kai_slab_cur;
extern KAI_TLS size_t kai_slab_off;
extern KAI_TLS char **kai_slab_list;
extern KAI_TLS int    kai_slab_count;
extern KAI_TLS int    kai_slab_cap;
extern KAI_TLS int    kai_slab_atexit;
#  if defined(KAI_RUNTIME_OWNER)
KAI_TLS char  *kai_slab_cur    = NULL;
KAI_TLS size_t kai_slab_off    = 0;
KAI_TLS char **kai_slab_list   = NULL;
KAI_TLS int    kai_slab_count  = 0;
KAI_TLS int    kai_slab_cap    = 0;
KAI_TLS int    kai_slab_atexit = 0;
#  endif
#else
static KAI_TLS char  *kai_slab_cur    = NULL;  /* current slab base */
static KAI_TLS size_t kai_slab_off    = 0;     /* bump offset into current slab */
static KAI_TLS char **kai_slab_list   = NULL;  /* all slabs, for teardown */
static KAI_TLS int    kai_slab_count  = 0;
static KAI_TLS int    kai_slab_cap    = 0;
static KAI_TLS int    kai_slab_atexit = 0;
#endif

/* noinline: the slab bump state (cur/off/list/count/cap) is thread-local, so
 * both functions must keep the slot address inside one activation — inlined
 * into a park-spanning frame the optimiser could carry it across the swap and
 * bump-allocate into, or free, the parking thread's slabs after a work-steal. */
__attribute__((noinline))
KAI_RT_SHARED void kai_slab_teardown(void) {
    if (kai_nthreads > 1) return;
    for (int i = 0; i < kai_slab_count; ++i) free(kai_slab_list[i]);
    free(kai_slab_list);
    kai_slab_list = NULL; kai_slab_count = 0; kai_slab_cap = 0;
    kai_slab_cur = NULL; kai_slab_off = 0;
}

/* The exit reports still read slab values, so the teardown must be the
 * first atexit registered: LIFO runs it last. */
static void kai_slab_register_teardown(void) {
    if (!kai_slab_atexit) { atexit(kai_slab_teardown); kai_slab_atexit = 1; }
}

__attribute__((noinline))
KAI_RT_SHARED void *kai_slab_alloc(size_t sz) {
    sz = (sz + 7u) & ~(size_t) 7u;            /* 8-byte align */
    if (sz > KAI_SLAB_SIZE) return kai_heap_malloc(sz); /* oversized: standalone (never freed individually either) */
    if (!kai_slab_cur || kai_slab_off + sz > KAI_SLAB_SIZE) {
        kai_heap_charge(KAI_SLAB_SIZE);
        char *slab = (char *) malloc(KAI_SLAB_SIZE);
        if (!slab) { kai_eprintf("kai: out of memory (slab)\n"); kai_exit(1); }
        if (kai_slab_count == kai_slab_cap) {
            int ncap = kai_slab_cap == 0 ? 64 : kai_slab_cap * 2;
            kai_slab_list = (char **) realloc(kai_slab_list, (size_t) ncap * sizeof(char *));
            kai_slab_cap = ncap;
        }
        kai_slab_list[kai_slab_count++] = slab;
        kai_slab_cur = slab;
        kai_slab_off = 0;
        /* At N>1 a variant block allocated on this thread's slab can
         * migrate (with a cross-thread message copy) into another
         * thread's block pool and be reused there, so a per-thread slab
         * free at exit would pull the ground out from under a live block
         * on another thread. Under M:N the OS reclaims all slabs at
         * process exit instead; the teardown (whose only purpose is a
         * clean ASAN "0 still-reachable" at N=1) does nothing. */
        if (kai_nthreads <= 1) kai_slab_register_teardown();
    }
    void *p = kai_slab_cur + kai_slab_off;
    kai_slab_off += sz;
    return p;
}

#else
static void kai_slab_register_teardown(void) {}
#endif

KAI_RT_SHARED KaiValue *kai_alloc(KaiTag tag) {
#endif
#ifdef KAI_CELL_POOL_ACTIVE
    KaiValue *v = kai_cell_pool_pop();
    if (v) {
        /* Zero the struct so callers see calloc-equivalent state (the
         * union, slots ptr, etc. must start clean). */
        memset(v, 0, sizeof(KaiValue));
    } else {
        kai_heap_charge(sizeof(KaiValue));
        v = (KaiValue *) calloc(1, sizeof(KaiValue));
    }
#else
    kai_heap_charge(sizeof(KaiValue));
    KaiValue *v = (KaiValue *) calloc(1, sizeof(KaiValue));
#endif
    if (!v) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
    v->rc = 1;
    v->tag = (uint8_t) tag;
    /* trace */
    kai_rc_count_alloc((int) tag);
#ifdef KAI_TRACE_RC
    v->alloc_site = site;
    kai_rc_site_record_alloc(site, (int32_t) tag);
    kai_rc_site_register_once();
    kai_rc_history_log(v, /* op=alloc */ 0, (int32_t) tag);
#endif
    return v;
}

#ifdef KAI_TRACE_RC
/* Macro shim: every call to kai_alloc(tag) inside a wrapper captures
 * the wrapper's caller (the real emit site) via
 * __builtin_return_address(0). Wrappers must be marked
 * KAI_RC_NOINLINE for this to point at the right frame. */
#define kai_alloc(tag) kai_alloc_traced((tag), __builtin_return_address(0))
#endif

/* Byte size of a variant block holding `n` inline slots. */
static inline size_t kai_var_block_size(int n) {
    /* Slots overlap the union at `offsetof(KaiValue, as)`, so the block is
     * the header up to `as` plus n slots — NOT sizeof(KaiValue) (which
     * counts the whole union) + n slots. A 5-slot node is offsetof(as)=8 +
     * 40 = 48 vs the old 80. Guard: at least sizeof(KaiValue) so a node
     * with few slots still has a valid base for the generic header reads.
     * `offsetof(KaiValue, as)` is the UB-free spelling — the old
     * `(char *)&((KaiValue *)1)->...var_slots[0] - (char *)1` trick formed a
     * member access on a misaligned bogus pointer that -fsanitize=undefined
     * flags on every variant alloc under the ASAN tier. */
    size_t base = offsetof(KaiValue, as);
    size_t sz = base + (size_t) n * sizeof(KaiVarSlot);
#ifndef KAI_CELL_POOL_ACTIVE
    /* Diagnostic builds (trace/profile/no-pool) malloc each block
     * individually and generic paths touch the cell through KaiValue*;
     * honour the documented floor so those accesses stay in bounds. */
    if (sz < sizeof(KaiValue)) sz = sizeof(KaiValue);
#endif
    return sz;
}

/* FAM lane: allocate a KAI_VARIANT block with `n` payload slots stored
 * inline (one contiguous allocation). Mirrors kai_alloc's bookkeeping
 * but draws from / returns to the per-arity block pool instead of the
 * size-uniform cell pool, and never touches the separate slot pool.
 * The trace epilogue is shared with kai_alloc via the same counters. */
#ifdef KAI_TRACE_RC
static KaiValue *kai_alloc_var_traced(int n, void *site) {
#else
static KaiValue *kai_alloc_var(int n) {
#endif
    KaiValue *v;
    size_t bsz = kai_var_block_size(n);
#ifdef KAI_CELL_POOL_ACTIVE
    v = kai_var_block_pool_pop(n);
    if (v) {
        memset(v, 0, bsz);
    } else {
        v = (KaiValue *) kai_slab_alloc(bsz);
        memset(v, 0, bsz);                 /* slab is uninit; this is the calloc-equivalent */
    }
#else
    /* No cell pool (KAI_TRACE_RC / KAI_NO_CELL_POOL):
     * the slab allocator lives inside the cell-pool block, so fall back
     * to a plain libc allocation. kai_var_block_free's matching #else
     * calls free() on it. */
    kai_heap_charge(bsz);
    v = (KaiValue *) calloc(1, bsz);
#endif
    if (!v) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
    v->rc = 1;
    v->tag = (uint8_t) KAI_VARIANT;
    kai_rc_count_alloc((int) KAI_VARIANT);
#ifdef KAI_TRACE_RC
    v->alloc_site = site;
    kai_rc_site_record_alloc(site, (int32_t) KAI_VARIANT);
    kai_rc_site_register_once();
    kai_rc_history_log(v, /* op=alloc */ 0, (int32_t) KAI_VARIANT);
#endif
    return v;
}
#ifdef KAI_TRACE_RC
#define kai_alloc_var(n) kai_alloc_var_traced((n), __builtin_return_address(0))
#endif

/* No-zero variant block allocator. Identical to kai_alloc_var except it
 * skips the calloc/memset zero-init: the caller (kai_variant_u_fast)
 * writes all n payload slots immediately after, so zeroing them is dead
 * work — measured 22M instructions (6%) of the rb-tree bench sat in
 * _int_malloc+calloc, the zero pass over 6.96M × 48 B nodes. malloc
 * leaves the slots indeterminate, which is sound ONLY because every slot
 * is overwritten before any read; the drop walker never sees an
 * unwritten slot. This is Koka's kk_alloc (raw malloc, no zero) — the
 * cell is fully initialised by the constructor, not by the allocator.
 *
 * The header fields the generic runtime reads (rc, tag, var_n_args,
 * variant_tag) are all written here or by the caller, so none rely on
 * the zero. Under tracing the alloc_site field is written
 * explicitly too. */
#ifdef KAI_TRACE_RC
static KaiValue *kai_alloc_var_nz_traced(int n, void *site) {
#else
static KaiValue *kai_alloc_var_nz(int n) {
#endif
    KaiValue *v;
#ifdef KAI_CELL_POOL_ACTIVE
    v = kai_var_block_pool_pop(n);
    if (!v) {
        /* pool miss: fresh slab block, no memset — caller overwrites every slot */
        v = (KaiValue *) kai_slab_alloc(kai_var_block_size(n));
    }
#else
    /* No cell pool: plain libc malloc (no-zero — caller fills slots);
     * kai_var_block_free's #else frees it. */
    v = (KaiValue *) kai_heap_malloc(kai_var_block_size(n));
#endif
    if (!v) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
    v->rc = 1;
    v->tag = (uint8_t) KAI_VARIANT;
    kai_rc_count_alloc((int) KAI_VARIANT);
#ifdef KAI_TRACE_RC
    v->alloc_site = site;
    kai_rc_site_record_alloc(site, (int32_t) KAI_VARIANT);
    kai_rc_site_register_once();
    kai_rc_history_log(v, /* op=alloc */ 0, (int32_t) KAI_VARIANT);
#endif
    return v;
}
#ifdef KAI_TRACE_RC
#define kai_alloc_var_nz(n) kai_alloc_var_nz_traced((n), __builtin_return_address(0))
#endif

/* FAM lane: return a variant block (header + inline slots) to its
 * per-arity free-list pool. Used by kai_free_value's VARIANT case in
 * place of a cell-pool return.
 *
 * Slab-only invariant: every block lives inside a malloc'd slab
 * (kai_slab_alloc), so it is NOT a standalone malloc'd address — calling
 * libc free() on it is UB/corruption. So there is NO free(v) here: the
 * block rejoins its arity's free list, and the slabs are reclaimed
 * wholesale by kai_slab_teardown at exit. If a free() ever reaches here on
 * a slab pointer, ASAN fires "free on non-malloc'd address" at once. */
static void kai_var_block_free(KaiValue *v, int n) {
#ifdef KAI_CELL_POOL_ACTIVE
    kai_var_block_pool_push(v, n);
#else
    /* No cell pool: blocks come from plain malloc/calloc (kai_alloc_var
     * #else), so free them individually — no slab to reclaim them, and
     * dropping here would leak (visible under the KAI_TRACE_RC tier). */
    (void) n;
    free(v);
#endif
}

/* The captures of a closure with more than one. Up to a cell's worth
 * of them are a cell of the cell pool, so building and dropping the
 * closure does not reach libc; a block of its own past that. Never a
 * slab block: a closure can outlive the slabs, read by the exit report. */
static inline KaiValue **kai_caps_alloc(int n) {
#ifdef KAI_CELL_POOL_ACTIVE
    if ((size_t) n * sizeof(KaiValue *) <= sizeof(KaiValue)) {
        KaiValue *cell = kai_cell_pool_pop();
        return (KaiValue **) (cell != NULL ? cell : malloc(sizeof(KaiValue)));
    }
#endif
    return (KaiValue **) malloc((size_t) n * sizeof(KaiValue *));
}

static inline void kai_caps_free(KaiValue **caps, int n) {
#ifdef KAI_CELL_POOL_ACTIVE
    if ((size_t) n * sizeof(KaiValue *) <= sizeof(KaiValue) && kai_cell_pool_push((KaiValue *) caps)) return;
#endif
    free(caps);
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
/* Singletons are returned by address (kai_unit() = &kai_singleton_unit),
 * so their identity is observable: any pointer-equality fast path breaks if
 * two TUs each hold a private copy. One shared instance (owner defines,
 * others extern) keeps unit/true/false/nil pointer-identical across TUs. */
#if defined(KAI_SEPARATE_COMPILATION)
extern KaiValue kai_singleton_unit;
extern KaiValue kai_singleton_true;
extern KaiValue kai_singleton_false;
extern KaiValue kai_singleton_nil;
#  if defined(KAI_RUNTIME_OWNER)
KaiValue kai_singleton_unit  = { .rc = INT32_MAX, .tag = KAI_UNIT, .as = { .b = 0 } };
KaiValue kai_singleton_true  = { .rc = INT32_MAX, .tag = KAI_BOOL, .as = { .b = 1 } };
KaiValue kai_singleton_false = { .rc = INT32_MAX, .tag = KAI_BOOL, .as = { .b = 0 } };
KaiValue kai_singleton_nil   = { .rc = INT32_MAX, .tag = KAI_NIL,  .as = { .b = 0 } };
#  endif
#else
static KaiValue kai_singleton_unit  = { .rc = INT32_MAX, .tag = KAI_UNIT, .as = { .b = 0 } };
static KaiValue kai_singleton_true  = { .rc = INT32_MAX, .tag = KAI_BOOL, .as = { .b = 1 } };
static KaiValue kai_singleton_false = { .rc = INT32_MAX, .tag = KAI_BOOL, .as = { .b = 0 } };
static KaiValue kai_singleton_nil   = { .rc = INT32_MAX, .tag = KAI_NIL,  .as = { .b = 0 } };
#endif

/* issue #120 — opt-in Perceus regions: bump-arena primitive (P0).
 *
 * A `region { ... }` block bump-allocates every value constructed
 * inside it into an arena and frees the whole arena in one shot when
 * the brace closes — no per-value RC traffic, no per-value free walk.
 * This pays where allocation is tight LIFO scratch (lexer / parser /
 * formatter buffers): issue #120.
 *
 * KEY TRICK (asu + linus, docs/issue-120-regions-design.md): an arena
 * value carries `rc = INT32_MAX`, the EXISTING saturation sentinel the
 * runtime already short-circuits in kai_incref / kai_decref /
 * kai_check_unique. So to the RC machinery an arena value is
 * indistinguishable from a singleton:
 *   - decref is a no-op (kai_decref returns early on INT32_MAX) — FREE.
 *   - reuse-in-place auto-disables (kai_check_unique returns 0 for
 *     INT32_MAX) — FREE, no Perceus pass change.
 * No new tag, no new branch in the hot RC path, no struct growth.
 *
 * BOOKKEEPING LANDMINE: because kai_arena_free reclaims in bulk
 * WITHOUT walking values, kai_rc_live_now is never decremented per
 * value. Left alone, KAI_TRACE_RC leaked-vs-iterations gates would
 * report false leaks. Fix: a SEPARATE per-arena live count
 * (`n_live`) is subtracted from kai_rc_live_now on free, and the
 * dedicated kai_arena_alloc_total / kai_arena_free_total counters are
 * surfaced in kai_rc_report() so a wrong-codegen silent leak (a
 * non-region value mistakenly arena-allocated — invisible to ASAN
 * because nothing is freed) shows up as alloc/free divergence.
 *
 * PORTABILITY: malloc-backed chunks, NOT mmap — stage0 must build on
 * any ANSI cc with zero deps (CLAUDE.md "no deps in stage0"). The
 * arena stack is a plain global, not __thread: kaikai fibers each run
 * to a suspension point on the OS thread that dispatched them, so a
 * region never spans a fiber switch in v1. A per-fiber arena stack is
 * the natural follow-up once regions are allowed to straddle await
 * (see docs/lane-experience-issue-120.md). */

#define KAI_ARENA_CHUNK_BYTES (64 * 1024)
#define KAI_ARENA_ALIGN       16

typedef struct KaiArenaChunk {
    struct KaiArenaChunk *next;   /* grow-only singly-linked list */
    size_t                used;   /* bytes consumed in `data` */
    size_t                cap;    /* usable bytes in `data` */
    unsigned char        *data;   /* malloc-backed payload */
} KaiArenaChunk;

typedef struct KaiArena {
    KaiArenaChunk *head;          /* current (most-recently-grown) chunk */
    int64_t        n_live;        /* KaiValue headers stamped into this arena */
} KaiArena;

static size_t kai_arena_align_up(size_t n) {
    return (n + (KAI_ARENA_ALIGN - 1)) & ~((size_t) (KAI_ARENA_ALIGN - 1));
}

static KaiArenaChunk *kai_arena_chunk_new(size_t need) {
    size_t cap = KAI_ARENA_CHUNK_BYTES;
    if (need > cap) cap = need;               /* oversized single object */
    kai_heap_charge(sizeof(KaiArenaChunk) + cap);
    KaiArenaChunk *c = (KaiArenaChunk *) malloc(sizeof(KaiArenaChunk));
    if (!c) { kai_eprintf("kai: out of memory (arena chunk)\n"); kai_exit(1); }
    c->data = (unsigned char *) malloc(cap);
    if (!c->data) { kai_eprintf("kai: out of memory (arena data)\n"); kai_exit(1); }
    c->next = NULL;
    c->used = 0;
    c->cap  = cap;
    return c;
}

static void kai_arena_init(KaiArena *a) {
    a->head   = NULL;
    a->n_live = 0;
}

/* Raw aligned bump for interior storage (record `fields`, variant
 * `slots`, array `items`, KAI_STR `bytes`) so an aggregate built in a
 * region keeps ALL of its memory inside the arena — otherwise the
 * interior arrays would dangle on bulk free. Returns uninitialised
 * memory; the caller writes it. Not refcounted, not a KaiValue. */
static void *kai_arena_raw(KaiArena *a, size_t nbytes) {
    size_t need = kai_arena_align_up(nbytes ? nbytes : 1);
    if (!a->head || a->head->used + need > a->head->cap) {
        KaiArenaChunk *c = kai_arena_chunk_new(need);
        c->next = a->head;
        a->head = c;
    }
    void *p = a->head->data + a->head->used;
    a->head->used += need;
    return p;
}

/* Bump a KaiValue header into the arena, stamped with the immortal
 * sentinel so RC skips it. Mirrors kai_alloc's header init (rc, tag,
 * zeroed union) but never calls calloc/free and never touches the
 * per-value RC counters — only the dedicated arena counters. Live-now
 * is bumped so live_peak stays honest mid-region; kai_arena_free
 * subtracts the whole batch back. */
static KaiValue *kai_arena_alloc(KaiArena *a, KaiTag tag) {
    KaiValue *v = (KaiValue *) kai_arena_raw(a, sizeof(KaiValue));
    memset(v, 0, sizeof(KaiValue));
    v->rc  = INT32_MAX;                 /* immortal sentinel: decref no-op */
    v->tag = (uint8_t) tag;
    a->n_live++;
    kai_arena_alloc_total++;
    kai_rc_count_live_inc();
    return v;
}

/* Bulk reclaim: free every chunk WITHOUT walking values (the whole
 * point — no per-value free, no per-value decref). Balance the trace
 * ledger by subtracting this arena's live count from kai_rc_live_now
 * and crediting kai_arena_free_total, then reset so the arena can be
 * reused. */
static void kai_arena_free(KaiArena *a) {
    KaiArenaChunk *c = a->head;
    while (c) {
        KaiArenaChunk *next = c->next;
        free(c->data);
        free(c);
        c = next;
    }
    kai_arena_free_total += a->n_live;
    kai_rc_count_live_sub(a->n_live);
    a->head   = NULL;
    a->n_live = 0;
}

/* Lexical region stack. `region { ... }` pushes a fresh arena on
 * entry and pops+frees it on exit; constructors inside the block call
 * kai_arena_alloc on the current top. Nesting is bounded by lexical
 * block depth — 64 is far beyond any realistic region nesting and
 * keeps the stack a flat global with no allocation of its own. */
#define KAI_ARENA_STACK_MAX 64
/* One region stack per process: a `region { }` pushing in one TU and the
 * matching pop in another must land on the same stack, so it is shared
 * (owner defines, others extern) — a per-TU copy would push and pop two
 * disjoint stacks and desync the region depth. */
#if defined(KAI_SEPARATE_COMPILATION)
extern KAI_TLS KaiArena kai_arena_stack[KAI_ARENA_STACK_MAX];
extern KAI_TLS int      kai_arena_sp;
#  if defined(KAI_RUNTIME_OWNER)
KAI_TLS KaiArena kai_arena_stack[KAI_ARENA_STACK_MAX];
KAI_TLS int      kai_arena_sp = 0;
#  endif
#else
static KAI_TLS KaiArena kai_arena_stack[KAI_ARENA_STACK_MAX];
static KAI_TLS int      kai_arena_sp = 0;
#endif

static KaiArena *kai_arena_push(void) {
    if (kai_arena_sp >= KAI_ARENA_STACK_MAX) {
        kai_eprintf("kai: region nesting exceeds %d\n", KAI_ARENA_STACK_MAX);
        kai_exit(1);
    }
    KaiArena *a = &kai_arena_stack[kai_arena_sp++];
    kai_arena_init(a);
    return a;
}

static KaiArena *kai_arena_current(void) {
    if (kai_arena_sp == 0) return NULL;     /* not inside a region */
    return &kai_arena_stack[kai_arena_sp - 1];
}

static void kai_arena_pop(void) {
    if (kai_arena_sp == 0) return;
    kai_arena_free(&kai_arena_stack[--kai_arena_sp]);
}

/* `rc >= KAI_RC_SPECIAL` is never a plain count: INT32_MAX is immortal and
 * KAI_RC_SPECIAL + n is an atomic count of n, for handles shared across
 * threads (Fiber[T]). A plain count traps before it reaches KAI_RC_SPECIAL,
 * so the hot path tells all three apart with one compare and never reads
 * the tag or kai_nthreads. The load is relaxed-atomic because an atomic
 * cell's rc is written from several threads; on x86/arm it is a plain ldr. */
#define KAI_RC_SPECIAL 0x40000000

static inline int32_t kai_rc_load(const KaiValue *v) {
    return atomic_load_explicit((const _Atomic int32_t *) &v->rc,
                                memory_order_relaxed);
}

