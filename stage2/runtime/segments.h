/* Fragment of runtime.h; include runtime.h, never this file. */
/* =================================================================
 * Stack segments
 * =================================================================
 *
 * A segment runs a body closure on its own mmap'd stack inside the current
 * fiber, sharing the fiber's heap. A suspend switches back to
 * whoever started or last resumed it and hands that side a one-shot
 * continuation; `kai_seg_resume` switches in again from wherever it is
 * called, re-parenting the segment's evidence chain onto the resumer's.
 * A segment's stack never moves or grows: C frames hold interior pointers
 * into it.
 *
 * Each switch exchanges the per-context half of the fiber (KaiSegCtx), so
 * the segment keeps its own unwind stack, pending clause tails and stack
 * bounds. A non-local exit that targets a frame outside the running segment
 * — a cancellation, a trap, a handle further out — unwinds the segment to
 * its entry and is continued by the resumer on its own stack: no `longjmp`
 * crosses stacks.
 *
 * The continuation is a KAI_CONT box naming its suspended segment; a resume
 * or a discontinue empties it, so a second use traps. Dropping a box that
 * still names a segment discontinues it: the segment unwinds, running every
 * `finally` and releasing through the unwind stack, and its stack goes back
 * to the per-thread pool. */

KAI_SCHED_FN KaiValue *kai_seg_start(KaiValue *body, KaiValue **k);
KAI_SCHED_FN KaiValue *kai_seg_resume(KaiValue *k, KaiValue *v, KaiValue **k_out);

/* What a forwarding clause hands its handle: op `op`'s `n` arguments, and
 * `to`, the node the clause was dispatched through (NULL: the running
 * segment's own). It lives on the suspended segment's stack; the
 * handle takes the arguments before it resumes. */
typedef struct { int op; int n; KaiValue **args; KaiEvidence *to; } KaiSegRequest;

#if !KAI_SCHED_DECL_ONLY && defined(KAI_SEG_SWITCH)

#define KAI_SEG_POOL_MAX 16

#if defined(KAI_SEPARATE_COMPILATION)
extern KAI_TLS KaiSegment *kai_seg_pool;
extern KAI_TLS int         kai_seg_pool_n;
#  if defined(KAI_RUNTIME_OWNER)
KAI_TLS KaiSegment *kai_seg_pool   = NULL;
KAI_TLS int         kai_seg_pool_n = 0;
#  endif
#else
static KAI_TLS KaiSegment *kai_seg_pool   = NULL;
static KAI_TLS int         kai_seg_pool_n = 0;
#endif

static void kai_seg_ctx_save(KaiFiber *f, KaiSegCtx *c) {
    c->evidence_top     = f->evidence_top;
    c->in_dispatch_node = f->in_dispatch_node;
    c->unw_buf          = f->unw_buf;
    c->unw_top          = f->unw_top;
    c->unw_cap          = f->unw_cap;
    c->rframe_buf       = f->rframe_buf;
    c->rframe_top       = f->rframe_top;
    c->rframe_cap       = f->rframe_cap;
    c->stack_base       = f->stack_base;
    c->stack_size       = f->stack_size;
    c->unwind_frame     = f->unwind_frame;
    c->tsan_fiber       = f->tsan_fiber;
    c->seg              = f->seg;
}

static void kai_seg_ctx_load(KaiFiber *f, const KaiSegCtx *c) {
    f->evidence_top     = c->evidence_top;
    f->in_dispatch_node = c->in_dispatch_node;
    f->unw_buf          = c->unw_buf;
    f->unw_top          = c->unw_top;
    f->unw_cap          = c->unw_cap;
    f->rframe_buf       = c->rframe_buf;
    f->rframe_top       = c->rframe_top;
    f->rframe_cap       = c->rframe_cap;
    f->stack_base       = c->stack_base;
    f->stack_size       = c->stack_size;
    f->unwind_frame     = c->unwind_frame;
    f->tsan_fiber       = c->tsan_fiber;
    f->seg              = c->seg;
}

#if defined(KAI_ASAN_FIBERS)
#  define KAI_SEG_ASAN_LEAVE(save, bottom, size) __sanitizer_start_switch_fiber((save), (bottom), (size))
#  define KAI_SEG_ASAN_ARRIVE(save, bottom, size) __sanitizer_finish_switch_fiber((save), (bottom), (size))
#else
#  define KAI_SEG_ASAN_LEAVE(save, bottom, size)  ((void) 0)
#  define KAI_SEG_ASAN_ARRIVE(save, bottom, size) ((void) 0)
#endif

