/* Fragment of runtime.h; include runtime.h, never this file. */
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
 * The reuse counter (`kai_rc_count_reuse`) increments on every
 * successful in-place rewrite (see top of file).
 */
static inline int kai_check_unique(KaiValue *v) {
    /* `rc == 1` already implies `rc != INT32_MAX` (a singleton's rc is
     * saturated at INT32_MAX, never 1), so the explicit singleton guard
     * the old code carried was a dead check executed on every call —
     * and check_unique runs at every descent level + twice per nested
     * balance arm. One `rc == 1` test suffices; an immediate (kai_is_
     * value) has no header so it is excluded first. Koka's
     * kk_datatype_ptr_is_unique is likewise a single refcount test.
     * Also marked inline (was a plain `static int` — a real call on the
     * hot path). */
    return v != NULL && !kai_is_value(v) && kai_rc_load(v) == 1;
}

/* Conditional incref for a variant reuse arm whose donor is SHARED. When the
 * donor reuses in place (unique), its kept children MOVE into the rebuild — no
 * incref. When it is shared, the rebuild fresh-allocs and keeps a borrowed
 * reference to each kept child, but the donor still owns those children and the
 * match-exit `kai_decref(donor)` will cascade-free them; so each kept child the
 * rebuild embeds needs its own ref. The emitter calls this once per embedded
 * borrowed child (multiset: a child used twice is incref'd twice). Idempotent
 * vs the reuse op's own uniqueness test — nothing mutates the donor between. */
static inline void kai_incref_if_shared(KaiValue *donor, KaiValue *child) {
    if (!kai_check_unique(donor)) kai_incref(child);
}

static KaiValue *kai_reuse_or_alloc_cons(KaiValue *_scr,
                                         KaiValue *head, KaiValue *tail) {
    if (_scr != NULL && _scr->tag == KAI_CONS && kai_check_unique(_scr)) {
        /* Aliasing guard (see kai_reuse_or_alloc_variant): the incoming
         * head/tail may alias the outgoing ones when `f` reused the
         * child cell in place. Store first, decref the old only if it
         * differs — decref-then-store would free a cell the new slot
         * still points at. Latent under libc malloc, exposed by the
         * cell free-list's immediate recycle. */
        KaiValue *old_h = _scr->as.cons.head;
        KaiValue *old_t = _scr->as.cons.tail;
        _scr->as.cons.head = head;
        _scr->as.cons.tail = tail;
        if (old_h != head) kai_decref(old_h);
        if (old_t != tail) kai_decref(old_t);
        kai_rc_count_reuse();
        return kai_incref(_scr);   /* survive enclosing match-exit decref */
    }
    return kai_cons(head, tail);
}

static KaiValue *kai_reuse_or_alloc_record(KaiValue *_scr,
                                           int n, KaiValue **fields,
                                           const char **names,
                                           uint64_t kept_mask) {
    if (_scr != NULL && _scr->tag == KAI_RECORD &&
        _scr->as.rec.n_fields == n && kai_check_unique(_scr)) {
        /* Decref the existing field values then overwrite. The
         * fields[] / names[] pointer arrays are reused — both have
         * the same length n, so no realloc is needed. names[]
         * usually points at the same static-string literals across
         * rebuilds (parsers thread the same record shape), so
         * overwriting is idempotent in the common case. */
        for (int i = 0; i < n; ++i) {
            /* Aliasing guard (see kai_reuse_or_alloc_variant): skip
             * the old-slot decref when old == new, EXCEPT where
             * kept_mask marks the incoming value as a plain binder
             * read (a kept child re-embedded in place). A binder read
             * carries its own reference, so the donor's claim on the
             * old child is still owed — the skip would leak it once
             * per reuse. The mask never marks call results, the shape
             * whose consumed-and-recycled pointer the guard protects. */
            KaiValue *old_f = _scr->as.rec.fields[i];
            _scr->as.rec.fields[i] = fields[i];
            _scr->as.rec.names[i]  = names[i];
            if (old_f != fields[i] ||
                (i < 64 && (kept_mask & ((uint64_t)1 << i)))) {
                kai_decref(old_f);
            }
        }
        /* The donor may be another record type: the rebuild is untagged
         * unless its own site stamps it. */
        _scr->as.rec.head_type_tag = 0;
        kai_rc_count_reuse();
        return kai_incref(_scr);
    }
    return kai_record(n, fields, names);
}


static KaiValue *kai_reuse_or_alloc_variant(KaiValue *_scr,
                                            int32_t tag, const char *name,
                                            int n, uint32_t mask,
                                            KaiVarSlot *slots) {
    if (_scr != NULL && _scr->tag == KAI_VARIANT &&
        _scr->var_n_args == n && kai_slot_mask_of(_scr->variant_tag) == mask &&
        kai_check_unique(_scr)) {
        /* Phase 2: reuse fires when scrutinee and replacement agree on
         * the slot-mask layout. For mask==0 the scrutinee's slots are
         * all pointers and we must decref each one before overwriting.
         * For mask!=0 the matching slots are primitive payloads with
         * no RC discipline; the call sites that emit
         * `kai_reuse_or_alloc_variant` for typed payloads are
         * synthesised by stage 2's Perceus recogniser only when the
         * mask matches, so this branch stays sound.
         *
         * ALIASING GUARD (issue #118 #3 latent UAF, surfaced by the cell
         * free-list): the incoming `slots[i]` may ALIAS the outgoing
         * `_scr->slots[i]` — the common desugar shape
         * `Some(e) -> Some(f(e))` increfs `e`, computes `f(e)` (which
         * may reuse e's own cell in place, so the result pointer ==
         * the old slot pointer), then reuses `_scr`. Decref-then-store
         * would free the very cell the new slot points at. We decref the
         * OLD pointer only when it differs from the incoming one; an
         * aliased slot keeps the cell live (the incref/decref pair from
         * the bind + f's consumption already balanced it). With libc
         * malloc this latent double-free was masked (the freed cell
         * survived untouched until a far-off reuse); the immediate
         * free-list recycle exposed it. */
        for (int i = 0; i < n; ++i) {
            uint32_t bit = (uint32_t) 1 << i;
            KaiValue *old_ptr = kai_var_slots(_scr)[i].ptr;
            kai_var_slots(_scr)[i] = slots[i];
            if ((mask & bit) == 0 && old_ptr != slots[i].ptr) {
                kai_decref(old_ptr);
            }
        }
        _scr->variant_tag  = tag;
        kai_rc_count_reuse();
        return kai_incref(_scr);
    }
    return kai_variant_u(tag, name, n, mask, slots);
}

