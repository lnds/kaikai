/* Fragment of runtime.h; include runtime.h, never this file. */
#ifdef KAI_TRACE_RC
#define KAI_RC_RECYCLE_TRACE(_kc) do {                                       \
    int32_t freed_tag = (_kc)->tag;                                          \
    if (freed_tag >= 0 && freed_tag < 16) kai_rc_free_by_tag[freed_tag]++;   \
    kai_rc_site_record_free((_kc)->alloc_site);                              \
    kai_rc_history_log((_kc), /* op=free */ 3, freed_tag);                   \
    { uint64_t *p64 = (uint64_t *) (_kc);                                    \
      size_t _psz = (freed_tag == (int32_t) KAI_VARIANT)                     \
          ? kai_var_block_size((_kc)->var_n_args) : sizeof(KaiValue);        \
      size_t nq = _psz / sizeof(uint64_t);                                   \
      for (size_t i = 0; i < nq; i++) p64[i] = KAI_RC_SENTINEL_U64; }        \
} while (0)
#else
#define KAI_RC_RECYCLE_TRACE(_kc) ((void) 0)
#endif

#ifdef KAI_CELL_POOL_ACTIVE
#define KAI_RC_RECYCLE_POOL(_kc) do {                                        \
    if ((_kc)->tag == (int32_t) KAI_VARIANT) {                              \
        kai_var_block_free((_kc), (_kc)->var_n_args);                        \
    } else if (!kai_cell_pool_push(_kc)) {                                    \
        free(_kc);                                                           \
    }                                                                        \
} while (0)
#else
#define KAI_RC_RECYCLE_POOL(_kc) free(_kc)
#endif

/* An atomic cell's decrement; 1 when it was the last owner. The release
 * orders this thread's writes to the cell before the drop; the last owner's
 * acquire makes every other owner's writes visible before the free. TSAN
 * does not model a standalone fence, so its build folds both into the
 * fetch_sub. */
KAI_RT_SHARED __attribute__((noinline)) int kai_drop_atomic_hits_zero(KaiValue *v) {
    KAI_RC_TRAFFIC_INC(kai_rc_decref_total);
#ifdef KAI_TRACE_RC
    kai_rc_history_log(v, /* op=decref */ 2, v->tag);
#endif
#if defined(KAI_TSAN_FIBERS)
    int32_t old = atomic_fetch_sub_explicit((_Atomic int32_t *) &v->rc, 1,
                                            memory_order_acq_rel);
#else
    int32_t old = atomic_fetch_sub_explicit((_Atomic int32_t *) &v->rc, 1,
                                            memory_order_release);
#endif
    if (old != KAI_RC_SPECIAL + 1) return 0;
#if !defined(KAI_TSAN_FIBERS)
    atomic_thread_fence(memory_order_acquire);
#endif
    atomic_store_explicit((_Atomic int32_t *) &v->rc, 0, memory_order_relaxed);
    return 1;
}

/* One reference dropped: 1 when the cell just reached rc 0 and must be
 * freed. The counters and trace fire exactly as a decref's. */
static inline int kai_drop_hits_zero(KaiValue *v) {
    if (kai_is_value(v) || !v) return 0;
    int32_t r = kai_rc_load(v);
    if (r >= KAI_RC_SPECIAL) return r != INT32_MAX && kai_drop_atomic_hits_zero(v);
    KAI_RC_TRAFFIC_INC(kai_rc_decref_total);
#ifdef KAI_TRACE_RC
    kai_rc_history_log(v, /* op=decref */ 2, v->tag);
#endif
    v->rc = r - 1;
    return r == 1;
}

static KAI_RC_NOINLINE void kai_free_stack_grow(KaiFreeStack *st) {
    int cap = st->cap * 2;
    KaiValue **grown = (KaiValue **) malloc(sizeof(KaiValue *) * (size_t) cap);
    if (!grown) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
    memcpy(grown, st->items, sizeof(KaiValue *) * (size_t) st->n);
    if (st->items != st->local) free(st->items);
    st->items = grown;
    st->cap = cap;
}

static inline void kai_free_stack_push(KaiFreeStack *st, KaiValue *v) {
    if (st->n == st->cap) kai_free_stack_grow(st);
    st->items[st->n++] = v;
}