/* Pool access stays inside these two activations, which never switch. */
__attribute__((noinline))
static KaiSegment *kai_seg_pool_take(void) {
    KaiSegment *seg = kai_seg_pool;
    if (seg != NULL) {
        kai_seg_pool = seg->pool_next;
        kai_seg_pool_n--;
        return seg;
    }
    seg = (KaiSegment *) calloc(1, sizeof *seg);
    if (seg == NULL) {
        kai_eputs("kai: out of memory allocating a stack segment\n");
        kai_exit(1);
    }
    kai_install_fiber_sigsegv_handler();
    seg->own.stack_size = kai_fiber_stack_size();
    seg->own.stack_base = kai_stack_map(seg->own.stack_size);
    return seg;
}

static void kai_seg_unmap(KaiSegment *seg) {
    while (seg->relays != NULL) {
        KaiSegRelay *r = seg->relays;
        seg->relays = r->next;
        free(r);
    }
#if defined(__wasm__)
    free(seg->own.stack_base);
#else
    munmap(seg->own.stack_base, seg->own.stack_size + kai_page_size());
#endif
    free(seg->own.unw_buf);
    free(seg->own.rframe_buf);
    free(seg);
}

__attribute__((noinline))
static void kai_seg_pool_put(KaiSegment *seg) {
    if (kai_seg_pool_n < KAI_SEG_POOL_MAX) {
        seg->pool_next = kai_seg_pool;
        kai_seg_pool = seg;
        kai_seg_pool_n++;
        return;
    }
    kai_seg_unmap(seg);
}

/* A segment released on another thread lands in that thread's pool, so a
 * worker's pool can hold stacks it never mapped. */
KAI_SCHED_FN void kai_seg_pool_drain(void) {
    while (kai_seg_pool != NULL) {
        KaiSegment *seg = kai_seg_pool;
        kai_seg_pool = seg->pool_next;
        kai_seg_unmap(seg);
    }
    kai_seg_pool_n = 0;
}

static void kai_seg_release(KaiSegment *seg) {
    KaiValue *body = seg->body;
    KaiValue *spare = seg->spare;
    for (KaiSegRelay *r = seg->relays; r != NULL; r = r->next) r->node.eff_label = NULL;
    seg->body  = NULL;
    seg->xfer  = NULL;
    seg->spare = NULL;
    if (spare != NULL) kai_decref(spare);
    KAI_SEG_LSAN_UNROOT(seg);
#if defined(KAI_TSAN_FIBERS)
    /* A finished segment's entry frames never returned: start afresh. */
    __tsan_destroy_fiber(seg->own.tsan_fiber);
    seg->own.tsan_fiber = NULL;
#endif
    kai_seg_pool_put(seg);
    kai_decref(body);
}

/* Hang the segment's evidence chain from `base`, the resumer's top. */
static void kai_seg_rebase(KaiSegment *seg, KaiEvidence *base) {
    if (seg->base == base) return;
    KaiEvidence **link = &seg->own.evidence_top;
    while (*link != seg->base) link = &(*link)->parent;
    *link = base;
    seg->base = base;
}

static void kai_seg_land(void *arg) {
    _longjmp(((KaiSegment *) arg)->pad, 1);
}

/* Unwind the running segment to its entry, releasing what its frames own and
 * running every `finally` on its chain. */
__attribute__((noreturn))
static void kai_seg_unwind(KaiFiber *f, KaiSegment *seg) {
    kai_evidence_unwind_all();
    seg->state = KAI_SEG_UNWOUND;
    seg->xfer  = NULL;
    kai_trap_unwind(f->unwind_frame, f->stack_base, kai_seg_land, seg);
}

/* Switch from the running segment back to its resumer for good. */
__attribute__((noreturn))
static void kai_seg_leave(KaiSegment *seg) {
    KaiFiber *f = seg->fiber;
    kai_seg_ctx_save(f, &seg->own);
    kai_seg_ctx_load(f, &seg->outer);
    KAI_SEG_ASAN_LEAVE(NULL, seg->asan_outer_bottom, seg->asan_outer_size);
#if defined(KAI_TSAN_FIBERS)
    __tsan_switch_to_fiber(seg->tsan_outer, 0);
#endif
#if defined(__wasm__)
    kai_seg_switch_final(seg->outer_sp);
#endif
    kai_seg_switch(&seg->sp, seg->outer_sp, NULL);
    __builtin_unreachable();
}

static void kai_seg_entry(void *arg) {
    KaiSegment *seg = (KaiSegment *) arg;
    KAI_SEG_ASAN_ARRIVE(NULL, &seg->asan_outer_bottom, &seg->asan_outer_size);
    seg->fiber->unwind_frame = (uintptr_t) __builtin_frame_address(0);
    if (_setjmp(seg->pad) == 0) {
        seg->xfer  = kai_apply_borrow(seg->body, 0, NULL);
        seg->state = KAI_SEG_RETURNED;
    }
    kai_seg_leave(seg);
}