/* ---------- TRMC constructor-context (port of Koka types-cctx.h) ----------
 *
 * Tail-Recursion-Modulo-Cons. Direct port of Koka's
 * `lib/std/core/inline/types-cctx.h` (Leijen & Lorenzen, "Tail Recursion
 * Modulo Context", POPL'22). A recursive spine-rebuild whose tail
 * expression is `Ctor(.., recur(child), ..)` is rewritten into a
 * `goto`-loop carrying a *constructor context* (`KaiCctx`): the partially
 * built result `res` plus a pointer `holeptr` to the open field where the
 * next node attaches. Each loop iteration builds one node with a HOLE in
 * the recursive slot, extends the cctx to point at that hole, and rebinds
 * the loop variable to the child — O(1) stack, perfect spine reuse when
 * the consumed cell is unique.
 *
 * kaikai difference from Koka: kaikai's holeptr points into the SEPARATE
 * `slots[]` array (`&kai_var_slots(node)[i].ptr`), not an inline field of
 * the block. The array stays alive and stable for the node's lifetime
 * (kai_variant_at overwrites in place; the fresh-alloc branch allocates a
 * fresh stable array), so the hole address never dangles. Koka's
 * unique-check is rc==0; kaikai's is rc==1 (see kai_check_unique).
 *
 * The `_linear` variants are what the compiler emits for affine effect
 * rows (the rb-tree's effect is total/affine). They assume `acc.res` is
 * unique — which TRMC guarantees because every node on the spine was just
 * built (or reused) by this loop and is held only by the cctx. Non-affine
 * (multi-resume) fns are NOT recognised by the TRMC pass; they fall back
 * to ordinary recursion, so the non-linear cctx path is not needed yet.
 */
typedef KaiValue **KaiFieldAddr;                  /* &kai_var_slots(node)[i].ptr */
typedef struct { KaiValue *res; KaiFieldAddr holeptr; } KaiCctx;

static inline KaiFieldAddr kai_field_addr_create(KaiValue **p) { return p; }

static inline KaiCctx kai_cctx_empty(void) { KaiCctx c = { NULL, NULL }; return c; }

/* apply_linear: plug `child` into the open hole, return the spine root.
 * Empty cctx (first level) → the child IS the root. Mirrors Koka
 * kk_cctx_apply_linear (types-cctx.h:66). */
static inline KaiValue *kai_cctx_apply_linear(KaiCctx acc, KaiValue *child) {
    if (acc.holeptr != NULL) { *acc.holeptr = child; return acc.res; }
    return child;
}

/* extend_linear: plug `child` into the old hole, return a new cctx whose
 * open hole is `field` (a slot inside `child`). Mirrors Koka
 * kk_cctx_extend_linear (types-cctx.h:91). */
static inline KaiCctx kai_cctx_extend_linear(KaiCctx acc, KaiValue *child,
                                             KaiFieldAddr field) {
    KaiCctx c;
    c.res = kai_cctx_apply_linear(acc, child);
    c.holeptr = field;
    return c;
}

/* Reuse token (Koka kk_datatype_ptr_reuse). The caller has already proven
 * `v` unique with kai_check_unique. Unlike kai_reuse_or_alloc_variant,
 * this does NOT decref the donor's children: the emitter MOVES them into
 * the new node (the unique branch) or dups+decrefs explicitly (the shared
 * branch), exactly as rbtree__koka.c:268-285. Returning the raw cell lets
 * kai_variant_at overwrite it in place. */
typedef KaiValue *KaiReuse;
#define kai_reuse_null NULL
static inline KaiReuse kai_ptr_reuse(KaiValue *v) { return v; }

/* arm-top drop-reuse, BORROW model (Koka kk_block_drop_reuse, kklib.h:818).
 * Runs at the TOP of a match arm whose binds are BORROW (children read from
 * `v`'s slots WITHOUT an incref), exactly like Koka's insert_loop:
 *
 *     tree l = con->left; ... r = con->right;      // borrow binds, no dup
 *     reuse_t _ru = kk_reuse_null;
 *     if (is_unique(t)) { _ru = reuse(t); }         // UNIQUE: take shell, children MOVE
 *     else { dup(l); dup(r); ...; decref(t); }      // SHARED: dup kept children, drop t
 *
 *   - UNIQUE (kaikai rc==1): return the bare cell as a token. Do NOT touch
 *     the children — they MOVE from this shell into the rebuilt node (the
 *     borrow binds become the owners; net RC 0). The emit does NOT dup them
 *     in the unique branch; kai_variant_at overwrites the slots next.
 *   - SHARED (rc>1): NON-recursive `rc--` only. The cell stays live for its
 *     other owners; its children stay referenced by it. The emit has dup'd
 *     the children it keeps in the shared branch. Return null → fresh alloc.
 *
 * `n` is the rebuild arity; n_args != n cannot host the rebuild. With borrow
 * binds the children are not owned here, so on mismatch we must NOT cascade:
 * refcount-only decrement, no token. (A mismatch is a recogniser bug.)
 *
 * CRITICAL vs kai_decref: the shared branch is a NON-recursive `rc--`. A
 * recursive decref would cascade into the borrow-bound children the emit is
 * about to use — the 14x-tree leak/UAF from the misplaced first attempt.
 * Koka's kk_block_drop_reuse is likewise refcount-only; child drops happen
 * via explicit emit-side dup/decref, never inside the reuse primitive. */
