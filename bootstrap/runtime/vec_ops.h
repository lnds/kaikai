/* Fragment of runtime.h; include runtime.h, never this file. */
/* ---------- Vec raw scalar paths (compiler-fused) ----------
 *
 * Int / Real payloads move as bare 8-byte words, no box on either side.
 * A read guards shape and bounds, then loads; every miss lands in one
 * noreturn trap, so a loop over the read keeps a single countable exit.
 * A write touches the buffer only when it is a unique, unviewed owner of
 * the expected shape with room; every other case boxes the payload and
 * takes the general path, which owns the unique-or-copy decision. */

#define KAI_VEC_FAST static inline __attribute__((always_inline))
#define KAI_VEC_COUNT_INPLACE() \
    do { if (__builtin_expect(kai_rc_traffic_on, 0)) kai_vec_count_inplace(); } while (0)

/* A well-formed empty vec. A raw read points its loads here when the value
 * is not a vec node, so every load is unconditional and a loop hoists the
 * whole guard. */
static const KaiVecMeta kai_vec_null_meta = { 0 };
static const KaiValue kai_vec_null = {
    .tag = KAI_VEC, .as.vec = { 0, 0, (void *) &kai_vec_null_meta, NULL } };

KAI_VEC_FAST int kai_vec_is_node(KaiValue *v) {
    return v != NULL && !kai_is_value(v) && v->tag == KAI_VEC;
}

/* A trapping read that consumes its vec pays that reference first: the
 * unwind skips the release the caller planted after the read. */
__attribute__((noinline, cold, noreturn))
static void kai_vec_read_shape_trap(KaiValue *v, const char *shape_msg, int owned) {
    kai_vec_bounds(v, 0, "vec_get");
    if (owned) kai_decref(v);
    kai_trap_abort(shape_msg);
}

__attribute__((noinline, cold, noreturn))
static void kai_vec_read_index_trap(KaiValue *v, int64_t i, int owned) {
    if (owned && kai_vec_is_node(v)) kai_index_check_owned(v->as.vec.len, i, v, NULL);
    kai_vec_bounds(v, i, "vec_get");
    kai_trap_abort("vec: index out of range");
}

/* What a raw read needs from the vec, loaded with no branch: a loop
 * invariant the optimiser can hoist in one step. `ok` is false when `v`
 * is not a vec node, and the rest then describes the null vec. */
typedef struct {
    int               ok;
    int64_t           len;
    const KaiVecMeta *meta;
    const char       *elems;
} KaiVecRead;

KAI_VEC_FAST KaiVecRead kai_vec_read_open(KaiValue *v) {
    int is_ptr = (v != NULL) & !kai_is_value(v);
    const KaiValue *p = is_ptr ? v : &kai_vec_null;
    int ok = is_ptr & (p->tag == KAI_VEC);
    const KaiValue *q = ok ? p : &kai_vec_null;
    const KaiValue *view = q->as.vec.view_of;
    const KaiValue *own = view ? view : q;
    KaiVecRead r;
    r.ok = ok;
    r.len = q->as.vec.len;
    r.meta = (const KaiVecMeta *) own->as.vec.data;
    r.elems = (const char *) q->as.vec.data + (size_t) (view == NULL) * sizeof(KaiVecMeta);
    return r;
}

/* The guard of a raw read. The shape test comes first and depends on
 * nothing the loop changes; an empty vec passes it so an index into one
 * reports the range, not the shape. */
KAI_VEC_FAST void kai_vec_read_guard(KaiValue *v, const KaiVecRead *r, int shape_ok,
                                     int64_t i, int owned, const char *shape_msg) {
    if (__builtin_expect(!(r->ok & ((r->len == 0) | shape_ok)), 0))
        kai_vec_read_shape_trap(v, shape_msg, owned);
    if (__builtin_expect((uint64_t) i >= (uint64_t) r->len, 0))
        kai_vec_read_index_trap(v, i, owned);
}

/* Column `fidx` of element `i` in an inline-record vec whose static shape
 * is `n` fields with scalar `tag` at that column. */
KAI_VEC_FAST const char *kai_vec_rec_col(KaiValue *v, int64_t i, int32_t fidx,
                                         int32_t n, int tag, int owned) {
    KaiVecRead r = kai_vec_read_open(v);
    int shape_ok = (r.meta->ekind == KAI_VEC_EK_REC) & (r.meta->n_fields == n) &
                   (r.meta->field_tags[fidx] == tag);
    kai_vec_read_guard(v, &r, shape_ok, i, owned,
                       "vec: fused field read on a non-inline-record vec");
    return r.elems + (size_t) i * ((size_t) n * 8) + (size_t) fidx * 8;
}