/* Continue on this side a non-local exit that left a segment. */
__attribute__((noreturn))
static void kai_seg_rethrow(KaiFiber *f, KaiEvidence *to, int trap) {
    if (to != NULL) {
        kai_evidence_unwind_to(to);
        _longjmp(*to->handle_jmp, 1);
    }
    kai_evidence_unwind_all();
    if (trap) kai_trap_unwind(f->unwind_frame, f->stack_base, kai_trap_land_fiber, f);
    kai_fiber_pad_jump(f, 0);
}

/* The runtime owner is built unoptimised: the per-element switch path stays
 * in one frame. */
static inline __attribute__((always_inline)) KaiValue *kai_seg_suspend_on(KaiFiber *f, KaiValue *v, KaiSegState state) {
    KaiSegment *seg = f->seg;
    if (seg == NULL) {
        kai_eputs("kai: internal error: suspend outside a stack segment\n");
        kai_exit(1);
    }
    if (seg->discontinue) kai_trap_abort("continuation suspended while being discontinued");
    seg->xfer  = v;
    seg->state = state;
    kai_seg_ctx_save(f, &seg->own);
    kai_seg_ctx_load(f, &seg->outer);
    void *fake = NULL;
    KAI_SEG_ASAN_LEAVE(&fake, seg->asan_outer_bottom, seg->asan_outer_size);
#if defined(KAI_TSAN_FIBERS)
    __tsan_switch_to_fiber(seg->tsan_outer, 0);
#endif
    void *r = kai_seg_switch(&seg->sp, seg->outer_sp, NULL);
    KAI_SEG_ASAN_ARRIVE(fake, &seg->asan_outer_bottom, &seg->asan_outer_size);
    if (seg->discontinue) kai_seg_unwind(seg->fiber, seg);
    return (KaiValue *) r;
}

/* Suspend for a request: this context's segment when its chain holds the
 * forwarding node, else hand the request on to the resumer, below. */
static inline __attribute__((always_inline)) KaiValue *kai_seg_ask_on(KaiFiber *f, KaiSegRequest *req) {
    KaiEvidence *to = req->to;
    int own = to == NULL || to == f->evidence_top || f->seg == NULL || kai_seg_holds(f, kai_ev_real(to));
    return kai_seg_suspend_on(f, (KaiValue *) req, own ? KAI_SEG_SUSPENDED : KAI_SEG_FORWARD);
}

/* Serve on this side a request `seg` forwarded, returning its answer. `seg`
 * is held as a continuation meanwhile: an exit that skips this frame
 * discontinues it. */
__attribute__((noinline))
static KaiValue *kai_seg_serve(KaiFiber *f, KaiSegment *seg) {
    KaiValue *held[1] = { kai_alloc(KAI_CONT) };
    held[0]->as.seg = seg;
    KaiFiber *uf = kai_unw_push(held, 1);
    KaiValue *v = kai_seg_ask_on(f, (KaiSegRequest *) seg->xfer);
    kai_unw_pop(uf);
    held[0]->as.seg = NULL;
    kai_decref(held[0]);
    return v;
}

/* Switch into `seg` with `arg` and report how it came back. */
static KaiValue *kai_seg_run(KaiFiber *f, KaiSegment *seg, void *arg, KaiValue **k) {
  for (;;) {
    /* The runtime owner is unoptimised: keep the common switch-in inline. */
    KaiEvidence *skip;
    KaiEvidence *view = f->in_dispatch_node != NULL && f->seg != NULL ? kai_ev_view(f, &skip) : f->evidence_top;
    kai_seg_ctx_save(f, &seg->outer);
    kai_seg_rebase(seg, view);
    if (seg->relays != NULL) kai_seg_reaim(seg);
    kai_seg_ctx_load(f, &seg->own);
    seg->state = KAI_SEG_RUNNING;
    void *outer_fake = NULL;
    KAI_SEG_ASAN_LEAVE(&outer_fake, (char *) seg->own.stack_base + kai_page_size(), seg->own.stack_size);
#if defined(KAI_TSAN_FIBERS)
    seg->tsan_outer = __tsan_get_current_fiber();
    __tsan_switch_to_fiber(seg->own.tsan_fiber, 0);
#endif
    kai_seg_switch(&seg->outer_sp, seg->sp, arg);
    KAI_SEG_ASAN_ARRIVE(outer_fake, NULL, NULL);
    switch (seg->state) {
        case KAI_SEG_SUSPENDED: {
            KaiValue *box = seg->spare;
            if (box != NULL) seg->spare = NULL;
            else box = kai_alloc(KAI_CONT);
            box->as.seg = seg;
            *k = box;
            return seg->xfer;
        }
        case KAI_SEG_FORWARD:
            arg = kai_seg_serve(f, seg);
            continue;
        case KAI_SEG_ESCAPED: {
            KaiEvidence *to = seg->escape_to;
            int trap = seg->trap;
            kai_seg_release(seg);
            kai_seg_rethrow(f, to, trap);
        }
        default: {
            KaiValue *v = seg->xfer;
            kai_seg_release(seg);
            *k = NULL;
            return v;
        }
    }
  }
}