/* Only Fiber and Pid cells are atomic, never a variant, so the plain
 * decrements below cannot meet an atomic count. */
static inline KaiReuse kai_drop_reuse_token(KaiValue *v, int n) {
    if (v == NULL || kai_is_value(v) || v->tag != KAI_VARIANT ||
        v->var_n_args != n) {
        if (v != NULL && !kai_is_value(v) && v->tag == KAI_VARIANT &&
            v->rc != INT32_MAX) {
            v->rc -= 1;   /* non-recursive: children are borrow-bound, not ours */
        }
#ifdef KAI_TRACE_RC
        kai_rc_tok_null_mismatch++;
#endif
        return kai_reuse_null;
    }
    if (kai_check_unique(v)) {
        /* sole owner: hand back the shell. Children MOVE into the rebuild
         * (borrow binds become the owners). kai_variant_at overwrites next. */
#ifdef KAI_TRACE_RC
        kai_rc_tok_unique++;
#endif
        return v;
    }
    /* shared: non-recursive refcount decrement. */
    if (v->rc != INT32_MAX) v->rc -= 1;
#ifdef KAI_TRACE_RC
    kai_rc_tok_null_shared++;
#endif
    return kai_reuse_null;
}

/* alloc-at: write a variant Ctor into a donated cell IN PLACE (no malloc),
 * else fall back to a fresh alloc. Move semantics — does NOT decref the
 * donor's old slots (they were extracted into locals and either moved into
 * `slots[]` or dup'd by the emitter before donation). This is the correct
 * embryo; kai_reuse_or_alloc_variant's eager-1:1-decref is NOT (it
 * double-frees on non-bijective rebuilds — see lane memory). The donated
 * cell already has the right slots[] array length (n_args >= n checked);
 * we overwrite the slot words, retag, and reset rc=1. */
static inline void kai_reuse_free(KaiReuse at);

/* A token whose arity cannot host the rebuild is still owned here: every
 * caller nulls its copy before the call, so it is freed, never dropped. */
static inline KaiValue *kai_variant_at(KaiReuse at, int32_t tag,
                                       const char *name, int n, uint32_t mask,
                                       KaiVarSlot *slots) {
    if (at != NULL && at->tag == KAI_VARIANT && at->var_n_args == n) {
        for (int i = 0; i < n; ++i) kai_var_slots(at)[i] = slots[i];
        at->variant_tag = tag;
        /* The tag→mask register is NOT repeated here: a reuse target is,
         * by construction, a cell the program already built fresh (via
         * kai_variant_u / kai_variant_u_fast), so its mask is registered.
         * The slotmask table read by the drop walker is keyed on tag, not
         * on this cell, so the steady-state register was pure redundant
         * work (kai_slotmask_register already no-op'd it).
         * Dropping it removes a call from every in-place rebuild — the
         * rb-tree reuse arm fires ~20M times. The reuse counter only
         * exists under tracing; gate it so the steady path is store-only. */
        at->rc = 1;
#ifdef KAI_TRACE_RC
        kai_rc_count_reuse();
#endif
        (void) mask; (void) name;
        return at;
    }
    kai_reuse_free(at);
    return kai_variant_u(tag, name, n, mask, slots);
}

/* Free a reuse-TOKEN whose children have already MOVED out (Koka
 * kk_block_drop / ParcReuse's drop of an unconsumed `Available` cell).
 * A reuse-token is captured at the arm top (kai_drop_reuse_token UNIQUE
 * branch) WITHOUT touching its children — the borrow binds become the
 * owners. When the arm body reaches a tail that does NOT host a
 * kai_variant_at rebuild (e.g. `balance_left(insert_loop(l,...), ..., r)`
 * — the call allocates its own node inside its own frame), the token is
 * never consumed. ParcReuse treats the reuse-token as LINEAR: consumed by
 * a Con@reuse, else dropped on the non-consuming path. This is that drop.
 *
 * CRITICAL: it must free ONLY the cell, never cascade into the children —
 * those were stolen (no incref) into owned binders the body is still using
 * (the rb-tree balance arm passes them to balance_left). We zero n_args so
 * the KAI_VARIANT case of kai_free_value skips the slot decref loop, then
 * route through the normal free path (poison / cell-pool / counters stay
 * correct). A null token is a no-op (the shared branch returned null). */