KAI_RT_SHARED void kai_free_rec(KaiValue *v, int depth);
KAI_RT_SHARED void kai_free_drain(KaiValue *v);

/* Reclaim a child that just reached rc 0: onto the stack while draining,
 * by recursion while the depth budget lasts, else through a fresh drain. */
static inline __attribute__((always_inline))
void kai_free_child(KaiValue *c, int depth, KaiFreeStack *st) {
    if (st) kai_free_stack_push(st, c);
    else if (depth < KAI_FREE_DEPTH) kai_free_rec(c, depth + 1);
    else kai_free_drain(c);
}

KAI_RT_SHARED_INLINE void kai_drop_child(KaiValue *c, int depth, KaiFreeStack *st) {
    if (!kai_drop_hits_zero(c)) return;
    kai_free_child(c, depth, st);
}

#if KAI_HOST_JS
__attribute__((import_module("kai_js"), import_name("release"))) extern void kai_js_release(uint32_t idx);
__attribute__((import_module("kai_js"), import_name("take"))) extern void kai_js_take(char *dst, uint32_t n);
#endif

/* One body for both modes; each wrapper inlines it with `st` fixed, so the
 * recursive mode never tests for a stack. */
static inline __attribute__((always_inline))
void kai_free_one(KaiValue *v, int depth, KaiFreeStack *st) {
  for (;;) {
    KaiValue *next = NULL;
    switch ((KaiTag) v->tag) {
        case KAI_STR:
            kai_heap_free(v->as.s.bytes);
            break;
        case KAI_CONS:
            /* A unique tail continues this loop at the same depth, so a
             * list of any length frees in O(1) stack. */
            kai_drop_child(v->as.cons.head, depth, st);
            if (kai_drop_hits_zero(v->as.cons.tail)) next = v->as.cons.tail;
            break;
        case KAI_RECORD:
            for (int i = 0; i < v->as.rec.n_fields; ++i) kai_drop_child(v->as.rec.fields[i], depth, st);
            free(v->as.rec.fields);
            free((void *) v->as.rec.names);
            break;
        case KAI_VARIANT: {
            /* Only pointer slots carry RC (mask kind PTR; mask==0 means
             * all-PTR; the mask lookup is hoisted — the tag is fixed
             * across the loop). A unique variant child transfers to the
             * free stack with no decrement: it is the per-node traffic of
             * a tree free. Every other slot drops as a decref would. FAM
             * payload slots are inline in this block — the whole block
             * returns to the per-arity variant pool at the recycle step. */
            uint32_t fmask = kai_slot_mask_of(v->variant_tag);
            int fn_args = v->var_n_args;
            for (int i = 0; i < fn_args; ++i) {
                if (fmask != 0 && kai_var_slot_kind(fmask, i) != KAI_VAR_SLOT_PTR) continue;
                KaiValue *c = kai_var_slots(v)[i].ptr;
                if (kai_is_ptr(c) && kai_rc_load(c) == 1 && c->tag == (int32_t) KAI_VARIANT) {
                    c->rc = 0;
                    kai_free_child(c, depth, st);
                } else {
                    kai_drop_child(c, depth, st);
                }
            }
        }
            break;
        case KAI_CLOSURE:
            for (int i = 0; i < v->as.clo.n_captures; ++i) kai_drop_child(v->as.clo.captures[i], depth, st);
            if (v->as.clo.n_captures > 1) kai_caps_free(v->as.clo.captures, v->as.clo.n_captures);
            break;
        case KAI_ARRAY:
            for (int64_t i = 0; i < v->as.arr.len; ++i) kai_drop_child(v->as.arr.items[i], depth, st);
            kai_heap_free(v->as.arr.items);
            break;
        case KAI_VEC: {
            /* A view owns nothing but its ref on the owner: release it
             * and stop — elements and block belong to the owner. */
            if (v->as.vec.view_of) {
                kai_drop_child(v->as.vec.view_of, depth, st);
                break;
            }
            /* Only boxed elements carry RC; raw / inline-record elements
             * are plain bytes freed with the block. The meta prefix and
             * the elements are ONE allocation (see kai_vec_meta). */
            if (kai_vec_meta(v)->ekind == KAI_VEC_EK_BOXED) {
                KaiValue **items = (KaiValue **) kai_vec_elems(v);
                for (int64_t i = 0; i < v->as.vec.len; ++i) kai_drop_child(items[i], depth, st);
            }
            kai_heap_free(v->as.vec.data);
            break;
        }
        case KAI_REF:
            /* A Ref owns one strong reference to its cell. */
            kai_drop_child(v->as.ref.cell, depth, st);
            break;
        case KAI_FIBER:
            if (v->as.fib) {
                kai_drop_child(v->as.fib->thunk, depth, st);
                kai_drop_child(v->as.fib->result, depth, st);
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
                        if (mn->target_pid) kai_drop_child(mn->target_pid, depth, st);
                        free(mn);
                        mn = next;
                    }
                    v->as.fib->monitor_head = NULL;
                }
                /* A fiber freed without terminating never ran to an exit;
                 * its pids must not keep pointing at it. */
                {
                    KaiFiberExit *x = atomic_load_explicit(&v->as.fib->exit_rec,
                                                           memory_order_acquire);
                    if (x) {
                        kai_fiber_exit_lock(x);
                        if (x->fiber == v->as.fib) {
                            x->fiber  = NULL;
                            x->reason = KAI_EXIT_CRASHED;
                        }
                        kai_fiber_exit_unlock(x);
                        kai_fiber_exit_unref(x);
                    }
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
                    kai_pending_free_set(v->as.fib);
                } else {
                    /* m8.x: release the private stack. Main fiber has
                     * stack_base == NULL (uses the OS thread stack)
                     * and is statically allocated; spawned fibers own
                     * an mmap region of stack_size + guard page bytes
                     * and must release it via munmap. */
#if KAI_HOST_STACK_GUARD
                    if (v->as.fib->stack_base) {
                        munmap(v->as.fib->stack_base,
                               v->as.fib->stack_size + kai_page_size());
                    }
#elif defined(__wasm__)
                    free(v->as.fib->stack_base);
#endif
                    free(v->as.fib->unw_buf);
                    free(v->as.fib->rframe_buf);
                    free(v->as.fib->free_stack);
                    free(v->as.fib);
                }
            }
            break;
        case KAI_PID:
            /* The mailbox is owned by the with_mailbox / spawn_actor
             * scope, NOT by the Pid value. Dropping a Pid handle
             * does not free the mailbox. */
            kai_fiber_exit_unref(v->as.pid.owner);
            break;
        case KAI_CONT:
            kai_seg_cont_drop(v);
            break;
        case KAI_SEQ:
            kai_drop_child(v->as.seq.slot, depth, st);
            kai_drop_child(v->as.seq.pred, depth, st);
            break;
#if KAI_HOST_JS
        case KAI_FOREIGN:
            if (v->variant_tag == 1) kai_js_release((uint32_t) (uintptr_t) v->as.foreign_ptr);
            break;
#endif
        case KAI_BYTE:
            /* Unreachable: every Byte is an immortal kai_byte_cache cell. */
            break;
        case KAI_RANGE:
            /* Three inline ints, no interior pointers — plain recycle. */
            break;
        default: break;
    }
    /* Recycle this cell (trace+poison+pool+counters). */
    KAI_RECYCLE_CELL(v);
    if (!next) return;
    v = next;
  }
}

