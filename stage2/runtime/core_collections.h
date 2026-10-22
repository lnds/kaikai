/* Fragment of runtime.h; include runtime.h, never this file. */
/* ---------- core: arrays ---------- */

static KaiValue *kai_core_array_make(KaiValue *n, KaiValue *init) {
    int64_t len = (kai_is_int(n)) ? kai_intf(n) : 0;
    /* impl increfs `init` once per slot; consume our own input
     * refs (n and init) at the boundary. */
    KaiValue *r = kai_array_make(len, init);
    if (n) kai_decref(n);
    if (init) kai_decref(init);
    return r;
}

/* Length-0 array of any element type. `kai_array_make(0, NULL)` is
   sound only because the fill loop never runs at len 0 — NULL is
   never dereferenced. */
static KaiValue *kai_core_array_empty(void) {
    return kai_array_make(0, NULL);
}

static KaiValue *kai_core_array_length(KaiValue *a) {
    int64_t len = (kai_is_ptr(a) && a->tag == KAI_ARRAY) ? a->as.arr.len : 0;
    KaiValue *r = kai_int(len);
    if (a) kai_decref(a);
    return r;
}

/* Identity: whether both handles are the one array. */
static KaiValue *kai_core_array_same(KaiValue *a, KaiValue *b) {
    KaiValue *r = kai_bool(a == b);
    if (a) kai_decref(a);
    if (b) kai_decref(b);
    return r;
}

/* Borrow variant (issue #1120): read the length without consuming `a`. */
static KaiValue *kai_core_array_length_borrow(KaiValue *a) {
    int64_t len = (kai_is_ptr(a) && a->tag == KAI_ARRAY) ? a->as.arr.len : 0;
    return kai_int(len);
}

/* An out-of-range index on a call that consumes its container (and the
 * value it writes): the trap's unwind skips the call's own release, so
 * the trap pays it first. */
static void kai_index_check_owned(int64_t len, int64_t i, KaiValue *c, KaiValue *x) {
    if (i >= 0 && i < len) return;
    static char buf[96];
    snprintf(buf, sizeof(buf), "index %lld out of range (len=%lld)",
             (long long) i, (long long) len);
    kai_decref(c);
    if (x) kai_decref(x);
    kai_trap_abort(buf);
}

static KaiValue *kai_core_array_get(KaiValue *a, KaiValue *i) {
    int64_t idx = (kai_is_int(i)) ? kai_intf(i) : 0;
    if (kai_is_ptr(a) && a->tag == KAI_ARRAY) kai_index_check_owned(a->as.arr.len, idx, a, NULL);
    KaiValue *r = kai_array_get_impl(a, idx);
    if (a) kai_decref(a);
    if (i) kai_decref(i);
    return r;
}

/* Borrow variant (issue #1120): the container is BORROWED, not consumed —
   the caller kept its ref (Perceus stripped the dup), so we must NOT decref
   `a`. The element still leaves the array with +1 (it escapes to the
   caller). `i` is a tagged Int and is not rc-touched. */
static KaiValue *kai_core_array_get_borrow(KaiValue *a, KaiValue *i) {
    int64_t idx = (kai_is_int(i)) ? kai_intf(i) : 0;
    return kai_array_get_impl(a, idx);
}

/* The element leaves with the slot's own reference and the slot holds
 * unit until the write the compiler pairs this read with. */
static KaiValue *kai_core_array_take(KaiValue *a, KaiValue *i) {
    int64_t idx = kai_intf(i);
    KaiValue *r = a->as.arr.items[idx];
    a->as.arr.items[idx] = kai_unit();
    kai_decref(a);
    kai_decref(i);
    return r;
}