/* Element `i` of a flat-scalar vec holding `tag` payloads. */
KAI_VEC_FAST const char *kai_vec_scalar_at(KaiValue *v, int64_t i, int tag, int owned) {
    KaiVecRead r = kai_vec_read_open(v);
    int shape_ok = (r.meta->ekind == KAI_VEC_EK_RAW) & (r.meta->elem_tag == tag);
    kai_vec_read_guard(v, &r, shape_ok, i, owned, "vec: element shape mismatch");
    return r.elems + (size_t) i * 8;
}

/* Column `fidx` of element `i` of an inline-record vec, boxed by the tag
 * the buffer holds for it. `owned` as in the raw reads: a trap pays `v`. */
static KaiValue *kai_vec_field_boxed(KaiValue *v, int64_t i, int32_t fidx, int owned) {
    if (__builtin_expect(!kai_vec_is_node(v) || (uint64_t) i >= (uint64_t) v->as.vec.len, 0))
        kai_vec_read_index_trap(v, i, owned);
    KaiVecMeta *m = kai_vec_meta(v);
    if (__builtin_expect(m->ekind != KAI_VEC_EK_REC || fidx < 0 || fidx >= m->n_fields, 0))
        kai_vec_read_shape_trap(v, "vec: fused field read on a non-inline-record vec", owned);
    int64_t bits;
    memcpy(&bits, kai_vec_elems(v) + (size_t) i * (size_t) m->stride + (size_t) fidx * 8, 8);
    return kai_vec_scalar_box(m->field_tags[fidx], bits);
}

/* The C backend's fused `vec_get(v, i).f`: consumes v and i. */
static KaiValue *kai_vec_get_field(KaiValue *v, KaiValue *i, int32_t fidx) {
    int64_t idx = (kai_is_int(i)) ? kai_intf(i) : 0;
    if (i) kai_decref(i);
    KaiValue *r = kai_vec_field_boxed(v, idx, fidx, 1);
    kai_decref(v);
    return r;
}

/* The borrowing form. `owned` marks a vec no binder holds: the read
 * releases it. */
static KaiValue *kai_vec_get_field_borrow(KaiValue *v, KaiValue *i, int32_t fidx, int owned) {
    KaiValue *r = kai_vec_field_boxed(v, (kai_is_int(i)) ? kai_intf(i) : 0, fidx, owned);
    if (owned) kai_decref(v);
    return r;
}

/* `owned` says the read consumes `v`; the release itself is the caller's. */
KAI_VEC_FAST int64_t kai_vec_field_i64(KaiValue *v, int64_t i, int32_t fidx, int32_t n,
                                       int owned) {
    int64_t x; memcpy(&x, kai_vec_rec_col(v, i, fidx, n, KAI_INT, owned), 8); return x;
}
KAI_VEC_FAST double kai_vec_field_f64(KaiValue *v, int64_t i, int32_t fidx, int32_t n,
                                      int owned) {
    double x; memcpy(&x, kai_vec_rec_col(v, i, fidx, n, KAI_REAL, owned), 8); return x;
}
KAI_VEC_FAST int64_t kai_vec_get_i64(KaiValue *v, int64_t i, int owned) {
    int64_t x; memcpy(&x, kai_vec_scalar_at(v, i, KAI_INT, owned), 8); return x;
}
KAI_VEC_FAST double kai_vec_get_f64(KaiValue *v, int64_t i, int owned) {
    double x; memcpy(&x, kai_vec_scalar_at(v, i, KAI_REAL, owned), 8); return x;
}

/* The compiler's static field kind, one char per field: 'r' = Real,
 * anything else = Int. */
KAI_VEC_FAST uint8_t kai_vec_kind_tag(char kind) {
    return kind == 'r' ? (uint8_t) KAI_REAL : (uint8_t) KAI_INT;
}

/* The block of a vec a raw write may touch in place, or NULL. An owner's
 * `data` is its block start, so the meta needs no view test here. */
KAI_VEC_FAST KaiVecMeta *kai_vec_inplace_meta(KaiValue *v) {
    if (!kai_vec_is_node(v) || v->as.vec.view_of || !kai_check_unique(v)) return NULL;
    return (KaiVecMeta *) v->as.vec.data;
}