__attribute__((noinline))
KAI_RT_SHARED void kai_free_drain(KaiValue *v) {
    KaiFiber *f = kai_current_fiber();
    KaiFreeStack *st = f->free_stack;
    if (!st) {
        st = (KaiFreeStack *) calloc(1, sizeof *st);
        if (!st) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
        f->free_stack = st;
    }
    if (st->active) { kai_free_stack_push(st, v); return; }
    if (!st->items) { st->items = st->local; st->cap = KAI_FREE_STACK_LOCAL; }
    st->active = 1;
    st->items[st->n++] = v;
    while (st->n > 0) kai_free_one(st->items[--st->n], 0, st);
    st->active = 0;
    if (st->items != st->local) {
        free(st->items);
        st->items = st->local;
        st->cap = KAI_FREE_STACK_LOCAL;
    }
}

KAI_RT_SHARED void kai_free_rec(KaiValue *v, int depth) { kai_free_one(v, depth, NULL); }

static void kai_free_value(KaiValue *v) { kai_free_rec(v, 0); }

/* Out-of-line drop-to-zero path: the cell reached rc==0, reclaim it.
 * Split out of kai_decref so the common case (decrement, still live)
 * stays a small inlinable body and only the rare free crosses a call.
 * Idea adapted from Koka's kk_block_decref / kk_block_drop split: the
 * drop fast path is inline at the call site, the free is a cold helper.
 * kaikai keeps its own counters + kai_free_value walker, not kklib's
 * free-list shape — the split is the idea, the body stays ours. */