static inline void kai_reuse_free(KaiReuse at) {
    if (at == NULL) return;
    if (kai_is_value(at) || at->tag != KAI_VARIANT) return;
    /* Children already MOVED out — must NOT cascade-decref them. With the
     * packed header, `slot_mask` is now a tag→mask TABLE, not a per-node
     * field, so the old trick of stamping this node's mask "all-INT" would
     * corrupt the shared mask of EVERY node of this tag (the double-free /
     * non-exhaustive crash). Instead recycle the block DIRECTLY at its real
     * arity, skipping kai_free_value's child-decref loop entirely — exactly
     * the "no cascade" intent, done per-node. The trace/RC counters that
     * kai_free_value would bump are handled here too. */
    kai_var_block_free(at, at->var_n_args);
    kai_rc_count_free();
#ifdef KAI_TRACE_RC
    kai_rc_reuse_free_total++;
    /* Per-tag free accounting (issue #296 table). `kai_var_block_free`
     * recycles the block directly, bypassing `kai_free_value`, so the
     * per-tag `frees` counter would otherwise stay flat while `allocs`
     * climbed — inflating the reported `live=allocs-frees` by exactly the
     * reuse_free count even though the cells are genuinely reclaimed (RSS
     * flat). Mirror kai_free_value's bump so `live` reflects real liveness
     * under arm-top reuse (this surfaced as a false LEAK in the #747 trace
     * once the LLVM backend began freeing unconsumed reuse tokens). */
    kai_rc_free_by_tag[(int) KAI_VARIANT]++;
#endif
}

/* A flat arm takes apart the variant cell its pattern matched. A sole owner
 * hands its children to the arm's binders: the pointer slots in `unbound`,
 * which no binder took, are released, and the shell is freed with no
 * cascade. A shared cell loses one reference and returns 0: the caller then
 * dups the children it bound. The unique half stays out of line, so an
 * arm's prologue is a refcount test. */
__attribute__((noinline)) KAI_RT_SHARED void kai_arm_take_shell(KaiValue *v, uint32_t unbound) {
    for (int i = 0; unbound != 0; ++i, unbound >>= 1) {
        if (unbound & 1u) kai_decref(kai_var_slots(v)[i].ptr);
    }
    kai_var_block_free(v, v->var_n_args);
    kai_rc_count_free();
#ifdef KAI_TRACE_RC
    kai_rc_free_by_tag[(int) KAI_VARIANT]++;
#endif
}

static inline int kai_arm_take(KaiValue *v, uint32_t unbound) {
    if (kai_check_unique(v)) { kai_arm_take_shell(v, unbound); return 1; }
    if (kai_is_ptr(v) && v->rc != INT32_MAX) {
        KAI_RC_TRAFFIC_INC(kai_rc_decref_total);
        v->rc -= 1;
    }
    return 0;
}

/* TRMC reuse-in-place (Koka kk_block_drop_reuse + kk_block_alloc_at,
 * fused for the TRMC modulo-cons site). `_scr` is the variant cell the
 * enclosing `match` arm just consumed. When it is UNIQUE and has the
 * target arity, donate it as the storage for the rebuilt node:
 * overwrite the slot words (MOVE semantics — the arm already extracted
 * the donor's children into locals without incref, so we must NOT
 * decref them), retag, and `incref` the cell so it survives the
 * `kai_decref(_scr)` the match emits at arm exit (net rc after exit:
 * 1, a unique freshly-built node — exactly the kai_reuse_or_alloc_cons
 * discipline). When `_scr` is shared or wrong-arity, allocate a fresh
 * node and leave `_scr` for the match exit to reclaim normally.
 *
 * This is what collapses the rb-tree's RBNode rebuild churn: each
 * insert reuses the cells it walked instead of paired free+alloc,
 * approaching Koka's in-place cost. Koka's unique gate is rc==0;
 * kaikai's is rc==1 (kai_check_unique). */
__attribute__((always_inline)) static inline KaiValue *kai_variant_reuse_at(KaiValue *_scr, int32_t tag,
                                                                            const char *name, int n,
                                                                            uint32_t mask, KaiVarSlot *slots) {
    if (_scr != NULL && !kai_is_value(_scr) && _scr->tag == KAI_VARIANT &&
        _scr->var_n_args == n && kai_check_unique(_scr)) {
        /* A donor pointer slot the rebuild does not re-embed still holds
         * the cell's claim on the old child; pay it here or it leaks
         * (the wildcard-slot rebuild `MErr(_, _) -> MVal(99, [1])`). An
         * aliased slot is a kept child moved in place — its bookkeeping
         * already balanced, same guard as kai_reuse_or_alloc_variant.
         * Slot-kind decode mirrors kai_free_one: mask 0 means
         * all-pointer, else 2 bits per slot. */
        uint32_t donor_mask = kai_slot_mask_of(_scr->variant_tag);
        for (int i = 0; i < n; ++i) {
            KaiValue *old_ptr = NULL;
            if (donor_mask == 0 || kai_var_slot_kind(donor_mask, i) == KAI_VAR_SLOT_PTR) {
                old_ptr = kai_var_slots(_scr)[i].ptr;
                if (!kai_is_ptr(old_ptr)) old_ptr = NULL;
            }
            kai_var_slots(_scr)[i] = slots[i];
            if (old_ptr != NULL && old_ptr != slots[i].ptr) kai_decref(old_ptr);
        }
        _scr->variant_tag = tag;
        kai_slotmask_register(_scr->variant_tag, mask);
        kai_rc_count_reuse();
        return kai_incref(_scr);   /* survive the match-exit kai_decref(_scr) */
    }
    return kai_variant_u(tag, name, n, mask, slots);
}

/* Allocate an array of `len` slots, each initialised to `init`
   (incref'd once per slot). Caller owns the returned array. */
KAI_RT_SHARED KAI_RC_NOINLINE KaiValue *kai_array_make(int64_t len, KaiValue *init) {
    if (len < 0) { kai_eprintf("kai: array_make: negative length\n"); kai_exit(1); }
    KaiValue *v = kai_alloc(KAI_ARRAY);
    v->as.arr.len = len;
    v->as.arr.cap = len > 0 ? len : 1;
    v->as.arr.items = (KaiValue **) kai_heap_malloc((size_t) v->as.arr.cap * sizeof(KaiValue *));
    for (int64_t i = 0; i < len; ++i) v->as.arr.items[i] = kai_incref(init);
    return v;
}