/* Empty a continuation box, consuming it, and return its segment. */
static KaiSegment *kai_seg_take(KaiValue *k) {
    if (k == NULL || k->tag != KAI_CONT) {
        kai_eputs("kai: internal error: resuming a value that is not a continuation\n");
        kai_exit(1);
    }
    KaiSegment *seg = k->as.seg;
    k->as.seg = NULL;
    if (seg == NULL) {
        kai_decref(k);
        kai_trap_abort("continuation resumed twice");
    }
    /* A box only this call held cannot be seen again: keep it for reuse. */
    if (kai_rc_load(k) == 1 && seg->spare == NULL) seg->spare = k;
    else kai_decref(k);
    if (seg->fiber != kai_current_fiber())
        kai_trap_abort("continuation resumed outside the fiber that captured it");
    return seg;
}

static void kai_seg_discontinue_seg(KaiSegment *seg) {
    KaiValue *k = NULL;
    seg->discontinue = 1;
    (void) kai_seg_run(seg->fiber, seg, NULL, &k);
}

/* Run `body`, a nullary closure the segment takes over, on a fresh segment.
 * Returns what the body returned with `*k` NULL, or the value it suspended
 * with and `*k` the continuation. */
KAI_SCHED_FN KaiValue *kai_seg_start(KaiValue *body, KaiValue **k) {
#if defined(__wasm__)
    kai_wctx_mark();
#endif
    KaiFiber *f = kai_current_fiber();
    KaiSegment *seg = kai_seg_pool_take();
    seg->fiber       = f;
    seg->body        = body;
    seg->xfer        = NULL;
    seg->discontinue = 0;
    seg->escape_to   = NULL;
    seg->trap        = 0;
    seg->base        = f->evidence_top;
    seg->own.evidence_top     = f->evidence_top;
    seg->own.in_dispatch_node = NULL;
    seg->own.unw_top          = 0;
    seg->own.rframe_top       = 0;
    seg->own.seg              = seg;
#if defined(KAI_TSAN_FIBERS)
    seg->own.tsan_fiber       = __tsan_create_fiber(0);
#endif
    KAI_SEG_LSAN_ROOT(seg);
    seg->sp = kai_seg_frame_init((char *) seg->own.stack_base + kai_page_size() + seg->own.stack_size,
                                 kai_seg_entry);
    return kai_seg_run(f, seg, seg, k);
}

/* Apply the thunk of a lazy cell on a fresh segment; `thunk` is borrowed.
 * A thunk performs nothing, so its segment never suspends. */
KAI_SCHED_FN KaiValue *kai_seq_apply_far(KaiValue *thunk) {
    KaiValue *k = NULL;
    KaiValue *step = kai_seg_start(kai_incref(thunk), &k);
    if (k != NULL) {
        kai_eputs("kai: internal error: a lazy cell's thunk suspended\n");
        kai_exit(1);
    }
    return step;
}

/* A forwarding clause: hand op `op`'s `n` arguments (consumed) to the handle
 * and return the value its continuation is called with. */
KAI_SCHED_FN KaiValue *kai_seg_request(int op, int n, KaiValue **args) {
    KaiFiber *f = kai_current_fiber();
    KaiSegRequest req = { op, n, args, f->in_dispatch_node };
    return kai_seg_ask_on(f, &req);
}

/* Resume `k` (consumed) with `v` (owned). Returns like kai_seg_start. */
KAI_SCHED_FN KaiValue *kai_seg_resume(KaiValue *k, KaiValue *v, KaiValue **k_out) {
    KaiSegment *seg = kai_seg_take(k);
    return kai_seg_run(seg->fiber, seg, v, k_out);
}

/* The last reference to an unresumed continuation went away. The free walk
 * that got here holds thread-local addresses, so the fiber stays on this
 * thread until the segment has unwound. */
KAI_SCHED_FN void kai_seg_cont_drop(KaiValue *box) {
    KaiSegment *seg = box->as.seg;
    if (seg == NULL) return;
    box->as.seg = NULL;
    KaiFiber *f = seg->fiber;
    if (f != kai_current_fiber()) kai_trap_abort("continuation dropped outside the fiber that captured it");
    f->seg_pin++;
    kai_seg_discontinue_seg(seg);
    f->seg_pin--;
}

/* A non-local exit leaving the running segment: unwind it to its entry; the
 * resumer continues the exit. `to` is the handle node the exit lands on, NULL
 * for the fiber's cancel pad (whose callers have already unwound). */
KAI_SCHED_FN void kai_seg_escape(KaiFiber *f, KaiEvidence *to, int trap) {
    KaiSegment *seg = f->seg;
    if (to != NULL) kai_evidence_unwind_all();
    seg->state     = KAI_SEG_ESCAPED;
    seg->escape_to = to;
    seg->trap      = trap;
    _longjmp(seg->pad, 1);
}

#elif !KAI_SCHED_DECL_ONLY