KAI_RT_SHARED void kai_decref_free(KaiValue *v) {
    kai_free_value(v);
}

/* Decrement a reference. The fast path — immediate Int / null /
 * saturated singleton / still-shared after decrement — is branch-only
 * and inlines into the caller; only a cell hitting rc==0 calls out to
 * kai_decref_free. Previously kai_decref was a non-inline `static void`
 * (callgrind: 48M instructions on the rb-tree bench, a real call at
 * every drop). Under tracing the counters still fire for full fidelity.
 * The inline body must stay small to be inlined. */
KAI_RT_SHARED_INLINE void kai_decref(KaiValue *v) {
    if (kai_drop_hits_zero(v)) kai_decref_free(v);
}

/* m5 #4 — Perceus dup/drop wrappers callable as KaiValue-returning fns.
 *
 * `__perceus_dup` and `__perceus_drop` are AST-level magic names the
 * `perceus_pass` rewrites onto non-last EVar reads (dup) and unused-
 * binding scope ends (drop). The emitter lowers them to these
 * wrappers so the pass operates entirely through the core path
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

/* Char cache (Char is not yet a value-immediate). Every cached entry
 * carries `rc = INT32_MAX` so `kai_incref` / `kai_decref` skip them.
 *
 * The Int cache that used to live here is GONE: with the Koka
 * tagged-value Int (see the section near struct KaiValue) a small Int
 * is an immediate with no heap footprint, so there is nothing to
 * cache. The old [-65536..65535] x 48 B .bss table and its lazy-warm
 * branch were the m5.x patch that approximated value-immediates; its
 * own comment named "tagged pointers + raw-slot extract" as the real
 * follow-up, which is exactly this port. */
#define KAI_CHAR_CACHE_HI  ((uint32_t) 127)
#define KAI_CHAR_CACHE_SIZE 128

/* Cached char values are returned by address (&kai_char_cache[c]), so like
 * the singletons they must be one shared instance across TUs — a per-TU
 * copy would hand out non-identical pointers for the same char and warm the
 * cache once per TU. Owner defines, others extern. */
#if defined(KAI_SEPARATE_COMPILATION)
extern KaiValue kai_char_cache[KAI_CHAR_CACHE_SIZE];
extern int      kai_char_cache_init;
#  if defined(KAI_RUNTIME_OWNER)
KaiValue kai_char_cache[KAI_CHAR_CACHE_SIZE];
int      kai_char_cache_init = 0;
#  endif
#else
static KaiValue kai_char_cache[KAI_CHAR_CACHE_SIZE];
static int kai_char_cache_init = 0;
#endif

/* Per-entry lazy warm for the Int cache (Phase 1.A, 2026-05-29).
 *
 * The widened range (131072 entries x 48 B = 6 MB) makes a one-shot
 * O(size) warm a 6 MB RSS hit on the FIRST in-range int — paid even
 * by a program that only ever mints `0` and `1`. Measured: a hello
 * world jumped 1.5 MB -> 6.8 MB under the eager warm.
 *
 * Instead we initialize each cache slot the first time its exact value
 * is requested. A never-warmed slot is all-zero (.bss), i.e.
 * rc == 0 != INT32_MAX, so the `rc != INT32_MAX` test detects it. This
 * touches only the OS pages (4 KB) backing the int values actually
 * used, so RSS scales with the program's working set of small ints,
 * not with the cache range. The pinned `rc = INT32_MAX` still makes
 * `kai_incref` / `kai_decref` short-circuit, and the returned pointer
 * is stable for the process lifetime (slots are never re-warmed once
 * pinned). One extra branch per in-range `kai_int`, predictably taken
 * after the slot is hot. (CPython's small-int cache warms eagerly; we
 * warm lazily because our range — and entry size — is far larger.) */