KAI_VEC_FAST int kai_vec_rec_kinds_match(const KaiVecMeta *m, int64_t n,
                                         const char *kinds) {
    if (m->ekind != KAI_VEC_EK_REC || (int64_t) m->n_fields != n) return 0;
    for (int64_t k = 0; k < n; ++k)
        if (m->field_tags[k] != kai_vec_kind_tag(kinds[k])) return 0;
    return 1;
}

static void kai_vec_box_rec_bits(int64_t n, const int64_t *bits, const char *kinds,
                                 KaiValue **xs) {
    if (n < 1 || n > KAI_VEC_REC_MAX) kai_trap_abort("vec: record shape mismatch");
    for (int64_t k = 0; k < n; ++k)
        xs[k] = kai_vec_scalar_box(kai_vec_kind_tag(kinds[k]), bits[k]);
}

static KAI_RC_NOINLINE KaiValue *kai_vec_push_rec_bits_slow(
        KaiValue *v, int64_t n, const int64_t *bits, const char *kinds,
        const char **names) {
    KaiValue *xs[KAI_VEC_REC_MAX];
    kai_vec_box_rec_bits(n, bits, kinds, xs);
    return kai_vec_push_rec_raw(v, n, xs, names);
}

static KAI_RC_NOINLINE KaiValue *kai_vec_set_rec_bits_slow(
        KaiValue *v, int64_t i, int64_t n, const int64_t *bits, const char *kinds) {
    KaiValue *xs[KAI_VEC_REC_MAX];
    kai_vec_box_rec_bits(n, bits, kinds, xs);
    return kai_vec_set_rec_raw(v, kai_int(i), n, xs);
}

/* `vec_push(v, Rec{..})` over unpacked raw fields; consumes v. */
KAI_VEC_FAST KaiValue *kai_vec_push_rec_bits(KaiValue *v, int64_t n, const int64_t *bits,
                                             const char *kinds, const char **names) {
    KaiVecMeta *m = kai_vec_inplace_meta(v);
    if (__builtin_expect(m != NULL && kai_vec_rec_kinds_match(m, n, kinds), 1)) {
        int64_t len = v->as.vec.len;
        if (__builtin_expect(len < v->as.vec.cap, 1)) {
            memcpy((char *) (m + 1) + (size_t) len * ((size_t) n * 8), bits, (size_t) n * 8);
            v->as.vec.len = len + 1;
            KAI_VEC_COUNT_INPLACE();
            return v;
        }
    }
    return kai_vec_push_rec_bits_slow(v, n, bits, kinds, names);
}

/* `vec_set(v, i, Rec{..})` over unpacked raw fields; consumes v. */
KAI_VEC_FAST KaiValue *kai_vec_set_rec_bits(KaiValue *v, int64_t i, int64_t n,
                                            const int64_t *bits, const char *kinds) {
    KaiVecMeta *m = kai_vec_inplace_meta(v);
    if (__builtin_expect(m != NULL && kai_vec_rec_kinds_match(m, n, kinds) &&
                         (uint64_t) i < (uint64_t) v->as.vec.len, 1)) {
        memcpy((char *) (m + 1) + (size_t) i * ((size_t) n * 8), bits, (size_t) n * 8);
        KAI_VEC_COUNT_INPLACE();
        return v;
    }
    return kai_vec_set_rec_bits_slow(v, i, n, bits, kinds);
}

static KAI_RC_NOINLINE KaiValue *kai_vec_push_bits_slow(KaiValue *v, int64_t bits, int tag) {
    return kai_vec_push_impl(v, kai_vec_scalar_box((uint8_t) tag, bits));
}

static KAI_RC_NOINLINE KaiValue *kai_vec_set_bits_slow(KaiValue *v, int64_t i,
                                                       int64_t bits, int tag) {
    if (kai_vec_is_node(v)) kai_index_check_owned(v->as.vec.len, i, v, NULL);
    return kai_vec_set_impl(v, i, kai_vec_scalar_box((uint8_t) tag, bits));
}