/* No switch for this target: no segment ever runs, so no box or escape
 * reaches the two hooks below. */
__attribute__((noreturn)) static void kai_seg_unsupported(void) {
#if defined(__wasi__) && !KAI_HOST_JS
    kai_eputs("kai: fibers and kept continuations are not available on wasm32-wasi\n");
#else
    kai_eputs("kai: stack segments are not supported on this target\n");
#endif
    kai_exit(1);
    __builtin_unreachable();
}
KAI_SCHED_FN KaiValue *kai_seg_start(KaiValue *body, KaiValue **k) { (void) body; (void) k; kai_seg_unsupported(); }
KAI_SCHED_FN KaiValue *kai_seq_apply_far(KaiValue *thunk) { return kai_apply_borrow(thunk, 0, NULL); }
KAI_SCHED_FN KaiValue *kai_seg_resume(KaiValue *k, KaiValue *v, KaiValue **k_out) {
    (void) k; (void) v; (void) k_out; kai_seg_unsupported();
}
KAI_SCHED_FN KaiValue *kai_seg_request(int op, int n, KaiValue **args) {
    (void) op; (void) n; (void) args; kai_seg_unsupported();
}
KAI_SCHED_FN void kai_seg_cont_drop(KaiValue *box) { (void) box; }
KAI_SCHED_FN void kai_seg_pool_drain(void) {}
KAI_SCHED_FN void kai_seg_escape(KaiFiber *f, KaiEvidence *to, int trap) {
    (void) f; (void) to; (void) trap; kai_seg_unsupported();
}

#endif

/* ---------- Handles that run their body on a segment ----------
 *
 * A handle whose clauses keep `resume` beyond a tail call lowers to
 * kai_seg_handle. The body runs on a segment under a forwarding handler
 * whose clauses call kai_seg_request; each request runs the real clause
 * here with a continuation closure. Calling that closure resumes the body
 * and handles whatever it does next, so the handler stays deep wherever the
 * continuation is called from. */

KAI_SCHED_FN KaiValue *kai_seg_handle(KaiValue *body, KaiValue *ret, int n, KaiValue **clauses);
KAI_SCHED_FN KaiValue *kai_seg_request(int op, int n, KaiValue **args);

#if !KAI_SCHED_DECL_ONLY

static KaiValue *kai_seg_handle_step(KaiValue *handler, KaiValue *v, KaiValue *k);

/* The handler: the return clause (NULL passes the body's value on), then one
 * clause per op. Never called. */
static KaiValue *kai_seg_handler_fn(KaiValue *self, KaiValue **args, int n) {
    (void) self; (void) args; (void) n;
    kai_eputs("kai: internal error: a segment handler called as a function\n");
    kai_exit(1);
}

/* A continuation: the box of the suspended body and its handler. */
static KaiValue *kai_seg_cont_call(KaiValue *self, KaiValue **args, int n) {
    (void) n;
    KaiValue *handler = self->as.clo.captures[1];
    KaiValue *k = NULL;
    KaiValue *v = kai_seg_resume(kai_incref(self->as.clo.captures[0]), args[0], &k);
    return kai_seg_handle_step(handler, v, k);
}

/* A finished body runs the return clause; a request runs its clause with
 * a continuation that owns the box. Consumes `v` and `k`. */
static KaiValue *kai_seg_handle_step(KaiValue *handler, KaiValue *v, KaiValue *k) {
    KaiValue **caps = handler->as.clo.captures;
    if (k == NULL) return caps[0] ? kai_apply_borrow(caps[0], 1, &v) : v;
    KaiSegRequest *req = (KaiSegRequest *) v;
    KaiValue *held[2] = { k, handler };
    KaiValue *cont = kai_closure(kai_seg_cont_call, 1, 2, held);
    kai_decref(k);
    KaiValue *argv[req->n + 1];
    for (int i = 0; i < req->n; i++) argv[i] = req->args[i];
    argv[req->n] = cont;
    return kai_apply_borrow(caps[1 + req->op], req->n + 1, argv);
}

/* Run `body` under clauses that may keep their continuation. Consumes every
 * argument. */
KAI_SCHED_FN KaiValue *kai_seg_handle(KaiValue *body, KaiValue *ret, int n, KaiValue **clauses) {
    KaiValue *caps[n + 1];
    caps[0] = ret;
    for (int i = 0; i < n; i++) caps[1 + i] = clauses[i];
    KaiValue *handler = kai_closure(kai_seg_handler_fn, 0, n + 1, caps);
    for (int i = 0; i <= n; i++) if (caps[i]) kai_decref(caps[i]);
    KaiValue *k = NULL;
    KaiValue *v = kai_seg_start(body, &k);
    KaiValue *r = kai_seg_handle_step(handler, v, k);
    kai_decref(handler);
    return r;
}


#endif