static void kai_char_cache_warm(void) {
    for (int k = 0; k < KAI_CHAR_CACHE_SIZE; k++) {
        kai_char_cache[k].rc = INT32_MAX;
        kai_char_cache[k].tag = KAI_CHAR;
        kai_char_cache[k].as.c = (uint32_t) k;
    }
    kai_char_cache_init = 1;
}

/* Koka kk_integer_from_small (integer.h:211-215): a small Int is an
 * immediate value — no heap, no RC. Only out-of-range Ints (Koka's
 * bigint path) heap-allocate a KAI_INT fallback. The 5 MB int cache is
 * gone; immediacy makes it pointless.
 *
 * `kai_int` is the inline fast path: a 63-bit-fitting value (the
 * overwhelming common case — every loop counter, key, index) becomes a
 * tagged immediate with one shift and no call. Only the out-of-range
 * bignum tail detours to the NOINLINE heap-alloc helper. Before this
 * split `kai_int` was wholly NOINLINE, so reading an Int back from a
 * KAI_VAR_SLOT_INT slot (`kai_int(slot.i64)`) was a real call at every
 * field touch — the round-trip that made the raw-i64 slot kind look
 * slower than the tagged-ptr slot. With the fast path inlined the slot
 * read is a shift, the slot stays KAI_VAR_SLOT_INT (so the generic drop
 * walker skips it — no decref on keys), and both wins compose. */
static KAI_RC_NOINLINE KaiValue *kai_int_big(int64_t i) {
    KaiValue *v = kai_alloc(KAI_INT);
    v->as.i = i;
    return v;
}
static inline KaiValue *kai_int(int64_t i) {
    if (kai_int_fits_immediate(i)) return kai_tagged_int(i);
    return kai_int_big(i);
}

/* Uniform Int accessors — understand BOTH the immediate and the heap
 * fallback forms, so every hot op (kai_op_*) and every emitter unbox
 * site reads an Int the same way and the boxed↔unboxed frontier (the
 * re-box-on-every-boundary that the rb-tree bench measured) is gone.
 * Mirror of Koka's kk_integer accessors (integer.h). */
static inline int kai_is_int(KaiValue *v) {
    return kai_is_value(v) || (v != NULL && v->tag == KAI_INT);
}
static inline int64_t kai_intf(KaiValue *v) {
    /* The immediate (tagged) form is the overwhelming common case — every
     * loop counter, key, index. Only the 63-bit-overflow bignum tail
     * (|n| > 2^62) detours to the heap `v->as.i` load, which cannot be
     * speculated (immediate `v` is not a valid address). We keep the branch
     * (soundness: heap bignums are real) but hint it so the predictor and
     * layout favour the immediate path. The heap load stays off the hot
     * path. */
    intptr_t iv = (intptr_t) v;
    if (__builtin_expect((iv & KAI_INT_TAG_BIT) != 0, 1)) return iv >> 1;
    return v->as.i;
}

static KAI_RC_NOINLINE KaiValue *kai_real(double r) {
    KaiValue *v = kai_alloc(KAI_REAL);
    v->as.r = r;
    return v;
}

/* All 256 Bytes are immortal cells built at compile time: `kai_byte` never
 * allocates, and the table is read-only from process start, so threads share
 * it with no warm-up. Returned by address, so one instance across TUs (owner
 * defines, others extern). */
#define KAI_BYTE_CELL(n)    { .rc = INT32_MAX, .tag = KAI_BYTE, .as = { .byte_val = (uint8_t) (n) } }
#define KAI_BYTE_CELLS4(n)  KAI_BYTE_CELL(n), KAI_BYTE_CELL((n) + 1), \
                            KAI_BYTE_CELL((n) + 2), KAI_BYTE_CELL((n) + 3)
#define KAI_BYTE_CELLS16(n) KAI_BYTE_CELLS4(n), KAI_BYTE_CELLS4((n) + 4), \
                            KAI_BYTE_CELLS4((n) + 8), KAI_BYTE_CELLS4((n) + 12)
#define KAI_BYTE_CELLS64(n) KAI_BYTE_CELLS16(n), KAI_BYTE_CELLS16((n) + 16), \
                            KAI_BYTE_CELLS16((n) + 32), KAI_BYTE_CELLS16((n) + 48)