/* Grow `a` to length `new_len`, initialising new slots with `init`.
   Mutates in place, returns the same value (incref'd for the caller
   so the result can be bound just like kai_array_make). If new_len
   <= current len this is a no-op beyond the incref. */
static KaiValue *kai_array_grow_impl(KaiValue *a, int64_t new_len, KaiValue *init) {
    if (!a || a->tag != KAI_ARRAY) {
        kai_eprintf("kai: array_grow: not an array\n"); kai_exit(1);
    }
    if (new_len > a->as.arr.cap) {
        int64_t nc = a->as.arr.cap * 2;
        if (nc < new_len) nc = new_len;
        a->as.arr.items = (KaiValue **) kai_heap_realloc(a->as.arr.items,
            (size_t) a->as.arr.cap * sizeof(KaiValue *), (size_t) nc * sizeof(KaiValue *));
        a->as.arr.cap = nc;
    }
    for (int64_t i = a->as.arr.len; i < new_len; ++i) a->as.arr.items[i] = kai_incref(init);
    if (new_len > a->as.arr.len) a->as.arr.len = new_len;
    return kai_incref(a);
}

/* O(1) read. Callers own the returned reference. */
static KaiValue *kai_array_get_impl(KaiValue *a, int64_t i) {
    if (!a || a->tag != KAI_ARRAY) {
        kai_eprintf("kai: array_get: not an array\n"); kai_exit(1);
    }
    if (i < 0 || i >= a->as.arr.len) {
        static char buf[96];
        snprintf(buf, sizeof(buf), "index %lld out of range (len=%lld)",
                 (long long) i, (long long) a->as.arr.len);
        kai_trap_abort(buf);
    }
    return kai_incref(a->as.arr.items[i]);
}

/* O(1) write. Takes ownership of `v`, decref's the previous slot.
   Returns the same array (incref'd) so callers can thread it. */
static KaiValue *kai_array_set_impl(KaiValue *a, int64_t i, KaiValue *v) {
    if (!a || a->tag != KAI_ARRAY) {
        kai_eprintf("kai: array_set: not an array\n"); kai_exit(1);
    }
    if (i < 0 || i >= a->as.arr.len) {
        static char buf[96];
        snprintf(buf, sizeof(buf), "index %lld out of range (len=%lld)",
                 (long long) i, (long long) a->as.arr.len);
        kai_trap_abort(buf);
    }
    kai_decref(a->as.arr.items[i]);
    a->as.arr.items[i] = v;
    return kai_incref(a);
}

/* ---------- Vec[T] — pure value vector (flat buffer, CoW) ----------
 *
 * One contiguous growable buffer with a single RC header (the KaiValue
 * node). The pure API never exposes mutation: writes go through the
 * same rc == 1 uniqueness check as Perceus reuse-in-place — a unique
 * buffer is rewritten in place (no witness can observe it), a shared
 * one is copied first. Element storage is unboxed whenever the element
 * shape allows: raw 8-byte scalars, or all-scalar records inlined at
 * n_fields * 8 bytes per element with no per-element header. */

/* The scalar KaiTag of a value that can live unboxed in a vec slot,
 * or -1 when it must stay boxed. A tagged-immediate Int (no header)
 * classifies as KAI_INT before any header read. */
static int kai_vec_scalar_tag(KaiValue *x) {
    if (!x) return -1;
    if (kai_is_value(x)) return KAI_INT;
    switch ((KaiTag) x->tag) {
        case KAI_INT: case KAI_REAL: case KAI_BOOL:
        case KAI_CHAR: case KAI_BYTE:
            return (int) x->tag;
        default:
            return -1;
    }
}

static int64_t kai_vec_scalar_bits(KaiValue *x, uint8_t tag) {
    switch ((KaiTag) tag) {
        case KAI_INT:  return kai_intf(x);   /* immediate or heap box */
        case KAI_REAL: { int64_t b; memcpy(&b, &x->as.r, 8); return b; }
        case KAI_BOOL: return (int64_t) x->as.b;
        case KAI_CHAR: return (int64_t) x->as.c;
        default:       return (int64_t) x->as.byte_val;  /* KAI_BYTE */
    }
}

static KaiValue *kai_vec_scalar_box(uint8_t tag, int64_t bits) {
    switch ((KaiTag) tag) {
        case KAI_INT:  return kai_int(bits);
        case KAI_REAL: { double r; memcpy(&r, &bits, 8); return kai_real(r); }
        case KAI_BOOL: return kai_bool((int) bits);
        case KAI_CHAR: return kai_char((uint32_t) bits);
        default:       return kai_byte((uint8_t) bits);  /* KAI_BYTE */
    }
}

/* Decide the storage kind from the first element the vec holds. The
 * element type is static, so one classification covers every element
 * the vector will ever store. */
static void kai_vec_classify(KaiVecMeta *m, KaiValue *x) {
    int st = kai_vec_scalar_tag(x);
    if (st >= 0) {
        m->ekind = KAI_VEC_EK_RAW;
        m->elem_tag = st;
        m->stride = 8;
        return;
    }
    if (x && kai_is_ptr(x) && x->tag == KAI_RECORD &&
        x->as.rec.n_fields > 0 && x->as.rec.n_fields <= KAI_VEC_REC_MAX) {
        int n = x->as.rec.n_fields;
        int all_scalar = 1;
        for (int i = 0; i < n; ++i) {
            int ft = kai_vec_scalar_tag(x->as.rec.fields[i]);
            if (ft < 0) { all_scalar = 0; break; }
            m->field_tags[i] = (uint8_t) ft;
            m->names[i] = x->as.rec.names[i];
        }
        if (all_scalar) {
            m->ekind = KAI_VEC_EK_REC;
            m->n_fields = n;
            m->head_tag = x->as.rec.head_type_tag;
            m->stride = (int64_t) n * 8;
            return;
        }
    }
    m->ekind = KAI_VEC_EK_BOXED;
    m->stride = (int64_t) sizeof(KaiValue *);
}