/* `vec_push(v, x)` / `vec_set(v, i, x)` of a raw `tag` scalar; consume v. */
KAI_VEC_FAST KaiValue *kai_vec_push_bits(KaiValue *v, int64_t bits, int tag) {
    KaiVecMeta *m = kai_vec_inplace_meta(v);
    if (__builtin_expect(m != NULL && m->ekind == KAI_VEC_EK_RAW && m->elem_tag == tag, 1)) {
        int64_t len = v->as.vec.len;
        if (__builtin_expect(len < v->as.vec.cap, 1)) {
            memcpy((char *) (m + 1) + (size_t) len * 8, &bits, 8);
            v->as.vec.len = len + 1;
            KAI_VEC_COUNT_INPLACE();
            return v;
        }
    }
    return kai_vec_push_bits_slow(v, bits, tag);
}

KAI_VEC_FAST KaiValue *kai_vec_set_bits(KaiValue *v, int64_t i, int64_t bits, int tag) {
    KaiVecMeta *m = kai_vec_inplace_meta(v);
    if (__builtin_expect(m != NULL && m->ekind == KAI_VEC_EK_RAW && m->elem_tag == tag &&
                         (uint64_t) i < (uint64_t) v->as.vec.len, 1)) {
        memcpy((char *) (m + 1) + (size_t) i * 8, &bits, 8);
        KAI_VEC_COUNT_INPLACE();
        return v;
    }
    return kai_vec_set_bits_slow(v, i, bits, tag);
}

KAI_VEC_FAST int64_t kai_real_bits(double x) { int64_t b; memcpy(&b, &x, 8); return b; }

/* ---------- Vec slices: O(1) views over the shared buffer ----------
 *
 * A slice never copies elements: it is a fresh KAI_VEC node whose
 * `view_of` holds one strong ref on the owner and whose `data` points
 * at the first sliced element inside the owner's block. Consuming a
 * UNIQUE view retargets it in place (no allocation) — a `[h, ...t]`
 * descent re-slices the same node per level. An empty slice releases
 * the source entirely, so a descent's pin dies exactly at `[]`. */

static KaiValue *kai_vec_slice_impl(KaiValue *v, int64_t start, int64_t slen) {
    if (!v || kai_is_value(v) || v->tag != KAI_VEC) {
        kai_eprintf("kai: vec_slice: not a vec\n"); kai_exit(1);
    }
    int64_t len = v->as.vec.len;
    if (start < 0 || slen < 0 || start > len || slen > len - start) {
        static char buf[96];
        snprintf(buf, sizeof(buf), "slice [%lld, +%lld] out of range (len=%lld)",
                 (long long) start, (long long) slen, (long long) len);
        kai_trap_abort(buf);
    }
    if (slen == 0) { kai_decref(v); return kai_vec_empty(); }
    KaiVecMeta *m = kai_vec_meta(v);
    if (v->as.vec.view_of && kai_check_unique(v)) {
        v->as.vec.data = (char *) v->as.vec.data + (size_t) start * (size_t) m->stride;
        v->as.vec.len = slen;
        v->as.vec.cap = slen;
        kai_vec_count_inplace();
        return v;
    }
    KaiValue *owner = v->as.vec.view_of ? v->as.vec.view_of : v;
    KaiValue *s = kai_alloc(KAI_VEC);
    s->as.vec.len = slen;
    s->as.vec.cap = slen;
    s->as.vec.data = kai_vec_elems(v) + (size_t) start * (size_t) m->stride;
    s->as.vec.view_of = owner;
    /* A view source hands its owner-ref over via incref+release; an
     * owner source transfers the consumed ref directly. */
    if (v->as.vec.view_of) { kai_incref(owner); kai_decref(v); }
    return s;
}

/* `[h, ...t]`'s tail: everything from `start` on. */
static KaiValue *kai_vec_tail_from_impl(KaiValue *v, int64_t start) {
    if (!v || kai_is_value(v) || v->tag != KAI_VEC) {
        kai_eprintf("kai: vec_slice: not a vec\n"); kai_exit(1);
    }
    return kai_vec_slice_impl(v, start, v->as.vec.len - start);
}

/* Empty vec with `n` elements of capacity. Classification is still
 * PENDING (stride 0), so the block holds only the meta; the first push
 * classifies and grows the block to the reserved capacity in one
 * realloc. */
static KaiValue *kai_vec_reserve_impl(int64_t n) {
    return kai_vec_alloc(0, n < 0 ? 0 : n, NULL);
}

/* List -> vec, pre-sized to the list's length. Consumes `xs`. The
 * cursor covers a range (or a range tail) without materialising it. */