/* Walk the current fiber's stack and return the innermost handler
 * for `eff_label`. Returns NULL if no matching handler is in
 * scope — which would indicate a compiler bug, since the type
 * checker rejects unhandled effects. The fast path is pointer
 * equality (most labels are literal strings shared at the call
 * site); strcmp is the fallback for edge cases like dynamically-
 * generated label strings. */

/* The evidence of the builtin `Cancel`. The runtime dispatches a user
 * `with Cancel { raise(_) -> ... }` clause itself when a cancellation
 * lands at a yield point or a scope exit, so the generated C names this
 * struct for Cancel instead of emitting its own: the clause is then
 * called through its exact function type. The layout is the Ev-struct
 * convention (`handler_id`, `env`, `state`, then the op pointers). */
typedef struct KaiRtEvCancel KaiRtEvCancel;
struct KaiRtEvCancel {
    KaiHandlerId handler_id;
    void        *env;
    KaiValue    *state;
    KaiValue   *(*raise)(KaiRtEvCancel *self, KaiCont *k);
};

/* An exit to `node`'s handle abandons the scopes opened inside its body. */
static void kai_nursery_abandon_inside(KaiFiber *f, KaiEvidence *node) {
    for (;;) {
        KaiNursery *scope = f->nursery_top;
        int inside = 0;
        for (KaiEvidence *n = scope ? scope->ev_top : NULL; n && n != KAI_EV_RELAY; n = n->parent)
            if (n == node) { inside = 1; break; }
        if (!inside) return;
        kai_nursery_abandon_top(f);
    }
}

/* Walk the current fiber's evidence stack and, if a user `with Cancel`
 * handler is in scope, dispatch its `raise` clause and longjmp to the
 * handle's landing pad — the call does not return. Returns 0 when no
 * user handler is in scope, leaving the caller to pick its own
 * fallback (cancel pad, banner, exit).
 *
 * A user handler is one with a live `handle_jmp`; default Cancel
 * handlers allocate no jmp_buf because they never longjmp out of their
 * clause. The walk skips `in_dispatch_node` so a Cancel handler that is
 * itself mid-dispatch resolves to an outer Cancel frame instead of
 * recursing into itself — the same per-fiber rule
 * `kai_evidence_lookup_node` enforces for user-driven dispatch. */
static int kai_cancel_dispatch_user_handler(void) {
    KaiFiber    *f    = kai_current_fiber();
    KaiEvidence *node = f->evidence_top;
    KaiEvidence *user_node = NULL;
    while (node) {
        if (node != kai_ev_real(f->in_dispatch_node)
            && node->handle_jmp != NULL
            && node->eff_label == KAI_EFF("Cancel")) {
            user_node = node;
            break;
        }
        node = node->parent;
    }
    if (user_node == NULL) return 0;

    /* Mirrors the op-call shape emitted for `Cancel.raise()`: bind an
     * identity continuation, mark the node in-dispatch across the call,
     * invoke the clause, and — `resume` discarded, the only legal
     * outcome because raise() returns `Nothing` — store the discarded
     * value, pop evidence, and longjmp to the handle's landing pad. */
    KaiRtEvCancel *ev = (KaiRtEvCancel *) user_node->handler;
    KaiCont k;
    kai_cont_init_identity(&k, ev->handler_id);

    KaiEvidence *saved_disp = f->in_dispatch_node;
    f->in_dispatch_node = user_node;
    KaiValue *op_r = ev->raise(ev, &k);
    f->in_dispatch_node = saved_disp;

    if (k.status == KAI_CONT_UNRESUMED && user_node->handle_jmp != NULL) {
        *user_node->discard_slot = op_r;
        kai_evidence_unwind_to(user_node);
        _longjmp(*user_node->handle_jmp, 1);
        /* Unreachable. */
    }

    /* Defensive: a Cancel clause that calls `resume(_)` is a compiler
     * bug — raise() returns Nothing, so there is no value to feed back.
     * Report no handler so the caller takes its terminal path. */
    return 0;
}

/* Deliver a requested cancellation at a yield point. Every effect-op
 * call goes through a kai_evidence_lookup* function, which makes those
 * the natural delivery points. With the pad unset (the root fiber, or
 * outside trampoline scope) the check falls through and dispatch
 * proceeds normally; a later `Cancel.raise()` still reaches the default
 * handler. */
static __attribute__((noinline)) void kai_cancel_deliver(KaiFiber *f) {
    if (!kai_cancel_due(f)) return;

    /* Delivered marker is flipped *before* invoking the clause. The
     * clause body may call into ops that re-enter
     * kai_check_cancel_yield_point — without the early flip those
     * re-entries would see the flag still set and try to dispatch
     * again. */
    f->cancel_delivered = 1;

    (void) kai_cancel_dispatch_user_handler();

    /* No user handler in scope (or a clause that illegally resumed):
     * fall back to the pad — the trampoline's second return marks the
     * fiber CANCELLED and continues with the awaiter walk. */
    if (!f->cancel_pad_set) {
        kai_test_cancel_escape();
        kai_eputs("kai: Cancel.raise: unhandled (fiber cancelled)\n");
        kai_exit(0);
    }
    kai_evidence_unwind_all();
    kai_fiber_pad_jump(f, 0);
    /* Unreachable. */
}