/* Fresh node + block. The block always carries a meta prefix, even at
 * cap 0 (PENDING keeps stride 0; the alloc floor keeps malloc happy). */
static KaiValue *kai_vec_alloc(int64_t len, int64_t cap, const KaiVecMeta *src) {
    KaiValue *v = kai_alloc(KAI_VEC);
    int64_t stride = (src && src->stride > 0) ? src->stride : 0;
    size_t bytes = sizeof(KaiVecMeta) + (size_t) cap * (size_t) stride;
    v->as.vec.len = len;
    v->as.vec.cap = cap;
    v->as.vec.view_of = NULL;
    v->as.vec.data = kai_heap_malloc(bytes);
    if (src) *kai_vec_meta(v) = *src;
    else     memset(kai_vec_meta(v), 0, sizeof(KaiVecMeta));
    return v;
}

/* Write one element into a slot whose old contents are GARBAGE (fresh
 * or already released). Consumes `x`. The shape checks trap loudly on
 * a meta mismatch — unreachable under a sound typer. */
static void kai_vec_write_elem(KaiVecMeta *m, char *slot, KaiValue *x) {
    switch (m->ekind) {
        case KAI_VEC_EK_RAW: {
            if (kai_vec_scalar_tag(x) != m->elem_tag)
                kai_trap_abort("vec: element shape mismatch");
            int64_t bits = kai_vec_scalar_bits(x, (uint8_t) m->elem_tag);
            memcpy(slot, &bits, 8);
            kai_decref(x);
            return;
        }
        case KAI_VEC_EK_REC: {
            if (!x || !kai_is_ptr(x) || x->tag != KAI_RECORD ||
                x->as.rec.n_fields != m->n_fields)
                kai_trap_abort("vec: record shape mismatch");
            for (int i = 0; i < m->n_fields; ++i) {
                /* Constructors normally emit fields in one canonical
                 * order; resolve by name so a permuted literal still
                 * lands each payload in its column. */
                int j = -1;
                if (x->as.rec.names[i] == m->names[i]) j = i;
                else {
                    for (int k = 0; k < m->n_fields; ++k) {
                        if (x->as.rec.names[k] == m->names[i] ||
                            strcmp(x->as.rec.names[k], m->names[i]) == 0) { j = k; break; }
                    }
                }
                if (j < 0 || kai_vec_scalar_tag(x->as.rec.fields[j]) != m->field_tags[i])
                    kai_trap_abort("vec: record shape mismatch");
                int64_t bits = kai_vec_scalar_bits(x->as.rec.fields[j], m->field_tags[i]);
                memcpy(slot + (size_t) i * 8, &bits, 8);
            }
            kai_decref(x);
            return;
        }
        default:  /* BOXED — the slot steals x's incoming ref */
            memcpy(slot, &x, sizeof(KaiValue *));
            return;
    }
}

/* Read one element as an owned boxed value. */
static KaiValue *kai_vec_read_elem(KaiVecMeta *m, const char *slot) {
    switch (m->ekind) {
        case KAI_VEC_EK_RAW: {
            int64_t bits;
            memcpy(&bits, slot, 8);
            return kai_vec_scalar_box((uint8_t) m->elem_tag, bits);
        }
        case KAI_VEC_EK_REC: {
            KaiValue *fields[KAI_VEC_REC_MAX];
            for (int i = 0; i < m->n_fields; ++i) {
                int64_t bits;
                memcpy(&bits, slot + (size_t) i * 8, 8);
                fields[i] = kai_vec_scalar_box(m->field_tags[i], bits);
            }
            KaiValue *r = kai_record(m->n_fields, fields, m->names);
            r->as.rec.head_type_tag = m->head_tag;
            return r;
        }
        default: {  /* BOXED */
            KaiValue *p;
            memcpy(&p, slot, sizeof(KaiValue *));
            return kai_incref(p);
        }
    }
}

static KAI_RC_NOINLINE KaiValue *kai_vec_make(int64_t len, KaiValue *init) {
    if (len < 0) { kai_eprintf("kai: vec_make: negative length\n"); kai_exit(1); }
    KaiVecMeta m;
    memset(&m, 0, sizeof(m));
    if (len > 0) kai_vec_classify(&m, init);
    KaiValue *v = kai_vec_alloc(len, len, &m);
    KaiVecMeta *vm = kai_vec_meta(v);
    char *base = kai_vec_elems(v);
    if (len == 0) return v;
    if (vm->ekind == KAI_VEC_EK_BOXED) {
        KaiValue **items = (KaiValue **) base;
        for (int64_t i = 0; i < len; ++i) items[i] = kai_incref(init);
    } else {
        /* Extract the payload once, then replicate the stride image. */
        kai_vec_write_elem(vm, base, kai_incref(init));
        for (int64_t i = 1; i < len; ++i)
            memcpy(base + (size_t) i * (size_t) vm->stride, base, (size_t) vm->stride);
    }
    return v;
}

static KaiValue *kai_vec_empty(void) {
    return kai_vec_alloc(0, 0, NULL);
}

/* Copy for the shared-write path. Boxed children are shared (incref),
 * raw bytes are memcpy'd; the copy is unique (rc = 1) by construction. */