static KaiValue *kai_vec_from_list_impl(KaiValue *xs) {
    int64_t n = 0;
    KaiValue *p = xs;
    while (p && kai_is_ptr(p) && p->tag == KAI_CONS) { n++; p = p->as.cons.tail; }
    if (p && kai_is_ptr(p) && p->tag == KAI_RANGE && !kai_range_is_empty(p))
        n += kai_range_len(p);
    KaiValue *v = kai_vec_alloc(0, n, NULL);
    KaiSeqIt it;
    kai_seq_it_init(&it, xs);
    for (KaiValue *x = kai_seq_it_next(&it); x; x = kai_seq_it_next(&it))
        v = kai_vec_push_impl(v, x);
    kai_decref(xs);
    return v;
}

/* Structural equality — Vec is a value, unlike identity-compared
 * Array. Raw scalars compare by payload (Reals via `==`, preserving
 * NaN/-0.0 semantics); boxed elements recurse through kai_op_eq. */
KAI_RT_SHARED int kai_op_eq(KaiValue *a, KaiValue *b);
KAI_RT_SHARED int kai_vec_eq(KaiValue *a, KaiValue *b) {
    int64_t len = a->as.vec.len;
    if (len != b->as.vec.len) return 0;
    if (len == 0) return 1;
    KaiVecMeta *ma = kai_vec_meta(a);
    KaiVecMeta *mb = kai_vec_meta(b);
    if (ma->ekind != mb->ekind) return 0;   /* unreachable for one static type */
    const char *pa = kai_vec_elems(a);
    const char *pb = kai_vec_elems(b);
    if (ma->ekind == KAI_VEC_EK_BOXED) {
        for (int64_t i = 0; i < len; ++i) {
            KaiValue *xa, *xb;
            memcpy(&xa, pa + (size_t) i * sizeof(KaiValue *), sizeof(KaiValue *));
            memcpy(&xb, pb + (size_t) i * sizeof(KaiValue *), sizeof(KaiValue *));
            if (!kai_op_eq(xa, xb)) return 0;
        }
        return 1;
    }
    /* RAW / REC: word-compare, except Real columns via double ==. */
    int64_t words = (ma->stride / 8) * len;
    int has_real = (ma->ekind == KAI_VEC_EK_RAW && ma->elem_tag == KAI_REAL);
    if (ma->ekind == KAI_VEC_EK_REC) {
        for (int i = 0; i < ma->n_fields; ++i)
            if (ma->field_tags[i] == KAI_REAL) has_real = 1;
    }
    if (!has_real) return memcmp(pa, pb, (size_t) words * 8) == 0;
    for (int64_t w = 0; w < words; ++w) {
        int fld = (int) (w % (ma->stride / 8));
        uint8_t t = (ma->ekind == KAI_VEC_EK_RAW) ? (uint8_t) ma->elem_tag
                                                  : ma->field_tags[fld];
        if (t == KAI_REAL) {
            double ra, rb;
            memcpy(&ra, pa + (size_t) w * 8, 8);
            memcpy(&rb, pb + (size_t) w * 8, 8);
            if (!(ra == rb)) return 0;
        } else {
            if (memcmp(pa + (size_t) w * 8, pb + (size_t) w * 8, 8) != 0) return 0;
        }
    }
    return 1;
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
        v->as.clo.captures = n_captures == 1 ? &v->as.clo.lone : kai_caps_alloc(n_captures);
        for (int i = 0; i < n_captures; ++i) v->as.clo.captures[i] = kai_incref(captures[i]);
    } else {
        v->as.clo.captures = NULL;
    }
    return v;
}

/* The one immortal closure of a capture-free fn, built on first reference and
 * published through `slot` and never freed, so the ledger retires it. A
 * racing builder's cell was never visible, so its rc is restored before the
 * free. */