/* A yield point on the fiber `f` the caller already holds. */
static inline void kai_cancel_poll(KaiFiber *f) {
    if (__builtin_expect(atomic_load_explicit(&f->cancel_requested, memory_order_relaxed), 0))
        kai_cancel_deliver(f);
}

static void kai_check_cancel_yield_point(void) { kai_cancel_poll(kai_current_fiber()); }

/* Label text compared without interning, for names the compiler holds as
 * strings. Same relation as `kai_eff_match`. */
static int kai_eff_text_match(const char *a, const char *b) {
    if (strcmp(a, b) == 0) return 1;
    size_t n = strcspn(a, "[");
    return strncmp(a, b, n) == 0 && (a[n] == '\0' ? b[n] == '[' : b[n] == '\0');
}

static void *kai_evidence_lookup_l(const char *eff_label) {
    kai_check_cancel_yield_point();
    /* Issue #103 — Cancel-on-linked-trap-exit'd-peer must bypass
     * user handlers (see kai_check_trap_exit_cancel_bypass). */
    if (eff_label == KAI_EFF("Cancel")) {
        kai_check_trap_exit_cancel_bypass();
    }
    KaiFiber *f = kai_current_fiber();
    KaiEvidence *node = f->evidence_top;
    while (node != NULL) {
        if (kai_eff_match(node->eff_label, eff_label)) {
            return node->handler;
        }
        node = node->parent;
    }
    return NULL;
}

/* The node a perform of `eff_label` dispatches to, `def` when none handles it.
 * m8 bug #12: the walk skips the node whose clause is being dispatched on
 * this fiber, so a recursive op resolves to the outer handler. */
static KaiEvidence *kai_evidence_find_node(const char *eff_label, KaiEvidence *def) {
    KaiFiber *f = kai_current_fiber();
    kai_cancel_poll(f);
    /* Issue #103 — bypass user Cancel handlers when the current
     * fiber is linked to a trap-exit'd peer (see
     * kai_check_trap_exit_cancel_bypass for rationale). */
    if (eff_label == KAI_EFF("Cancel")) {
        kai_check_trap_exit_cancel_bypass();
        f = kai_current_fiber();
    }
    if (f->seg != NULL) return kai_seg_lookup(f, eff_label, def);
    KaiEvidence *node = kai_ev_find(f->evidence_top, NULL, eff_label, f->in_dispatch_node);
    return node != NULL ? node : def;
}

/* m7a #6e: same lookup but returns the whole node so the op-call
 * site can reach the handle's jmp_buf if a discard happens. */
static KaiEvidence *kai_evidence_lookup_node_l(const char *eff_label) {
    return kai_evidence_find_node(eff_label, NULL);
}

/* Resolve a default-bearing effect performed with no caller frame slot: a
 * lexical handler on the stack wins (the walk finds it), else the frame-supplied
 * default node (no longer pushed). The bridge for a direct perform in a fn that
 * carries no frame (e.g. `main`), where the walk alone would miss the default. */
static KaiEvidence *kai_evidence_lookup_or_default_l(const char *eff_label, KaiEvidence *def) {
    return kai_evidence_find_node(eff_label, def);
}

/* A fiber-local effect resolved to no handler — the perform site walked an
 * empty evidence stack. A capability does not cross a spawn, so a fiber-local
 * op (Actor/Cancel/Link/Monitor/Spawn) performed in a fiber whose own body
 * installed no handler for it has no disposition to bind. Report it instead of
 * dereferencing the NULL node. */
/* KAI_DEBUG_EVIDENCE=1 dumps the fiber and scheduler state behind an
 * unhandled fiber-local effect. Under M:N the one message covers causes that
 * need opposite fixes — the executing thread resolved the wrong fiber, the
 * right fiber with an empty chain, or a live handler masked by a stale
 * in_dispatch_node — and they are indistinguishable from the message alone.
 * noinline so the thread pointer is re-read here rather than hoisted from
 * the caller. Failure path only; no cost on the lookup. */