static KAI_RC_NOINLINE KaiValue *kai_vec_clone(KaiValue *v, int64_t cap) {
    KaiVecMeta *m = kai_vec_meta(v);
    int64_t len = v->as.vec.len;
    if (cap < len) cap = len;
    KaiValue *c = kai_vec_alloc(len, cap, m);
    char *dst = kai_vec_elems(c);
    memcpy(dst, kai_vec_elems(v), (size_t) len * (size_t) (m->stride > 0 ? m->stride : 0));
    if (m->ekind == KAI_VEC_EK_BOXED) {
        KaiValue **items = (KaiValue **) dst;
        for (int64_t i = 0; i < len; ++i) kai_incref(items[i]);
    }
    return c;
}

static void kai_vec_bounds(KaiValue *v, int64_t i, const char *op) {
    if (!v || kai_is_value(v) || v->tag != KAI_VEC) {
        kai_eprintf("kai: %s: not a vec\n", op); kai_exit(1);
    }
    if (i < 0 || i >= v->as.vec.len) {
        static char buf[96];
        snprintf(buf, sizeof(buf), "index %lld out of range (len=%lld)",
                 (long long) i, (long long) v->as.vec.len);
        kai_trap_abort(buf);
    }
}

/* Bounds gate for a dynamic index into a fixed-width `Vec[t]<n>`: the
 * length is a compile-time constant, so only the index needs checking.
 * Returns the index so the caller can use it inline as the subscript. */
static int64_t kai_fixed_vec_idx(int64_t i, int64_t n) {
    if (i < 0 || i >= n) {
        static char buf[96];
        snprintf(buf, sizeof(buf), "index %lld out of range (len=%lld)",
                 (long long) i, (long long) n);
        kai_trap_abort(buf);
    }
    return i;
}

/* O(1) read; the element leaves the vec owned by the caller. */
static KaiValue *kai_vec_get_impl(KaiValue *v, int64_t i) {
    kai_vec_bounds(v, i, "vec_get");
    KaiVecMeta *m = kai_vec_meta(v);
    return kai_vec_read_elem(m, kai_vec_elems(v) + (size_t) i * (size_t) m->stride);
}

/* The raw 8-byte payload of a scalar element, no box, no RC on either
 * side. The dynamic twin of a fixed vec's `(v).a[i]` inline load: the
 * read a `Vec[t]<n>` value takes once it has crossed the boxed border
 * and is a heap vec, where the caller still wants a bare scalar. Only
 * valid on a RAW-kind vec (flat scalar elements). */
static const void *kai_vec_raw_slot(KaiValue *v, int64_t i) {
    kai_vec_bounds(v, i, "vec_get");
    KaiVecMeta *m = kai_vec_meta(v);
    return kai_vec_elems(v) + (size_t) i * (size_t) m->stride;
}

/* The one unique-or-copy decision every Vec write rides: unique (rc==1)
 * mutates in place, shared clones with `need_cap` capacity. Every write
 * path — boxed or raw — MUST pass through here before touching bytes; a
 * raw store on a shared buffer would silently corrupt a value another
 * reference is observing. */
static KaiValue *kai_vec_ensure_unique(KaiValue *v, int64_t need_cap) {
    /* A view is shared by construction: its bytes belong to the owner,
     * so every write through it copies — even at node rc == 1. */
    if (v->as.vec.view_of || !kai_check_unique(v)) {
        KaiValue *c = kai_vec_clone(v, need_cap);
        kai_decref(v);
        kai_vec_count_cow();
        return c;
    }
    kai_vec_count_inplace();
    return v;
}

/* Grow the (unique) block to `ncap` elements; returns the moved meta. */
static KaiVecMeta *kai_vec_grow_cap(KaiValue *v, int64_t ncap) {
    KaiVecMeta *m = kai_vec_meta(v);
    v->as.vec.data = kai_heap_realloc(v->as.vec.data,
        sizeof(KaiVecMeta) + (size_t) v->as.vec.cap * (size_t) (m->stride > 0 ? m->stride : 0),
        sizeof(KaiVecMeta) + (size_t) ncap * (size_t) m->stride);
    v->as.vec.cap = ncap;
    return kai_vec_meta(v);
}

/* Pure write. Consumes `v` and `x`; returns the result vec (in place
 * when `v` was unique, a copy when shared). */
static KaiValue *kai_vec_set_impl(KaiValue *v, int64_t i, KaiValue *x) {
    kai_vec_bounds(v, i, "vec_set");
    v = kai_vec_ensure_unique(v, v->as.vec.cap);
    KaiVecMeta *m = kai_vec_meta(v);
    char *slot = kai_vec_elems(v) + (size_t) i * (size_t) m->stride;
    if (m->ekind == KAI_VEC_EK_BOXED) {
        KaiValue *old;
        memcpy(&old, slot, sizeof(KaiValue *));
        kai_vec_write_elem(m, slot, x);
        kai_decref(old);
    } else {
        kai_vec_write_elem(m, slot, x);
    }
    return v;
}

/* Pure append. Consumes `v` and `x`. Unique with spare capacity is a
 * plain in-place append; unique at capacity grows the block (doubling);
 * shared copies first. An empty PENDING vec classifies from `x`. */