static KaiValue *kai_core_array_set(KaiValue *a, KaiValue *i, KaiValue *v) {
    int64_t idx = (kai_is_int(i)) ? kai_intf(i) : 0;
    if (kai_is_ptr(a) && a->tag == KAI_ARRAY) kai_index_check_owned(a->as.arr.len, idx, a, v);
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

static KaiValue *kai_core_array_grow(KaiValue *a, KaiValue *n, KaiValue *init) {
    int64_t new_len = (kai_is_int(n)) ? kai_intf(n) : 0;
    /* impl returns kai_incref(a) — caller owns the new ref. The
     * input `a` ref is consumed under the callee-consumes
     * convention. */
    KaiValue *r = kai_array_grow_impl(a, new_len, init);
    if (a) kai_decref(a);
    if (n) kai_decref(n);
    if (init) kai_decref(init);
    return r;
}

/* ---------- core: Vec (pure value vector) ----------
 *
 * Callee-consumes at the boundary, like every core fn. The impls
 * for set/push take the vec ref itself as the ownership transfer:
 * a last-use caller arrives with rc == 1 (in-place is unobservable),
 * a caller that kept the vec arrives with rc > 1 (copy-on-write). */

static KaiValue *kai_core_vec_make(KaiValue *n, KaiValue *init) {
    int64_t len = (kai_is_int(n)) ? kai_intf(n) : 0;
    KaiValue *r = kai_vec_make(len, init);
    if (n) kai_decref(n);
    if (init) kai_decref(init);
    return r;
}

static KaiValue *kai_core_vec_empty(void) {
    return kai_vec_empty();
}

static KaiValue *kai_core_vec_length(KaiValue *v) {
    int64_t len = (v && kai_is_ptr(v) && v->tag == KAI_VEC) ? v->as.vec.len : 0;
    KaiValue *r = kai_int(len);
    if (v) kai_decref(v);
    return r;
}

static KaiValue *kai_core_vec_get(KaiValue *v, KaiValue *i) {
    int64_t idx = (kai_is_int(i)) ? kai_intf(i) : 0;
    if (kai_is_ptr(v) && v->tag == KAI_VEC) kai_index_check_owned(v->as.vec.len, idx, v, NULL);
    KaiValue *r = kai_vec_get_impl(v, idx);
    if (v) kai_decref(v);
    if (i) kai_decref(i);
    return r;
}

static KaiValue *kai_core_vec_set(KaiValue *v, KaiValue *i, KaiValue *x) {
    int64_t idx = (kai_is_int(i)) ? kai_intf(i) : 0;
    if (kai_is_ptr(v) && v->tag == KAI_VEC) kai_index_check_owned(v->as.vec.len, idx, v, x);
    if (i) kai_decref(i);
    return kai_vec_set_impl(v, idx, x);   /* consumes v and x */
}

static KaiValue *kai_core_vec_push(KaiValue *v, KaiValue *x) {
    return kai_vec_push_impl(v, x);       /* consumes v and x */
}

/* Borrow variants (the array_get_borrow move): the container is
 * BORROWED — the caller kept its ref (Perceus stripped the dup), so
 * no decref here. `i` is a tagged Int and is not rc-touched. */
static KaiValue *kai_core_vec_length_borrow(KaiValue *v) {
    int64_t len = (v && kai_is_ptr(v) && v->tag == KAI_VEC) ? v->as.vec.len : 0;
    return kai_int(len);
}

static KaiValue *kai_core_vec_get_borrow(KaiValue *v, KaiValue *i) {
    int64_t idx = (kai_is_int(i)) ? kai_intf(i) : 0;
    return kai_vec_get_impl(v, idx);
}

static KaiValue *kai_core_vec_slice(KaiValue *v, KaiValue *start, KaiValue *n) {
    int64_t s = (kai_is_int(start)) ? kai_intf(start) : 0;
    int64_t sl = (kai_is_int(n)) ? kai_intf(n) : 0;
    if (start) kai_decref(start);
    if (n) kai_decref(n);
    return kai_vec_slice_impl(v, s, sl);      /* consumes v */
}

static KaiValue *kai_core_vec_tail_from(KaiValue *v, KaiValue *start) {
    int64_t s = (kai_is_int(start)) ? kai_intf(start) : 0;
    if (start) kai_decref(start);
    return kai_vec_tail_from_impl(v, s);      /* consumes v */
}

static KaiValue *kai_core_vec_reserve(KaiValue *n) {
    int64_t cap = (kai_is_int(n)) ? kai_intf(n) : 0;
    if (n) kai_decref(n);
    return kai_vec_reserve_impl(cap);
}

static KaiValue *kai_core_vec_from_list(KaiValue *xs) {
    return kai_vec_from_list_impl(xs);        /* consumes xs */
}

/* ---------- core: refs (issue #257) ----------
 *
 * `Ref[T]` is a single-cell mutable reference in the `Mutable`
 * effect (docs/effects-stdlib.md §Mutable). Surface ops:
 *   ref_make[T](init: T)        : Ref[T]
 *   ref_get[T](r: Ref[T])       : T
 *   ref_set[T](r: Ref[T], v: T) : Unit
 *
 * Runtime layout: a Ref is a KAI_REF — a single mutable cell with its
 * own tag (one `KaiValue *cell` field). Distinct from KAI_ARRAY: no
 * length, no capacity, no indexing — the "a Ref is exactly one slot"
 * invariant lives in the representation, not just the surface type.
 * (The earlier length-1 KAI_ARRAY hack from #257 is replaced here.)
 * RC: a Ref owns one strong reference to its cell. All three ops obey
 * the callee-consumes convention. */
static KaiValue *kai_core_ref_make(KaiValue *init) {
    /* Steal `init` straight into the cell — no incref/decref dance
     * (the array-backed version did one of each; this is strictly less
     * RC work, which matters on the hot path of Front A). */
    KaiValue *r = kai_alloc(KAI_REF);
    r->as.ref.cell = init;
    return r;
}

static KaiValue *kai_core_ref_get(KaiValue *r) {
    /* Hand back a fresh strong reference to the cell; consume `r`. */
    KaiValue *v = r ? kai_incref(r->as.ref.cell) : NULL;
    if (r) kai_decref(r);
    return v;
}

static KaiValue *kai_core_ref_set(KaiValue *r, KaiValue *v) {
    /* Drop the old contents, steal `v` into the cell; consume `r`.
     * Surface contract is Unit-typed. No bounds check, no index. */
    if (r) {
        kai_decref(r->as.ref.cell);
        r->as.ref.cell = v;       /* steals v */
        kai_decref(r);
    } else if (v) {
        kai_decref(v);
    }
    return kai_unit();
}

/* ---------- core: lists ---------- */

static KaiValue *kai_core_list_length(KaiValue *xs) {
    int64_t n = 0;
    KaiValue *p = xs;
    while (kai_is_ptr(p) && p->tag == KAI_CONS) { n++; p = p->as.cons.tail; }
    if (kai_is_ptr(p) && p->tag == KAI_RANGE && !kai_range_is_empty(p))
        n += kai_range_len(p);
    KaiValue *r = kai_int(n);
    if (xs) kai_decref(xs);
    return r;
}

/* Borrows `xs` (does NOT decref it); the public wrapper decrefs
 * `xs`/`ys` after the chain is built. Iterative forward build through
 * a tail slot: one pass, one cell per element, no recursion (a deep
 * spine must not consume C stack) — and the cursor makes a range
 * prefix work without materialising `xs` itself. */
static KaiValue *kai_list_append_borrow(KaiValue *xs, KaiValue *ys) {
    KaiValue *head = NULL;
    KaiValue **slot = &head;
    KaiSeqIt it;
    kai_seq_it_init(&it, xs);
    for (KaiValue *x = kai_seq_it_next(&it); x; x = kai_seq_it_next(&it)) {
        KaiValue *cell = kai_cons(x, NULL);
        *slot = cell;
        slot = &cell->as.cons.tail;
    }
    *slot = kai_incref(ys);
    return head;
}

static KaiValue *kai_core_list_append(KaiValue *xs, KaiValue *ys) {
    KaiValue *r = kai_list_append_borrow(xs, ys);
    if (xs) kai_decref(xs);
    if (ys) kai_decref(ys);
    return r;
}

static KaiValue *kai_core_list_reverse(KaiValue *xs) {
    /* A pure range reverses in O(1): same elements walked from the
     * last actually-generated one back to `from` with the step
     * negated. */
    if (kai_is_ptr(xs) && xs->tag == KAI_RANGE) {
        KaiValue *r = kai_range_is_empty(xs)
            ? kai_nil()
            : kai_range_new(kai_range_last(xs), xs->as.range.from,
                            (int64_t) (0 - (uint64_t) xs->as.range.step));
        kai_decref(xs);
        return r;
    }
    KaiValue *acc = kai_nil();
    KaiSeqIt it;
    kai_seq_it_init(&it, xs);
    for (KaiValue *x = kai_seq_it_next(&it); x; x = kai_seq_it_next(&it))
        acc = kai_cons(x, acc);
    if (xs) kai_decref(xs);
    return acc;
}

/* ---------- core: higher order ---------- */
/*
 * Iterative refactor (Perceus Tier 2 part 3, 2026-04-29). Pre-flip
 * these were recursive over `xs->as.cons.tail`, which forced the
 * helpers to *borrow* `xs` (the recursion holds aliased pointers
 * to every cell while the closure runs). Now they walk iteratively
 * with `KaiValue *p = xs; p = p->as.cons.tail;` so the entire
 * cons-chain stays alive under `xs`'s single reference until the
 * helper exits, and we can decref `xs` once at the end.
 *
 * Every arg slot holds an OWNED reference that the callee consumes
 * (the closure body is perceus-compiled and decrefs each arg through
 * normal use). The helpers give the closure its own reference of each
 * element while the cons cell stays alive under `xs`, and lend it `f`,
 * which they release once, post-loop.
 *
 * `_map` / `_filter` build their result reversed (each iteration
 * cons-prepends) and call `kai_core_list_reverse` once to
 * restore order. `list_reverse` consumes its arg, so the
 * intermediate reversed list is freed in the same step that
 * produces the final list — no extra retention.
 */

/* The callback may exit non-locally, past this frame: what the frame
 * owns across it sits in `held`, registered on the unwind stack once per
 * call, and the callback borrows `f`. A slot whose reference has moved
 * on holds NULL. */
static inline KaiFiber *kai_unw_push(KaiValue **base, intptr_t n);
static inline void kai_unw_pop(KaiFiber *f);

/* ---------- Seq: the lazy cell ---------- */

static KaiValue *kai_seq_defer(KaiValue *thunk) {
    KaiValue *s = kai_alloc(KAI_SEQ);
    s->as.seq.slot = thunk;
    return s;
}

/* An exit past a running cell: the cell holds its thunk again, and the
 * reference of the frame that was evaluating it goes. */
static void kai_seq_abandon(KaiValue *s) {
    s->variant_tag = s->variant_tag == KAI_SEQ_FILTERING ? KAI_SEQ_FILTER : KAI_SEQ_THUNK;
    kai_decref(s);
}

/* The step of an evaluated cell; consumes `s`. */
static inline KaiValue *kai_seq_take(KaiValue *s) {
    KaiValue *step = s->as.seq.slot;
    if (kai_check_unique(s)) s->as.seq.slot = NULL;
    else kai_incref(step);
    kai_decref(s);
    return step;
}

KAI_SCHED_FN KaiValue *kai_seq_apply_far(KaiValue *thunk);

/* A stage reads the one before it from inside its own read, so a chain of
 * stages nests one activation each: past the middle of the running stack a
 * read carries on over a fresh segment. The half kept free is what a trap
 * raised inside the chain unwinds on. Depth is counted from the entry frame
 * of the running stack; the main fiber records none and is never deep. */
static inline int kai_seq_deep(KaiFiber *f) {
    uintptr_t entry = f->unwind_frame;
    return entry != 0 && (size_t) (entry - (uintptr_t) __builtin_frame_address(0)) > f->stack_size / 2;
}

/* Apply a cell's thunk. */
static inline KaiValue *kai_seq_apply(KaiFiber *f, KaiValue *thunk) {
    if (kai_seq_deep(f)) return kai_seq_apply_far(thunk);
    return kai_apply_borrow(thunk, 0, NULL);
}

/* Run the thunk of `s`. An unshared cell has no other reader, so it is
 * freed before the thunk runs and the step is never written. A shared one
 * is marked while it runs and then holds the step in place of the thunk;
 * the thunk is released only after, since dropping it can run a `finally`
 * that reads the cell. */
__attribute__((noinline))
static KaiValue *kai_seq_run(KaiValue *s) {
    if (s->variant_tag == KAI_SEQ_RUNNING) {
        kai_decref(s);
        return kai_core_panic(kai_str("sequence depends on itself"));
    }
    KaiValue *thunk = s->as.seq.slot;
    if (kai_check_unique(s)) {
        s->as.seq.slot = NULL;
        kai_decref(s);
        KaiValue *held[1] = { thunk };
        KaiFiber *uf = kai_unw_push(held, 1);
        KaiValue *step = kai_seq_apply(uf, thunk);
        kai_unw_pop(uf);
        kai_decref(thunk);
        return step;
    }
    s->variant_tag = KAI_SEQ_RUNNING;
    KaiFiber *uf = kai_unw_push((KaiValue **) s, KAI_UNW_SEQ);
    KaiValue *step = kai_seq_apply(uf, thunk);
    kai_unw_pop(uf);
    s->as.seq.slot = step;
    s->variant_tag = KAI_SEQ_DONE;
    kai_decref(thunk);
    return kai_seq_take(s);
}

static KaiValue *kai_seq_filter_run(KaiValue *c);

/* Read a cell: its step, computed at most once; consumes `s`. */
static KaiValue *kai_seq_force(KaiValue *s) {
    if (s->variant_tag == KAI_SEQ_DONE) return kai_seq_take(s);
    if (s->variant_tag >= KAI_SEQ_FILTER) return kai_seq_filter_run(s);
    return kai_seq_run(s);
}

/* A filter stage: the cell holds its source and its predicate where
 * another holds a thunk. Consumes both. */
static KaiValue *kai_seq_filter(KaiValue *s, KaiValue *p) {
    KaiValue *c = kai_alloc(KAI_SEQ);
    c->as.seq.slot = s;
    c->as.seq.pred = p;
    c->variant_tag = KAI_SEQ_FILTER;
    return c;
}

/* The predicates of a filter stage: one closure, or an array of them in
 * the order they apply once stages have merged. */
static inline int64_t kai_seq_preds_n(KaiValue *ps) {
    return ps->tag == KAI_ARRAY ? ps->as.arr.len : 1;
}

static inline KaiValue **kai_seq_preds_at(KaiValue **ps) {
    return (*ps)->tag == KAI_ARRAY ? (*ps)->as.arr.items : ps;
}

/* `first` then `then` as one list with room for `want`; consumes both. */
static KaiValue *kai_seq_preds_join(KaiValue *first, KaiValue *then, int64_t want) {
    int64_t nf = kai_seq_preds_n(first), nt = kai_seq_preds_n(then);
    int64_t room = (nf + nt) * 2 > want ? (nf + nt) * 2 : want;
    KaiValue *out = first;
    if (first->tag != KAI_ARRAY || !kai_check_unique(first)) {
        out = kai_alloc(KAI_ARRAY);
        out->as.arr.cap = room;
        out->as.arr.items = (KaiValue **) kai_heap_malloc((size_t) room * sizeof(KaiValue *));
        KaiValue **from = kai_seq_preds_at(&first);
        for (int64_t i = 0; i < nf; i++) out->as.arr.items[i] = kai_incref(from[i]);
        out->as.arr.len = nf;
        kai_decref(first);
    } else if (nf + nt > out->as.arr.cap) {
        out->as.arr.items = (KaiValue **) kai_heap_realloc(out->as.arr.items,
            (size_t) out->as.arr.cap * sizeof(KaiValue *), (size_t) room * sizeof(KaiValue *));
        out->as.arr.cap = room;
    }
    KaiValue **from = kai_seq_preds_at(&then);
    for (int64_t i = 0; i < nt; i++) out->as.arr.items[nf + i] = kai_incref(from[i]);
    out->as.arr.len = nf + nt;
    kai_decref(then);
    return out;
}

/* The filter stages under `own` that nobody else reads become part of
 * it: `own` takes the source of the lowest and applies their predicates
 * first, lowest first. A chain of stages is then one stage, read in a
 * loop instead of one frame per stage. The chain is walked down with its
 * links reversed and back up along them, so its length costs no stack. */
static KaiValue *kai_seq_filter_absorb(KaiValue *own, KaiValue *s) {
    KaiValue *below = NULL;
    int64_t n = kai_seq_preds_n(own->as.seq.pred);
    while (s->variant_tag == KAI_SEQ_FILTER && s->as.seq.slot != NULL && kai_check_unique(s)) {
        KaiValue *next = s->as.seq.slot;
        n += kai_seq_preds_n(s->as.seq.pred);
        s->as.seq.slot = below;
        below = s;
        s = next;
    }
    KaiValue *preds = NULL;
    while (below != NULL) {
        KaiValue *up = below->as.seq.slot;
        preds = preds == NULL ? below->as.seq.pred : kai_seq_preds_join(preds, below->as.seq.pred, n);
        below->as.seq.slot = NULL;
        below->as.seq.pred = NULL;
        kai_decref(below);
        below = up;
    }
    if (preds != NULL) own->as.seq.pred = kai_seq_preds_join(preds, own->as.seq.pred, n);
    return s;
}

/* The body of a read moved to a fresh segment: the cell leaves the
 * closure, so the read holds its only reference when the caller did. */
static KaiValue *kai_seq_force_held(KaiValue *self, KaiValue **args, int n) {
    (void) args; (void) n;
    KaiValue *s = self->as.clo.captures[0];
    self->as.clo.captures[0] = NULL;
    return kai_seq_force(s);
}

/* Read `s` on a fresh segment; consumes `s`. `*slot`, a slot of the
 * caller's unwind entry, holds the read's closure meanwhile. */
__attribute__((noinline))
static KaiValue *kai_seq_force_far(KaiValue *s, KaiValue **slot) {
    KaiValue *body = kai_closure(kai_seq_force_held, 0, 1, &s);
    kai_decref(s);
    *slot = body;
    KaiValue *step = kai_seq_apply_far(body);
    *slot = NULL;
    kai_decref(body);
    return step;
}

/* The body of a deep call moved to a fresh segment: the callee and its
 * arguments leave the closure, so the call holds their only references. */
static KaiValue *kai_deep_apply_held(KaiValue *self, KaiValue **args, int n) {
    (void) args; (void) n;
    int argc = self->as.clo.n_captures - 1;
    KaiValue *f = self->as.clo.captures[0];
    KaiValue *argv[2] = { NULL, NULL };
    for (int i = 0; i < argc; ++i) {
        argv[i] = self->as.clo.captures[i + 1];
        self->as.clo.captures[i + 1] = NULL;
    }
    self->as.clo.captures[0] = NULL;
    KaiValue *r = kai_apply_borrow(f, argc, argv);
    kai_decref(f);
    return r;
}

/* Apply `f` to `argc` (1 or 2) owned arguments, on a fresh segment once
 * the running stack is past half used; consumes `f`. A callee that
 * suspends aborts, as a lazy cell's thunk does. */
__attribute__((noinline))
static KaiValue *kai_deep_apply_far(KaiValue *f, int argc, KaiValue **argv, KaiValue **held) {
    KaiValue *caps[3] = { f, argv[0], argc > 1 ? argv[1] : NULL };
    KaiValue *body = kai_closure(kai_deep_apply_held, 0, argc + 1, caps);
    kai_decref(f);
    for (int i = 0; i < argc; ++i) kai_decref(argv[i]);
    held[0] = NULL;
    held[1] = body;
    KaiValue *r = kai_seq_apply_far(body);
    held[1] = NULL;
    kai_decref(body);
    return r;
}

static KaiValue *kai_deep_apply(KaiValue *f, int argc, KaiValue **argv) {
    KaiValue *held[2] = { f, NULL };
    KaiFiber *uf = kai_unw_push(held, 2);
    KaiValue *r;
    if (kai_seq_deep(uf)) {
        r = kai_deep_apply_far(f, argc, argv, held);
        kai_unw_pop(uf);
    } else {
        r = kai_apply_borrow(f, argc, argv);
        kai_unw_pop(uf);
        kai_decref(f);
    }
    return r;
}

/* `__deep_apply1(x, f)` / `__deep_apply2(x, p, f)`: `f(x)` / `f(x, p)`,
 * continued over a fresh stack segment when the running one is past half
 * used, so a recursion through `f` is bounded by memory, not by the stack. */
static KaiValue *kai_core___deep_apply1(KaiValue *x, KaiValue *f) {
    KaiValue *argv[1] = { x };
    return kai_deep_apply(f, 1, argv);
}

static KaiValue *kai_core___deep_apply2(KaiValue *x, KaiValue *p, KaiValue *f) {
    KaiValue *argv[2] = { x, p };
    return kai_deep_apply(f, 2, argv);
}

/* The first step of `s` whose head satisfies every predicate, with the
 * rest filtered in turn; consumes `s`, borrows the predicates. `own` is
 * the stage being read when the reader holds its only reference: it
 * becomes the stage over the rest, and an unshared step is rewritten in
 * place, so a read then allocates nothing. */
static KaiValue *kai_seq_filter_step(KaiValue *s, KaiValue **preds, KaiValue *own) {
    KaiValue *held[2] = { own, NULL };
    KaiFiber *uf = kai_unw_push(held, 2);
    for (;;) {
        if (own != NULL) s = kai_seq_filter_absorb(own, s);
        KaiValue *step = kai_seq_deep(uf) ? kai_seq_force_far(s, &held[1]) : kai_seq_force(s);
        if (step->var_n_args == 0) {
            kai_unw_pop(uf);
            if (own != NULL) kai_decref(own);
            return step;
        }
        held[1] = step;
        uint32_t mask = kai_slot_mask_of(step->variant_tag);
        int boxed = mask == 0 || kai_var_slot_kind(mask, 0) == KAI_VAR_SLOT_PTR;
        int64_t n = kai_seq_preds_n(*preds);
        KaiValue **ps = kai_seq_preds_at(preds);
        int keep = 1;
        for (int64_t i = 0; i < n && keep; i++) {
            KaiValue *x = boxed ? kai_incref(kai_var_slots(step)[0].ptr) : kai_variant_slot_box(step, 0);
            keep = kai_apply_borrow(ps[i], 1, &x) == &kai_singleton_true;
        }
        held[1] = NULL;
        KaiValue *xs = kai_var_slots(step)[1].ptr;
        if (!keep) {
            s = kai_incref(xs);
            kai_decref(step);
            continue;
        }
        kai_unw_pop(uf);
        int unique = kai_check_unique(step);
        if (!unique) kai_incref(xs);
        KaiValue *rest;
        if (own != NULL) { own->as.seq.slot = xs; rest = own; }
        else rest = kai_seq_filter(xs, kai_incref(*preds));
        if (unique) {
            kai_var_slots(step)[1].ptr = rest;
            return step;
        }
        KaiValue *cons = kai_alloc_var(2);
        cons->variant_tag = step->variant_tag;
        cons->var_n_args = 2;
        kai_var_slots(cons)[0] = kai_var_slots(step)[0];
        if (boxed) kai_incref(kai_var_slots(cons)[0].ptr);
        kai_var_slots(cons)[1].ptr = rest;
        kai_decref(step);
        return cons;
    }
}

/* Read a filter stage; consumes `c`. The source leaves the stage while
 * it is read, so the stages below stay unshared; an exit past the read
 * leaves a shared stage without one, and reading it again panics. */
__attribute__((noinline))
static KaiValue *kai_seq_filter_run(KaiValue *c) {
    if (c->variant_tag == KAI_SEQ_FILTERING) {
        kai_decref(c);
        return kai_core_panic(kai_str("sequence depends on itself"));
    }
    KaiValue *s = c->as.seq.slot;
    if (s == NULL) {
        kai_decref(c);
        return kai_core_panic(kai_str("sequence read again after a read that did not finish"));
    }
    c->as.seq.slot = NULL;
    if (kai_check_unique(c)) return kai_seq_filter_step(s, &c->as.seq.pred, c);
    c->variant_tag = KAI_SEQ_FILTERING;
    KaiFiber *uf = kai_unw_push((KaiValue **) c, KAI_UNW_SEQ);
    KaiValue *step = kai_seq_filter_step(s, &c->as.seq.pred, NULL);
    kai_unw_pop(uf);
    KaiValue *p = c->as.seq.pred;
    c->as.seq.slot = step;
    c->as.seq.pred = NULL;
    c->variant_tag = KAI_SEQ_DONE;
    kai_decref(p);
    return kai_seq_take(c);
}

/* ---------- constants of a lazy type ---------- */

static KaiConstCell *kai_const_find(KaiFiber *f, KaiFn init) {
    for (uint32_t i = 0; i < f->const_n; i++) {
        if (f->consts[i].init == init) return &f->consts[i];
    }
    return NULL;
}

/* An exit past a constant's initialiser: the constant is unevaluated
 * again, and the evaluating frame's reference to `init` goes. */
static void kai_const_abandon(KaiFiber *f, KaiValue *init) {
    KaiConstCell *c = kai_const_find(f, init->as.clo.fn);
    if (c != NULL) *c = f->consts[--f->const_n];
    kai_decref(init);
}

/* Release a sequence nothing will read again, emptying each cell of its
 * spine on the way. A sequence defined through itself ends in a cycle (the
 * cell not yet read holds, in its thunk, cells that lead back to it), which
 * a plain release would leave behind; an emptied cell ends every cycle that
 * passes through it. Consumes `s`. */
static void kai_seq_unravel(KaiValue *s) {
    while (kai_is_ptr(s) && s->tag == KAI_SEQ && s->as.seq.slot != NULL) {
        KaiValue *held = s->as.seq.slot;
        KaiValue *rest = NULL;
        if (s->variant_tag == KAI_SEQ_DONE && kai_is_ptr(held) && held->tag == KAI_VARIANT
            && held->var_n_args > 0) {
            int last = held->var_n_args - 1;
            uint32_t mask = kai_slot_mask_of(held->variant_tag);
            if (mask == 0 || kai_var_slot_kind(mask, last) == KAI_VAR_SLOT_PTR) {
                rest = kai_incref(kai_var_slots(held)[last].ptr);
            }
        }
        s->as.seq.slot = NULL;
        s->variant_tag = KAI_SEQ_DONE;
        kai_decref(held);
        kai_decref(s);
        s = rest;
    }
    kai_decref(s);
}

/* What the fiber kept for its constants goes when it ends. Newest first:
 * a release can run a `finally` that reads a constant. */
static void kai_fiber_consts_drop(KaiFiber *f) {
    while (f->const_n > 0) kai_seq_unravel(f->consts[--f->const_n].value);
    free(f->consts);
    f->consts = NULL;
    f->const_cap = 0;
}

/* The value the initialiser `init` builds, built once by the fiber that
 * asks: every later read by that fiber shares it. A read that arrives
 * while `init` runs is the constant reading itself. Consumes `init`. */
static KaiValue *kai_seq_once(KaiValue *init) {
    KaiFiber *f = kai_current_fiber();
    KaiFn key = init->as.clo.fn;
    KaiConstCell *c = kai_const_find(f, key);
    if (c != NULL) {
        kai_decref(init);
        if (!c->ready) return kai_core_panic(kai_str("constant depends on itself"));
        return kai_incref(c->value);
    }
    if (f->const_n == f->const_cap) {
        uint32_t cap = f->const_cap ? f->const_cap * 2 : 4;
        KaiConstCell *buf = (KaiConstCell *) realloc(f->consts, (size_t) cap * sizeof(KaiConstCell));
        if (buf == NULL) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
        f->consts = buf;
        f->const_cap = cap;
    }
    f->consts[f->const_n++] = (KaiConstCell) { key, NULL, 0 };
    kai_unw_push((KaiValue **) init, KAI_UNW_CONST);
    KaiValue *v = kai_apply_borrow(init, 0, NULL);
    kai_unw_pop(f);
    c = kai_const_find(f, key);
    c->value = v;
    c->ready = 1;
    kai_decref(init);
    return kai_incref(v);
}

static KaiValue *kai_core_map(KaiValue *xs, KaiValue *f) {
    KaiValue *held[3] = { xs, f, kai_nil() };
    KaiFiber *uf = kai_unw_push(held, 3);
    KaiSeqIt it;
    kai_seq_it_init(&it, xs);
    for (KaiValue *x = kai_seq_it_next(&it); x; x = kai_seq_it_next(&it)) {
        KaiValue *head = kai_apply_borrow(f, 1, &x);
        held[2] = kai_cons(head, held[2]);
    }
    kai_unw_pop(uf);
    KaiValue *result = kai_core_list_reverse(held[2]);  /* consumes acc */
    if (xs) kai_decref(xs);
    if (f)  kai_decref(f);
    return result;
}

/* issue #201: runtime backing for the flat-map-pipe operator (`||`).
 * Mirrors `kai_core_map` but each `f(elem)` produces a list that
 * gets cons-prepended onto the reversed accumulator; the per-element
 * piece is decref'd in the same step. The final reverse + decref loop
 * matches `_map`'s ownership story so RC stays balanced regardless of
 * the input shape. Empty pieces are a no-op for the inner loop.
 */
static KaiValue *kai_core_flat_map(KaiValue *xs, KaiValue *f) {
    KaiValue *held[3] = { xs, f, kai_nil() };
    KaiFiber *uf = kai_unw_push(held, 3);
    KaiSeqIt it;
    kai_seq_it_init(&it, xs);
    for (KaiValue *x = kai_seq_it_next(&it); x; x = kai_seq_it_next(&it)) {
        KaiValue *piece = kai_apply_borrow(f, 1, &x);
        /* Reverse `piece` into `acc` (which is in reverse order); the
         * final single reverse restores element order. `piece` may
         * itself be a range — the cursor covers it. */
        KaiSeqIt qi;
        kai_seq_it_init(&qi, piece);
        for (KaiValue *q = kai_seq_it_next(&qi); q; q = kai_seq_it_next(&qi))
            held[2] = kai_cons(q, held[2]);
        if (piece) kai_decref(piece);
    }
    kai_unw_pop(uf);
    KaiValue *result = kai_core_list_reverse(held[2]);  /* consumes acc */
    if (xs) kai_decref(xs);
    if (f)  kai_decref(f);
    return result;
}

static KaiValue *kai_core_filter(KaiValue *xs, KaiValue *pred) {
    KaiValue *held[4] = { xs, pred, kai_nil(), NULL };
    KaiFiber *uf = kai_unw_push(held, 4);
    KaiSeqIt it;
    kai_seq_it_init(&it, xs);
    for (KaiValue *x = kai_seq_it_next(&it); x; x = kai_seq_it_next(&it)) {
        /* The predicate gets its own ref of x; the kept element reuses
         * the cursor's owned ref. */
        KaiValue *arg0 = kai_incref(x);
        held[3] = x;
        KaiValue *keep = kai_apply_borrow(pred, 1, &arg0);
        held[3] = NULL;
        int yes = kai_op_truthy(keep);
        kai_decref(keep);
        if (yes) held[2] = kai_cons(x, held[2]);
        else     kai_decref(x);
    }
    kai_unw_pop(uf);
    KaiValue *result = kai_core_list_reverse(held[2]);  /* consumes acc */
    if (xs)   kai_decref(xs);
    if (pred) kai_decref(pred);
    return result;
}

static KaiValue *kai_core_reduce(KaiValue *xs, KaiValue *init, KaiValue *f) {
    /* acc starts as the caller's `init` ref (transferred). Each
     * iteration hands acc to the closure (which consumes it) and
     * receives a freshly-owned `next` back. If `xs` is empty we
     * return init unchanged — its single ref flows out to the
     * caller. A range input runs entirely through the cursor: no cons
     * cell is ever built. */
    KaiValue *held[3] = { xs, f, init };
    KaiFiber *uf = kai_unw_push(held, 3);
    KaiSeqIt it;
    kai_seq_it_init(&it, xs);
    for (KaiValue *x = kai_seq_it_next(&it); x; x = kai_seq_it_next(&it)) {
        KaiValue *args[2];
        args[0] = held[2];                          /* transfer to closure */
        args[1] = x;                                /* closure consumes */
        held[2] = NULL;
        held[2] = kai_apply_borrow(f, 2, args);     /* closure produces fresh acc */
    }
    kai_unw_pop(uf);
    if (xs) kai_decref(xs);
    if (f)  kai_decref(f);
    return held[2];
}

static KaiValue *kai_core_each(KaiValue *xs, KaiValue *f) {
    KaiValue *held[2] = { xs, f };
    KaiFiber *uf = kai_unw_push(held, 2);
    KaiSeqIt it;
    kai_seq_it_init(&it, xs);
    for (KaiValue *x = kai_seq_it_next(&it); x; x = kai_seq_it_next(&it)) {
        KaiValue *r = kai_apply_borrow(f, 1, &x);
        kai_decref(r);
    }
    kai_unw_pop(uf);
    if (xs) kai_decref(xs);
    if (f)  kai_decref(f);
    return kai_unit();
}