__attribute__((noinline))
static void kai_evidence_diag(KaiEvidence *node, const char *eff_label) {
    KaiFiber *f = kai_current_fiber();
    struct KaiMailbox *mb = f ? f->mailbox : NULL;
    kai_eprintf(
        "kai: evidence-diag eff=%s node=%p handler=%p\n"
        "  fiber=%p is_main=%d state=%d home_thread=%d thread_id=%d nthreads=%d\n"
        "  evidence_top=%p in_dispatch=%p parent=%p\n"
        "  mailbox=%p mb_owner=%p mb_owner_is_self=%d\n",
        eff_label, (void *)node, node ? (void *)node->handler : NULL,
        (void *)f, f == &kai_worker_here()->main_fiber, f ? (int)f->state : -1,
        f ? (int)f->home_thread : -1, kai_worker_here()->id, kai_nthreads,
        f ? (void *)f->evidence_top : NULL, f ? (void *)f->in_dispatch_node : NULL,
        f ? (void *)f->parent : NULL,
        (void *)mb, mb ? (void *)mb->owner_fiber : NULL,
        mb ? (mb->owner_fiber == f) : 0);
    int depth = 0;
    for (KaiEvidence *n = f ? f->evidence_top : NULL; n && depth < 16; n = n->parent) {
        kai_eprintf("  [%d] node=%p eff=%s handler=%p%s\n", depth, (void *)n,
                n->eff_label ? n->eff_label : "(null)", (void *)n->handler,
                n == f->in_dispatch_node ? "  <-- masked by in_dispatch" : "");
        depth++;
    }
}

static KaiEvidence *kai_evidence_require(KaiEvidence *node, const char *eff_label) {
    /* A NULL node (empty evidence stack) or a node whose handler slot was never
     * filled (a default global minted for a fiber-local effect that has no real
     * default) both mean "no disposition to bind here". Report either, instead
     * of letting a later `node->handler` deref segfault. */
    if (node == NULL || node->handler == NULL) {
        if (getenv("KAI_DEBUG_EVIDENCE")) kai_evidence_diag(node, eff_label);
        kai_eprintf("kai: effect not handled in fiber: %s\n", eff_label);
        kai_exit(1);
    }
    return node;
}

/* A by-id capability (a `var`/State/Reader cell or a `with Eff as a` alias)
 * resolved to no node — the alias's evidence is on the fiber where it was
 * installed and does not cross a spawn. A NULL here means the op ran on a
 * child fiber that does not carry the cell; report it instead of dereferencing
 * the NULL node. The compile-time escape check catches the common shapes; this
 * is the runtime floor for the ones it cannot see statically. */
static KaiEvidence *kai_evidence_require_reachable(KaiEvidence *node, const char *cap_name) {
    if (node == NULL || node->handler == NULL) {
        kai_eprintf("kai: capability not reachable in this fiber: %s\n", cap_name);
        kai_exit(1);
    }
    return node;
}

/* Resolve a whole evidence frame in one pass: one cancel poll and one fiber
 * fetch for every slot, and an empty evidence stack resolves each slot to its
 * default. Bit i of `unchecked` exempts slot i from the unhandled-effect
 * report. `labels` is the site's zero-initialised cache of the interned
 * `texts`. */
static KaiEvidence **kai_evidence_fill_frame_l(KaiEvidence **out, int n, const char *const *texts,
                                             const char **labels, KaiEvidence *const *defs, uint64_t unchecked) {
    kai_check_cancel_yield_point();
    KaiFiber *f = kai_current_fiber();
    int bare = f->seg == NULL && f->evidence_top == NULL;
    for (int i = 0; i < n; i++) {
        const char *label = kai_eff_cached(&labels[i], texts[i]);
        if (label == KAI_EFF("Cancel")) kai_check_trap_exit_cancel_bypass();
        KaiEvidence *node;
        if (bare) {
            node = defs[i];
        } else if (f->seg != NULL) {
            node = kai_seg_lookup(f, label, defs[i]);
        } else {
            node = kai_ev_find(f->evidence_top, NULL, label, f->in_dispatch_node);
            if (node == NULL) node = defs[i];
        }
        out[i] = (i < 64 && ((unchecked >> i) & 1)) ? node : kai_evidence_require(node, label);
    }
    return out;
}

/* m7b #15: per-instance dispatch — find the evidence node whose
 * handler_id matches `id`, no name match required. The codegen
 * uses this when the op call comes from a `with Eff as alias`
 * binding, so an outer alias's op stays reachable even after an
 * inner `with Eff as other` shadows the effect name. */
static KaiEvidence *kai_evidence_lookup_node_by_id(KaiHandlerId id) {
    kai_check_cancel_yield_point();
    KaiFiber *f = kai_current_fiber();
    KaiEvidence *skip = f->in_dispatch_node;
    KaiEvidence *node = f->seg != NULL ? kai_ev_view(f, &skip) : f->evidence_top;
    while (node != NULL) {
        /* m8 bug #12: same per-fiber skip rule as the by-name lookup. */
        if (node == skip) { node = node->parent; continue; }
        KaiHandlerId nid = ((KaiHandlerId *) node->handler)[0];
        if (nid == id) {
            /* Issue #103 — same Cancel bypass as the by-name path:
             * an aliased `with Cancel as e` op call on a fiber linked
             * to a trap-exit'd peer must unwind directly. */
            if (node->eff_label == KAI_EFF("Cancel")) {
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