#define KAI_BYTE_CELLS_ALL  KAI_BYTE_CELLS64(0), KAI_BYTE_CELLS64(64), \
                            KAI_BYTE_CELLS64(128), KAI_BYTE_CELLS64(192)
#if defined(KAI_SEPARATE_COMPILATION)
extern KaiValue kai_byte_cache[256];
#  if defined(KAI_RUNTIME_OWNER)
KaiValue kai_byte_cache[256] = { KAI_BYTE_CELLS_ALL };
#  endif
#else
static KaiValue kai_byte_cache[256] = { KAI_BYTE_CELLS_ALL };
#endif

static inline KaiValue *kai_byte(uint8_t n) { return &kai_byte_cache[n]; }

/* Fixed-width integer boxes (numeric lane A). Used only when a raw iN
 * value must cross into a boxed slot (Show, polymorphic container);
 * arithmetic stays raw and never allocates. */
static KAI_RC_NOINLINE KaiValue *kai_int32(int32_t n) {
    KaiValue *v = kai_alloc(KAI_INT32);
    v->as.i32 = n;
    return v;
}
static KAI_RC_NOINLINE KaiValue *kai_uint32(uint32_t n) {
    KaiValue *v = kai_alloc(KAI_UINT32);
    v->as.u32 = n;
    return v;
}
static KAI_RC_NOINLINE KaiValue *kai_uint64(uint64_t n) {
    KaiValue *v = kai_alloc(KAI_UINT64);
    v->as.u64 = n;
    return v;
}
/* Load / store an Int128 box's value through its two 8-byte halves.
 * `memcpy` is alignment-agnostic, so this never triggers the misaligned
 * `__int128` access UB that a bare `v->as.i128` would on an 8-byte-aligned
 * KaiValue (the cause of the Linux/-O2 selfhost segfault). */
static inline __int128 kai_i128_load(const KaiValue *v) {
    __int128 n;
    memcpy(&n, v->as.i128_halves, sizeof(n));
    return n;
}
static inline void kai_i128_store(KaiValue *v, __int128 n) {
    memcpy(v->as.i128_halves, &n, sizeof(n));
}

static KAI_RC_NOINLINE KaiValue *kai_int128(__int128 n) {
    KaiValue *v = kai_alloc(KAI_INT128);
    kai_i128_store(v, n);
    return v;
}

/* Parse a decimal / `0x` hex / `0b` binary integer literal (digits
 * only, optional leading `-`, `_` separators allowed) into __int128.
 * The compiler emits this for Int128 literals whose value may exceed
 * 64 bits — no C integer constant can hold them, so we build the value
 * at runtime. Overflow past 128 bits wraps (two's complement). */
static __int128 kai_i128_parse(const char *s) {
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    unsigned __int128 acc = 0;
    unsigned base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) { base = 2; s += 2; }
    for (; *s; s++) {
        char c = *s;
        if (c == '_') continue;
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned) (c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned) (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned) (c - 'A' + 10);
        else break;
        acc = acc * base + d;
    }
    return neg ? -(__int128) acc : (__int128) acc;
}

/* Render a signed 128-bit value to decimal (handles INT128_MIN). The
 * C library has no %lld for __int128, so we build the digits manually
 * into `buf` (max 40 chars: sign + 39 digits) and return the start. */
static char *kai_i128_to_decimal(__int128 v, char *buf, size_t buflen) {
    char *p = buf + buflen - 1;
    *p = '\0';
    unsigned __int128 mag = (v < 0) ? (unsigned __int128)(-(v + 1)) + 1u
                                    : (unsigned __int128) v;
    do { *--p = (char) ('0' + (int) (mag % 10)); mag /= 10; } while (mag != 0);
    if (v < 0) *--p = '-';
    return p;
}

/* FFI v2 (#417): box an opaque C handle. `p` is borrowed external
 * memory — RC frees only this box, never `p` (see kai_free_value's
 * default arm: no payload free for KAI_FOREIGN). */
static KAI_RC_NOINLINE KaiValue *kai_foreign(void *p) {
    KaiValue *v = kai_alloc(KAI_FOREIGN);
    v->as.foreign_ptr = p;
    v->variant_tag = 0;
    return v;
}


/* Unwrap an opaque handle back to its raw `void *` (a borrow — the box
 * keeps owning the cell; the pointer stays valid until the C resource
 * is destroyed by the driver). */