static KAI_RC_NOINLINE KaiValue *kai_static_closure_build(KaiValue **slot, KaiFn fn, int arity) {
    KaiValue *c = kai_closure(fn, arity, 0, NULL);
    c->rc = INT32_MAX;
    KaiValue *expected = NULL;
    if (__atomic_compare_exchange_n(slot, &expected, c, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        kai_rc_count_free();
        return c;
    }
    c->rc = 1;
    kai_decref(c);
    return expected;
}

static inline KaiValue *kai_static_closure(KaiValue **slot, KaiFn fn, int arity) {
    KaiValue *c = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    return c != NULL ? c : kai_static_closure_build(slot, fn, arity);
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
 * pointer is freed before the result hits the caller). */
static KaiValue *kai_apply(KaiValue *clo, int argc, KaiValue **argv) {
    if (!clo || clo->tag != KAI_CLOSURE) {
        kai_eprintf("kai: attempted to call a non-callable value\n");
        kai_exit(1);
    }
    KaiValue *r = clo->as.clo.fn(clo, argv, argc);
    kai_decref(clo);
    return r;
}

/* Borrowing indirect call (issue #1130). Invokes the closure WITHOUT
 * consuming it: the caller retains the ref and drops it after its last
 * use. This is the call variant a `^`-borrowed function-typed parameter
 * routes to — the closure threaded through a tail loop pays no per-call
 * incref/decref pair, only the entry dup and one exit drop. Args keep the
 * owned convention (the body consumes them exactly as under kai_apply);
 * only the closure is borrowed. */
static KaiValue *kai_apply_borrow(KaiValue *clo, int argc, KaiValue **argv) {
    if (!clo || clo->tag != KAI_CLOSURE) {
        kai_eprintf("kai: attempted to call a non-callable value\n");
        kai_exit(1);
    }
    return clo->as.clo.fn(clo, argv, argc);
}

/* ---------- field access helpers ---------- */

static KaiValue *kai_op_field(KaiValue *rec, const char *name) {
    if (!rec || rec->tag != KAI_RECORD) {
        kai_eprintf("kai: field access on non-record\n"); kai_exit(1);
    }
    for (int i = 0; i < rec->as.rec.n_fields; ++i) {
        if (strcmp(rec->as.rec.names[i], name) == 0) {
            return kai_incref(rec->as.rec.fields[i]);
        }
    }
    kai_eprintf("kai: no such field `%s`\n", name); kai_exit(1);
}

/* Field read for an arm that owns its record: a unique cell hands the field
   over and keeps the unit immediate in its slot, so the binder is the sole
   owner and a write through it is in place; a shared cell increfs as
   kai_op_field does. Only an unguarded arm may use it, since a rejected arm
   would find the slot emptied. */
static KaiValue *kai_op_field_take(KaiValue *rec, const char *name) {
    if (!rec || rec->tag != KAI_RECORD) {
        kai_eprintf("kai: field access on non-record\n"); kai_exit(1);
    }
    for (int i = 0; i < rec->as.rec.n_fields; ++i) {
        if (strcmp(rec->as.rec.names[i], name) == 0) {
            KaiValue *f = rec->as.rec.fields[i];
            if (kai_check_unique(rec)) { rec->as.rec.fields[i] = kai_unit(); return f; }
            return kai_incref(f);
        }
    }
    kai_eprintf("kai: no such field `%s`\n", name); kai_exit(1);
}

/* After a guard passed: when the cell is held only by its owner, its own
   reference to a field the arm binds moves to that binder, the slot left
   unit. The binder's reference to the same value stays. */
static void kai_op_field_untake(KaiValue *rec, const char *name) {
    if (!kai_check_unique(rec) || rec->tag != KAI_RECORD) return;
    for (int i = 0; i < rec->as.rec.n_fields; ++i) {
        if (strcmp(rec->as.rec.names[i], name) == 0) {
            KaiValue *old = rec->as.rec.fields[i];
            rec->as.rec.fields[i] = kai_unit();
            kai_decref(old);
            return;
        }
    }
}

/* Field read of a dup'd base at slot `i`: when the base is held only by its
   owner and this dup, the field is moved out of the cell and the slot left
   unit; the dup is consumed either way. */
static inline KaiValue *kai_op_field_take_dup_at(KaiValue *rec, int i) {
    if (!rec || rec->tag != KAI_RECORD) {
        kai_eprintf("kai: field access on non-record\n"); kai_exit(1);
    }
    KaiValue *f = rec->as.rec.fields[i];
    if (!kai_is_value(rec) && kai_rc_load(rec) == 2) {
        rec->as.rec.fields[i] = kai_unit();
        kai_decref(rec);
        return f;
    }
    f = kai_incref(f);
    kai_decref(rec);
    return f;
}

/* Constant-index field read. Same RC contract as kai_op_field (increfs the
   read field) but skips the strcmp: the emitter computes the slot from the
   static record type when known, relying on the desugar-canonicalised
   declaration-order layout. */
static KaiValue *kai_op_field_at(KaiValue *rec, int i) {
    if (!rec || rec->tag != KAI_RECORD) {
        kai_eprintf("kai: field access on non-record\n"); kai_exit(1);
    }
    return kai_incref(rec->as.rec.fields[i]);
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
        kai_eprintf("kai: field access on non-record\n"); kai_exit(1);
    }
    for (int i = 0; i < rec->as.rec.n_fields; ++i) {
        if (strcmp(rec->as.rec.names[i], name) == 0) {
            return rec->as.rec.fields[i];
        }
    }
    kai_eprintf("kai: no such field `%s`\n", name); kai_exit(1);
}