static KaiValue *kai_vec_push_impl(KaiValue *v, KaiValue *x) {
    if (!v || kai_is_value(v) || v->tag != KAI_VEC) {
        kai_eprintf("kai: vec_push: not a vec\n"); kai_exit(1);
    }
    int64_t len = v->as.vec.len;
    {
        /* Grow only at capacity: an unconditional cap*2 here would double
         * the clone's capacity on EVERY shared push (2^n blow-up). */
        int64_t ncap = v->as.vec.cap;
        if (len >= ncap) {
            ncap = ncap * 2;
            if (ncap < len + 1) ncap = len + 1;
            if (ncap < 4) ncap = 4;
        }
        v = kai_vec_ensure_unique(v, ncap);
    }
    KaiVecMeta *m = kai_vec_meta(v);
    if (m->ekind == KAI_VEC_EK_PENDING) {
        kai_vec_classify(m, x);
        int64_t ncap = v->as.vec.cap;
        if (ncap < 4) ncap = 4;
        m = kai_vec_grow_cap(v, ncap);
    } else if (len == v->as.vec.cap) {
        int64_t ncap = v->as.vec.cap * 2;
        if (ncap < 4) ncap = 4;
        m = kai_vec_grow_cap(v, ncap);
    }
    kai_vec_write_elem(m, kai_vec_elems(v) + (size_t) len * (size_t) m->stride, x);
    v->as.vec.len = len + 1;
    return v;
}

/* ---------- Vec raw element paths (compiler-fused) ----------
 *
 * The compiler fuses `vec_get(v, i).f` and `vec_push(v, Rec{..})` on
 * inline-record carriers into these entry points so no boxed record is
 * built per access. Only the element MOVE is raw: every write still
 * rides kai_vec_ensure_unique — the ownership decision is never
 * skipped. Field index trusts the declaration-order column layout,
 * the same bet the constant-index record read (kai_op_field_at)
 * already makes; ekind/arity/tag are checked and trap on mismatch. */

static void kai_index_check_owned(int64_t len, int64_t i, KaiValue *c, KaiValue *x);

/* Store `n` unpacked scalar fields into one REC slot; consumes each x. */
static void kai_vec_store_rec_fields(KaiVecMeta *m, char *slot, int64_t n,
                                     KaiValue **xs) {
    if (m->ekind != KAI_VEC_EK_REC || (int64_t) m->n_fields != n)
        kai_trap_abort("vec: record shape mismatch");
    for (int64_t k = 0; k < n; ++k) {
        if (kai_vec_scalar_tag(xs[k]) != m->field_tags[k])
            kai_trap_abort("vec: record shape mismatch");
        int64_t bits = kai_vec_scalar_bits(xs[k], m->field_tags[k]);
        memcpy(slot + (size_t) k * 8, &bits, 8);
        kai_decref(xs[k]);
    }
}

/* Fused `vec_push(v, Rec{f0: x0, ...})`: consumes v and every x; the
 * record is never allocated. A PENDING vec classifies REC directly
 * from the unpacked fields (`names` must be static literals); the
 * head tag is 0, mirroring kai_record's anonymous-literal stamp. */
static KaiValue *kai_vec_push_rec_raw(KaiValue *v, int64_t n,
                                      KaiValue **xs, const char **names) {
    if (!v || kai_is_value(v) || v->tag != KAI_VEC) {
        kai_eprintf("kai: vec_push: not a vec\n"); kai_exit(1);
    }
    int64_t len = v->as.vec.len;
    {
        /* Grow only at capacity — see kai_vec_push_impl. */
        int64_t ncap = v->as.vec.cap;
        if (len >= ncap) {
            ncap = ncap * 2;
            if (ncap < len + 1) ncap = len + 1;
            if (ncap < 4) ncap = 4;
        }
        v = kai_vec_ensure_unique(v, ncap);
    }
    KaiVecMeta *m = kai_vec_meta(v);
    if (m->ekind == KAI_VEC_EK_PENDING) {
        if (n < 1 || n > KAI_VEC_REC_MAX)
            kai_trap_abort("vec: record shape mismatch");
        m->ekind = KAI_VEC_EK_REC;
        m->n_fields = (int32_t) n;
        m->head_tag = 0;
        m->stride = n * 8;
        for (int64_t k = 0; k < n; ++k) {
            int ft = kai_vec_scalar_tag(xs[k]);
            if (ft < 0) kai_trap_abort("vec: record shape mismatch");
            m->field_tags[k] = (uint8_t) ft;
            m->names[k] = names[k];
        }
        int64_t ncap = v->as.vec.cap;
        if (ncap < 4) ncap = 4;
        m = kai_vec_grow_cap(v, ncap);
    } else if (len == v->as.vec.cap) {
        int64_t ncap = v->as.vec.cap * 2;
        if (ncap < 4) ncap = 4;
        m = kai_vec_grow_cap(v, ncap);
    }
    kai_vec_store_rec_fields(m, kai_vec_elems(v) + (size_t) len * (size_t) m->stride,
                             n, xs);
    v->as.vec.len = len + 1;
    return v;
}

/* Fused `vec_set(v, i, Rec{...})`: consumes v, i, and every x. */
static KaiValue *kai_vec_set_rec_raw(KaiValue *v, KaiValue *i, int64_t n,
                                     KaiValue **xs) {
    int64_t idx = (kai_is_int(i)) ? kai_intf(i) : 0;
    if (i) kai_decref(i);
    if (kai_is_ptr(v) && v->tag == KAI_VEC && (uint64_t) idx >= (uint64_t) v->as.vec.len) {
        for (int64_t k = 0; k < n; ++k) kai_decref(xs[k]);
        kai_index_check_owned(v->as.vec.len, idx, v, NULL);
    }
    kai_vec_bounds(v, idx, "vec_set");
    v = kai_vec_ensure_unique(v, v->as.vec.cap);
    KaiVecMeta *m = kai_vec_meta(v);
    kai_vec_store_rec_fields(m, kai_vec_elems(v) + (size_t) idx * (size_t) m->stride,
                             n, xs);
    return v;
}