static inline void *kai_foreign_ptr(KaiValue *v) {
    return (v && v->tag == KAI_FOREIGN) ? v->as.foreign_ptr : NULL;
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
    v->as.s.bytes = (char *) kai_heap_malloc(len + 1);
    if (len > 0) memcpy(v->as.s.bytes, bytes, len);
    v->as.s.bytes[len] = '\0';
    return v;
}

#if KAI_HOST_JS
/* A JavaScript handle: `variant_tag` 1 marks a box whose death releases the
 * host's table slot named by the parked index. */

static KAI_RC_NOINLINE KaiValue *kai_foreign_js(void *p) {
    KaiValue *v = kai_foreign(p);
    v->variant_tag = 1;
    return v;
}

static KaiValue *kai_js_take_str(uint32_t n) {
    char *buf = (char *) malloc((size_t) n + 1);
    kai_js_take(buf, n);
    buf[n] = 0;
    KaiValue *v = kai_str_from_bytes(buf, n);
    free(buf);
    return v;
}
#endif

/* Lane FIX (top-3 leak sites): short-string interning.
 *
 * `core_table()` (DIAG rank #1) reallocates ~50 strings per call —
 * every call to `core_find` rebuilds the same `.rodata`-pointed
 * literals. The strings are content-deduped by the C compiler, so
 * repeat `kai_str("print")` calls receive the *same* `cstr` pointer.
 * Interning by pointer-then-content gives an immortal singleton per
 * literal, eliminating the per-call alloc/leak the same way
 * #300 / #304 did for nullary and immortal-payload variants.
 *
 * Two-stage lookup (open-addressed, 1024 buckets):
 *   1. Pointer match — hits 100% of compiler-emitted literal calls
 *      because identical literals share a single .rodata entry.
 *   2. Content match (≤ 64 bytes) — backstop for a repeated literal
 *      whose .rodata pointer differs across translation units.
 *
 * Misses (long strings, full table) fall through to
 * `kai_str_from_bytes` unchanged. Data-derived content must NOT come
 * through here — every distinct string inserted is pinned immortal, so
 * unbounded content grows the table without bound (use `kai_str_dyn`).
 *
 * Cached values carry `rc = INT32_MAX` so `kai_incref` / `kai_decref`
 * short-circuit; `kai_free_value` is never reached. */
#define KAI_STR_INTERN_BUCKETS 1024
#define KAI_STR_INTERN_MAXLEN  64
/* `cstr` is the bucket's publication flag as well as its key, so it is the
 * one field that must be atomic: an inserter fills `len` and `value` first
 * and RELEASES `cstr` last, a reader ACQUIRES `cstr` and only then reads the
 * other two. Without that order a reader can match a bucket whose `value` is
 * still NULL and hand a NULL string back to the program — on the path of
 * every string literal. */
typedef struct {
    _Atomic(const char *) cstr;  /* NULL ⇒ empty bucket. Owned via strdup. */
    size_t                len;
    KaiValue             *value;
} KaiStrInternBucket;
/* The intern table's whole purpose is pointer-identity: equal strings map to
 * one immortal value. A per-TU table would give each TU its own dedup set,
 * so the same literal interned in two TUs would yield distinct pointers and
 * defeat the interning. One shared table (owner defines, others extern).
 *
 * Shared and mutable, so the mutex is real: it serializes inserts (probe,
 * allocate, publish) against each other. Lookups stay lock-free — entries are
 * never removed or rewritten, so a probe that acquires a published `cstr`
 * sees a bucket that is already final. The mutex takes no other lock, so it
 * enters no ordering relation with the reactor / slot / mailbox locks. */
#if defined(KAI_SEPARATE_COMPILATION)
extern KaiStrInternBucket kai_str_intern_table[KAI_STR_INTERN_BUCKETS];
extern pthread_mutex_t    kai_str_intern_mu;
#  if defined(KAI_RUNTIME_OWNER)
KaiStrInternBucket kai_str_intern_table[KAI_STR_INTERN_BUCKETS];
pthread_mutex_t    kai_str_intern_mu = PTHREAD_MUTEX_INITIALIZER;
#  endif
#else
static KaiStrInternBucket kai_str_intern_table[KAI_STR_INTERN_BUCKETS];
static pthread_mutex_t    kai_str_intern_mu = PTHREAD_MUTEX_INITIALIZER;
#endif