/* ---------- equality ---------- */

KAI_RT_SHARED int kai_op_eq(KaiValue *a, KaiValue *b) {
    /* identical word — covers two equal immediates. A NaN is unequal to itself,
     * so a Real and every container that can hold one compare structurally. */
    if (a == b) {
        if (!a || kai_is_value(a)) return 1;
        switch ((KaiTag) a->tag) {
            case KAI_REAL: case KAI_CONS: case KAI_VARIANT: case KAI_RECORD: case KAI_VEC: case KAI_ARRAY: break;
            default: return 1;
        }
    }
    /* Immediate Int: equal iff both are Ints with the same value. An
     * immediate vs a heap value of any other type is unequal. Done
     * before any header deref, since an immediate has no header. */
    if (kai_is_value(a) || kai_is_value(b)) {
        return kai_is_int(a) && kai_is_int(b) && kai_intf(a) == kai_intf(b);
    }
    if (!a || !b) return 0;
    /* A range is a list in another representation — list equality must
     * compare elements across representations, never tags. The cursor
     * walk allocates nothing and leaves both sides untouched. */
    if (a->tag == KAI_RANGE || b->tag == KAI_RANGE) {
        int a_seq = a->tag == KAI_RANGE || a->tag == KAI_CONS || a->tag == KAI_NIL;
        int b_seq = b->tag == KAI_RANGE || b->tag == KAI_CONS || b->tag == KAI_NIL;
        if (!a_seq || !b_seq) return 0;
        KaiSeqIt ia, ib;
        kai_seq_it_init(&ia, a);
        kai_seq_it_init(&ib, b);
        for (;;) {
            KaiValue *xa = kai_seq_it_next(&ia);
            KaiValue *xb = kai_seq_it_next(&ib);
            if (!xa || !xb) {
                kai_decref(xa);
                kai_decref(xb);
                return xa == xb;
            }
            int eq = kai_op_eq(xa, xb);
            kai_decref(xa);
            kai_decref(xb);
            if (!eq) return 0;
        }
    }
    if (a->tag != b->tag) return 0;
    switch ((KaiTag) a->tag) {
        case KAI_UNIT: return 1;
        case KAI_BOOL: return a->as.b == b->as.b;
        case KAI_INT:  return a->as.i == kai_intf(b);
        case KAI_REAL: return a->as.r == b->as.r;
        case KAI_CHAR: return a->as.c == b->as.c;
        case KAI_STR:  return a->as.s.len == b->as.s.len &&
                              memcmp(a->as.s.bytes, b->as.s.bytes, a->as.s.len) == 0;
        case KAI_NIL:  return 1;
        case KAI_CONS:
            /* the spine is a loop: recursing per element overflows a fiber stack */
            for (;;) {
                if (!kai_op_eq(a->as.cons.head, b->as.cons.head)) return 0;
                a = a->as.cons.tail;
                b = b->as.cons.tail;
                if (kai_is_value(a) || kai_is_value(b) || !a || !b ||
                    a->tag != KAI_CONS || b->tag != KAI_CONS)
                    return kai_op_eq(a, b);
            }
        case KAI_RANGE: return 1;   /* unreachable: the cursor walk above owns ranges */
        case KAI_VARIANT: {
            /* Eq dispatch: if a's head type has a custom impl Eq, route to it
             * before structural fallback. Covers root AND nested cases (this
             * fires on every recursive descent into a field of this kind).
             * kai_op_eq is NON-consuming, so we incref a/b for the impl (which
             * consumes them) and do NOT decref here. */
            int32_t _eq_head = kai_head_tag(a);
            void *_eq_fn = kai_eq_impl_of(_eq_head);
            if (_eq_fn) {
                kai_incref(a); kai_incref(b);
                KaiValue *_eq_r = ((KaiValue *(*)(KaiValue *, KaiValue *)) _eq_fn)(a, b);
                int _eq_res = kai_op_truthy(_eq_r);
                kai_decref(_eq_r);
                return _eq_res;
            }
            if (a->variant_tag != b->variant_tag) return 0;
            if (a->var_n_args != b->var_n_args) return 0;
            /* Phase 2: if either cell carries primitive slots, the
             * masks must agree slot-by-slot (a variant of the same
             * tag/name must have the same layout). Then compare each
             * slot by its kind: pointer slots recurse via kai_op_eq,
             * primitive slots compare raw scalars. The mask==0 hot
             * path stays on the original pointer-by-pointer walk. */
            if (kai_slot_mask_of(a->variant_tag) == 0 && kai_slot_mask_of(b->variant_tag) == 0) {
                for (int i = 0; i < a->var_n_args; ++i) {
                    if (!kai_op_eq(kai_var_slots(a)[i].ptr, kai_var_slots(b)[i].ptr)) return 0;
                }
            } else {
                if (kai_slot_mask_of(a->variant_tag) != kai_slot_mask_of(b->variant_tag)) return 0;
                for (int i = 0; i < a->var_n_args; ++i) {
                    uint32_t k = kai_var_slot_kind(kai_slot_mask_of(a->variant_tag), i);
                    if (k == KAI_VAR_SLOT_PTR) {
                        if (!kai_op_eq(kai_var_slots(a)[i].ptr, kai_var_slots(b)[i].ptr)) return 0;
                    } else if (k == KAI_VAR_SLOT_INT || k == KAI_VAR_SLOT_ENUM) {
                        /* ENUM compares its immediate variant_tag like a raw
                         * Int; two enum slots are equal iff same tag. */
                        if (kai_var_slots(a)[i].i64 != kai_var_slots(b)[i].i64) return 0;
                    } else if (k == KAI_VAR_SLOT_REAL) {
                        if (kai_var_slots(a)[i].r != kai_var_slots(b)[i].r) return 0;
                    }
                }
            }
            return 1;
        }
        case KAI_RECORD: {
            /* Eq dispatch for records carrying a custom impl Eq. Records only
             * get a dispatchable head_type_tag when stamped at construction
             * (see LIMITATION in the handoff: nested record fields do not get
             * a tag today). Root records reached via the resolver still work;
             * this hook fires only when as.rec.head_type_tag is a real impl. */
            int32_t _eq_head = kai_head_tag(a);
            void *_eq_fn = kai_eq_impl_of(_eq_head);
            if (_eq_fn) {
                kai_incref(a); kai_incref(b);
                KaiValue *_eq_r = ((KaiValue *(*)(KaiValue *, KaiValue *)) _eq_fn)(a, b);
                int _eq_res = kai_op_truthy(_eq_r);
                kai_decref(_eq_r);
                return _eq_res;
            }
            if (a->as.rec.n_fields != b->as.rec.n_fields) return 0;
            for (int i = 0; i < a->as.rec.n_fields; ++i) {
                if (!kai_op_eq(a->as.rec.fields[i], b->as.rec.fields[i])) return 0;
            }
            return 1;
        }
        case KAI_CLOSURE: return 0;      /* closures are not equatable */
        case KAI_ARRAY: {                /* contents; `array_same` is identity */
            if (a->as.arr.len != b->as.arr.len) return 0;
            for (int64_t i = 0; i < a->as.arr.len; ++i) {
                if (!kai_op_eq(a->as.arr.items[i], b->as.arr.items[i])) return 0;
            }
            return 1;
        }
        case KAI_VEC:     return kai_vec_eq(a, b);  /* value: structural */
        case KAI_REF:     return 0;      /* refs are identity-compared; a==b handled above */
        case KAI_FIBER:   return a->as.fib == b->as.fib;  /* identity */
        case KAI_PID:     return a->as.mb  == b->as.mb;   /* identity */
        case KAI_FOREIGN: return a->as.foreign_ptr == b->as.foreign_ptr; /* identity (#417) */
        case KAI_CONT:    return a == b;
        case KAI_SEQ:     return a == b;
        case KAI_BYTE:      return a->as.byte_val == b->as.byte_val;    /* Lane 4 (#473) */
        case KAI_INT32:     return a->as.i32 == b->as.i32;
        case KAI_UINT32:    return a->as.u32 == b->as.u32;
        case KAI_UINT64:    return a->as.u64 == b->as.u64;
        case KAI_INT128:    return kai_i128_load(a) == kai_i128_load(b);
    }
    return 0;
}