/* FNV-1a over `len` bytes. The offset basis is not the standard FNV-64 one
 * (a digit short); `String` hashes, and so HashMap iteration order, depend on
 * it, so it must not change. */
static inline uint64_t kai_fnv1a64(const char *p, size_t len) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t) (uint8_t) p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static inline size_t kai_str_intern_hash(const char *cstr, size_t len) {
    return (size_t) (kai_fnv1a64(cstr, len) & (KAI_STR_INTERN_BUCKETS - 1));
}

/* Slow path: this probe found an empty bucket, so the string is (probably)
 * not interned yet. Re-probe under the mutex — a peer may have inserted the
 * same string, or filled the bucket we picked — then publish. */
static KaiValue *kai_str_intern_insert(const char *cstr, size_t len) {
    if (kai_nthreads > 1) pthread_mutex_lock(&kai_str_intern_mu);
    KaiValue *result = NULL;
    size_t i = kai_str_intern_hash(cstr, len);
    for (size_t probe = 0; probe < KAI_STR_INTERN_BUCKETS; probe++) {
        KaiStrInternBucket *b = &kai_str_intern_table[i];
        const char *bc = atomic_load_explicit(&b->cstr, memory_order_relaxed);
        if (bc == NULL) {
            KaiValue *v = kai_str_from_bytes(cstr, len);
            v->rc = INT32_MAX;
            char *owned = (char *) malloc(len + 1);
            if (owned) {
                memcpy(owned, cstr, len);
                owned[len] = '\0';
                b->len   = len;
                b->value = v;
                /* Publish last, with release: a reader that acquires this
                 * pointer is guaranteed to see the `len` / `value` above. */
                atomic_store_explicit(&b->cstr, (const char *) owned,
                                      memory_order_release);
            }
            /* Even if the copy failed, the value is still valid (just not
             * cacheable). Subsequent calls re-alloc but stay correct. */
            result = v;
            break;
        }
        if (b->len == len && (bc == cstr || memcmp(bc, cstr, len) == 0)) {
            result = b->value;   /* a peer interned it first */
            break;
        }
        i = (i + 1) & (KAI_STR_INTERN_BUCKETS - 1);
    }
    if (kai_nthreads > 1) pthread_mutex_unlock(&kai_str_intern_mu);
    /* Table full — fall back to non-cached. */
    return result ? result : kai_str_from_bytes(cstr, len);
}

KAI_RT_SHARED KAI_RC_NOINLINE KaiValue *kai_str(const char *cstr) {
    size_t len = strlen(cstr);
    if (len > KAI_STR_INTERN_MAXLEN) return kai_str_from_bytes(cstr, len);
    size_t i = kai_str_intern_hash(cstr, len);
    for (size_t probe = 0; probe < KAI_STR_INTERN_BUCKETS; probe++) {
        KaiStrInternBucket *b = &kai_str_intern_table[i];
        /* Acquire pairs with the release store in the insert path; entries are
         * never removed or rewritten, so a published bucket is already final
         * and the lock-free read needs nothing more. */
        const char *bc = atomic_load_explicit(&b->cstr, memory_order_acquire);
        if (bc == NULL) return kai_str_intern_insert(cstr, len);
        if (b->len == len && (bc == cstr || memcmp(bc, cstr, len) == 0)) {
            return b->value;
        }
        i = (i + 1) & (KAI_STR_INTERN_BUCKETS - 1);
    }
    /* Table full — fall back to non-cached. */
    return kai_str_from_bytes(cstr, len);
}

/* For data-derived C strings (number renders, paths, dir entries).
 * `kai_str` interns and pins its result immortal, which is only sound
 * for program-bounded literals — unbounded content would grow the
 * intern table one immortal cell per distinct value. */
static KaiValue *kai_str_dyn(const char *cstr) {
    return kai_str_from_bytes(cstr, strlen(cstr));
}

static KaiValue *kai_nil(void) { return &kai_singleton_nil; }

static KAI_RC_NOINLINE KaiValue *kai_cons(KaiValue *head, KaiValue *tail) {
    KaiValue *v = kai_alloc(KAI_CONS);
    v->as.cons.head = head;
    v->as.cons.tail = tail;
    return v;
}

