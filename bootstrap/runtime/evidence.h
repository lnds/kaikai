/* Fragment of runtime.h; include runtime.h, never this file. */
/* Tell `peer` that a fiber linked to it ended: a trap-exit'd peer with a
 * mailbox receives the reason string, any other is cancelled at its next
 * yield point. */
static void kai_link_notify(KaiFiber *peer, KaiExitReason reason, const char *trap_msg) {
    if (!(peer->trap_exit && peer->mailbox)) {
        kai_fiber_request_cancel(peer);
    } else if (reason == KAI_EXIT_TRAPPED) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Trapped: %s", trap_msg ? trap_msg : "runtime trap");
        kai_deliver_to_mailbox(peer->mailbox, kai_str_dyn(buf));
    } else {
        const char *txt = (reason == KAI_EXIT_NORMAL) ? "Normal" : "Crashed";
        kai_deliver_to_mailbox(peer->mailbox, kai_str(txt));
    }
}

static void kai_link_propagate_terminate(KaiFiber *self, KaiLinkNode *ln, KaiExitReason reason) {
    while (ln) {
        KaiLinkNode *next = ln->next;
        KaiFiber *peer = ln->peer;
        if (peer) {
            kai_link_notify(peer, reason, self->trap_msg);
            /* Remove our back-link from peer's chain (linear scan;
             * v1 chains are short — typically 1-2 entries). At N>1 a
             * link straddling two threads mutates the peer's chain from
             * this thread; cross-thread link teardown as a peer-owned
             * system message (design §5) is deferred — the propagation
             * signal itself is safe (cancel_requested is atomic, mailbox
             * delivery copies), only the far chain edit is unsynchronized.
             * Same-thread links (the N=1 case) are unaffected. */
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

/* Link the current fiber to `peer_pid`'s owner. A peer that has already
 * ended is reported at once, as if it had ended right after the call. */
static void kai_link_watch(KaiValue *peer_pid) {
    KaiFiberExit *x = peer_pid->as.pid.owner;
    if (!x) return;
    KaiFiber *self = kai_current_fiber();
    kai_fiber_exit_lock(x);
    KaiFiber     *peer   = x->fiber;
    KaiExitReason reason = (KaiExitReason) x->reason;
    const char   *msg    = x->msg;
    if (peer) kai_link_add_bidirectional(self, peer);
    kai_fiber_exit_unlock(x);
    if (!peer) kai_link_notify(self, reason, msg);
}

static KaiValue *kai_default_link_link(void *self, KaiValue *peer, KaiCont *k) {
    (void) self;
    if (peer && peer->tag == KAI_PID) kai_link_watch(peer);
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
 * walks its own fiber's evidence chain and lands in any
 * `with Cancel { raise(_) -> ... }` installed inside the fiber —
 * short-circuiting the trampoline-tail link propagation that would
 * have pushed `"Crashed"` into the supervisor's mailbox.
 *
 * The check fires only when the cancel_pad is live (so we have
 * somewhere to longjmp) and the fiber actually has a trap-exit'd
 * link (so this is the linkage-relevant case the issue scopes the
 * fix to). Plain Cancel.raise() inside a fiber that holds no
 * trap-exit'd link still dispatches through user handlers as before,
 * preserving the cleanup-on-cancel idiom for non-supervised work. */
static void kai_check_trap_exit_cancel_bypass(void) {
    KaiFiber *f = kai_current_fiber();
    if (!f || !f->cancel_pad_set) return;
    if (!kai_fiber_has_trap_exit_link(f)) return;
    f->cancel_delivered = 1;
    kai_evidence_unwind_all();
    kai_fiber_pad_jump(f, 0);
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
        kai_eprintf("kai: out of memory (monitor)\n");
        kai_exit(1);
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
/* Mark `self` ended on its exit record and detach its link and monitor
 * chains under the record's lock. kai_link_watch and kai_monitor_watch
 * place under the same lock, so each either lands on a chain detached
 * here or finds the fiber gone and reports the exit itself. */
static void kai_fiber_detach_watchers(KaiFiber *self, KaiExitReason reason,
                                      KaiLinkNode **links, KaiMonitorNode **monitors) {
    KaiFiberExit *x = atomic_load_explicit(&self->exit_rec, memory_order_acquire);
    if (x) {
        kai_fiber_exit_lock(x);
        x->fiber  = NULL;
        x->reason = reason;
        x->msg    = self->trap_msg;
    }
    *links    = self->linked_head;
    *monitors = self->monitor_head;
    self->linked_head  = NULL;
    self->monitor_head = NULL;
    if (x) kai_fiber_exit_unlock(x);
}

static void kai_monitor_propagate_terminate(KaiMonitorNode *mn) {
    while (mn) {
        KaiMonitorNode *next = mn->next;
        KaiFiber *observer = mn->observer;
        if (observer && observer->mailbox && mn->target_pid) {
            /* The mailbox takes ownership of the incref we stamped at
             * kai_monitor_add time; kai_deliver_to_mailbox consumes it
             * (copying across a thread boundary when needed). */
            kai_deliver_to_mailbox(observer->mailbox, mn->target_pid);
        } else if (mn->target_pid) {
            /* No mailbox to deliver into — drop our owning ref so
             * the pid value can be reclaimed. */
            kai_decref(mn->target_pid);
        }
        free(mn);
        mn = next;
    }
}

/* Watch `target_pid` from the current fiber. A target that has already
 * ended is reported at once, as if it had ended right after the call. */
static void kai_monitor_watch(KaiValue *target_pid) {
    KaiFiber     *observer = kai_current_fiber();
    KaiFiberExit *x        = target_pid->as.pid.owner;
    int           watching = 0;
    if (x) {
        kai_fiber_exit_lock(x);
        if (x->fiber) {
            kai_monitor_add(x->fiber, observer, target_pid);
            watching = 1;
        }
        kai_fiber_exit_unlock(x);
    }
    if (!watching && observer->mailbox) {
        kai_deliver_to_mailbox(observer->mailbox, kai_incref(target_pid));
    }
}

static KaiValue *kai_default_monitor_monitor(void *self, KaiValue *target, KaiCont *k) {
    (void) self;
    if (target && target->tag == KAI_PID) kai_monitor_watch(target);
    /* The ref is the monitored pid itself. The shim releases `target`
     * once this returns, so the ref carries its own reference. */
    return kai_cont_resume(k, kai_incref(target));
}

static KaiValue *kai_default_monitor_demonitor(void *self, KaiValue *ref, KaiCont *k) {
    (void) self;
    KaiFiberExit *x = (ref && ref->tag == KAI_PID) ? ref->as.pid.owner : NULL;
    if (x) {
        kai_fiber_exit_lock(x);
        if (x->fiber) kai_monitor_remove(x->fiber, kai_current_fiber(), ref);
        kai_fiber_exit_unlock(x);
    }
    return kai_cont_resume(k, kai_unit());
}

/* (typedef forward-declared above, before KaiFiber.) */
struct KaiEvidence {
    /* KaiValue's rc and tag, pinned: a capability is a node pointer passed
     * where a value goes, so a closure capturing one counts it like any
     * capture and the pin makes that a no-op. */
    int32_t      rc;
    uint8_t      tag;
    /* Slot of the fiber-local builtin this node handles, or -1. */
    int8_t       fl;
    KaiEvidence *parent;
    /* The slot's previous holder: the next-outer node of the same builtin. */
    KaiEvidence *fl_prev;
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
    /* `finally { }`: run when the scope exits, however it exits. The
     * thunk runs with `evidence_top` restored to this node's parent, so
     * it sees the handlers that were live when the handler was
     * installed — not those at the jump site, which are already dead.
     * NULL on every handler that declares no `finally`, which is what
     * keeps the unwind walk free for them. Returns the block's value,
     * which the caller drops — the clause is run for its effect. */
    KaiValue  *(*cleanup)(void *);
    void        *cleanup_env;
    /* Guards against running the same cleanup twice when a normal exit
     * races an unwind through the same node. */
    uint8_t      cleanup_done;
    /* Distance from `parent` to the node whose clause was running when this
     * one was pushed (1 = `parent`); 0 for none. An exit through this node
     * lands back inside that clause. */
    uint8_t      disp_hops;
    /* The fiber's unwind-stack height when the node was pushed: what lies
     * above it belongs to frames an exit through this node discards. */
    uint32_t     unw_mark;
    /* Position on the fiber's own chain: the root handler is 1. Orders two
     * nodes of one chain without walking it. */
    uint32_t     depth;
};

static KAI_RC_NOINLINE void kai_unw_grow(KaiFiber *f) {
    uint32_t cap = f->unw_cap ? f->unw_cap * 2 : 64;
    KaiUnwEntry *buf = (KaiUnwEntry *) realloc(f->unw_buf, (size_t) cap * sizeof(KaiUnwEntry));
    if (buf == NULL) {
        kai_eputs("kai: out of memory growing the unwind stack\n");
        kai_exit(1);
    }
    f->unw_buf = buf;
    f->unw_cap = cap;
}

/* Register `n` slots the calling frame owns across a call that can exit
 * non-locally. Returns the fiber so the pop needs no second lookup: the
 * fiber survives a migration across threads, a cached TLS address does not. */
static inline KaiFiber *kai_unw_push_on(KaiFiber *f, KaiValue **base, intptr_t n) {
    if (f->unw_top == f->unw_cap) kai_unw_grow(f);
    f->unw_buf[f->unw_top].base = base;
    f->unw_buf[f->unw_top].n    = n;
    f->unw_top++;
    return f;
}

static inline KaiFiber *kai_unw_push(KaiValue **base, intptr_t n) {
    return kai_unw_push_on(kai_current_fiber(), base, n);
}

/* `kai_unw_push` with the fiber looked up once per C frame: `*cache`
 * starts NULL and a frame never changes fiber. */
static inline KaiFiber *kai_unw_push_c(KaiFiber **cache, KaiValue **base, intptr_t n) {
    KaiFiber *f = *cache;
    if (__builtin_expect(f == NULL, 0)) f = *cache = kai_current_fiber();
    return kai_unw_push_on(f, base, n);
}

static inline void kai_unw_pop(KaiFiber *f) { f->unw_top--; }

static void kai_resume_frames_drop(KaiFiber *f, KaiEvidence *node);

/* An entry that names something other than slots: a handle's drops the
 * resume frames it was draining, a lazy cell's gives the cell its thunk
 * back, a constant's leaves it unevaluated. Out of line, so the loop that
 * releases slots pays one test for all three. */
__attribute__((noinline))
static void kai_unw_release_named(KaiFiber *f, KaiUnwEntry e) {
    if (e.n == KAI_UNW_HANDLE) kai_resume_frames_drop(f, (KaiEvidence *) e.base);
    else if (e.n == KAI_UNW_SEQ) kai_seq_abandon((KaiValue *) e.base);
    else kai_const_abandon(f, (KaiValue *) e.base);
}

/* Release every slot above `mark`, innermost frame first. */
KAI_RT_SHARED void kai_unw_release_to(KaiFiber *f, uint32_t mark) {
    while (f->unw_top > mark) {
        KaiUnwEntry e = f->unw_buf[--f->unw_top];
        if (e.n < 0) kai_unw_release_named(f, e);
        for (intptr_t i = 0; i < e.n; i++) {
            if (e.base[i] != NULL) kai_decref(e.base[i]);
        }
    }
}

/* Run one node's `finally` in its installation-time evidence context.
 * Restoring `evidence_top` to `node->parent` is load-bearing: a cleanup
 * that performs an effect must dispatch against the handlers live when
 * the node was pushed, otherwise it walks a chain of dead frames. */
static inline void kai_fl_pop(KaiFiber *f, KaiEvidence *node);

static void kai_evidence_run_cleanup(KaiEvidence *node) {
    if (node == NULL || node->cleanup == NULL || node->cleanup_done) return;
    node->cleanup_done = 1;
    KaiFiber *f = kai_current_fiber();
    KaiEvidence *saved = f->evidence_top;
    KaiEvidence *saved_fl = node->fl >= 0 ? f->fl_top[node->fl] : NULL;
    f->evidence_top = node->parent;
    kai_fl_pop(f, node);
    KaiValue *r = node->cleanup(node->cleanup_env);
    f->evidence_top = saved;
    if (node->fl >= 0) f->fl_top[node->fl] = saved_fl;
    if (r != NULL) kai_decref(r);
}

static uint8_t kai_disp_hops(KaiFiber *f);

/* ---------- Interned effect labels ----------
 *
 * An evidence label names an effect instance: `Eff` or `Eff[args]`. Every
 * label the evidence API takes is interned: equal text is one pointer, and a
 * header just before the text points at the interned `Eff` part. Two labels
 * match when equal, or when they share `Eff` and one of them is the bare
 * `Eff` (an instance the compiler could not pin). A site interns its literal
 * once through `KAI_EFF`; a lookup then never reads label text. */
typedef struct KaiEffLabel {
    const char          *base;
    struct KaiEffLabel  *next;
    int                  fl;     /* fiber-local slot of the effect, or -1 */
    char                 text[];
} KaiEffLabel;

static const char *const kai_fl_names[KAI_FL_COUNT] = { "Cancel", "Link", "Monitor", "Spawn", "Actor" };

#define KAI_EFF_BUCKETS 1024

#if defined(KAI_SEPARATE_COMPILATION)
extern KaiEffLabel    *kai_eff_table[KAI_EFF_BUCKETS];
extern pthread_mutex_t kai_eff_mu;
#  if defined(KAI_RUNTIME_OWNER)
KaiEffLabel    *kai_eff_table[KAI_EFF_BUCKETS];
pthread_mutex_t kai_eff_mu = PTHREAD_MUTEX_INITIALIZER;
#  endif
#else
static KaiEffLabel    *kai_eff_table[KAI_EFF_BUCKETS];
static pthread_mutex_t kai_eff_mu = PTHREAD_MUTEX_INITIALIZER;
#endif

static inline const char *kai_eff_base(const char *label) {
    return ((const KaiEffLabel *) (label - offsetof(KaiEffLabel, text)))->base;
}

static const char *kai_eff_intern_locked(const char *text, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) h = (h ^ (unsigned char) text[i]) * 16777619u;
    KaiEffLabel **bucket = &kai_eff_table[h % KAI_EFF_BUCKETS];
    for (KaiEffLabel *e = *bucket; e != NULL; e = e->next) {
        if (strncmp(e->text, text, len) == 0 && e->text[len] == '\0') return e->text;
    }
    KaiEffLabel *e = (KaiEffLabel *) malloc(sizeof *e + len + 1);
    if (e == NULL) {
        kai_eputs("kai: out of memory interning an effect label\n");
        kai_exit(1);
    }
    memcpy(e->text, text, len);
    e->text[len] = '\0';
    size_t n = strcspn(e->text, "[");
    e->base = n == len ? e->text : kai_eff_intern_locked(text, n);
    e->fl = -1;
    for (int i = 0; i < KAI_FL_COUNT; i++)
        if (strcmp(e->base, kai_fl_names[i]) == 0) e->fl = i;
    e->next = *bucket;
    *bucket = e;
    return e->text;
}

static const char *kai_eff_intern(const char *text) {
    pthread_mutex_lock(&kai_eff_mu);
    const char *l = kai_eff_intern_locked(text, strlen(text));
    pthread_mutex_unlock(&kai_eff_mu);
    return l;
}

/* The interned `text`, memoised in the caller's `slot`. */
static inline const char *kai_eff_cached(const char **slot, const char *text) {
    const char *l = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (__builtin_expect(l == NULL, 0)) {
        l = kai_eff_intern(text);
        __atomic_store_n(slot, l, __ATOMIC_RELEASE);
    }
    return l;
}

#define KAI_EFF(lit) ({ static const char *_kai_efl; kai_eff_cached(&_kai_efl, lit); })

static inline int kai_eff_fl(const char *label) {
    return ((const KaiEffLabel *) (label - offsetof(KaiEffLabel, text)))->fl;
}

static inline int kai_eff_match(const char *a, const char *b) {
    if (a == b) return 1;
    const char *ba = kai_eff_base(a), *bb = kai_eff_base(b);
    return ba == bb && (a == ba || b == bb);
}

/* A fiber-local builtin's node is linked into its slot on push and unlinked
 * on every exit from the chain; any other node takes no slot. */
static inline void kai_fl_push(KaiFiber *f, KaiEvidence *node) {
    int i = kai_eff_fl(node->eff_label);
    node->depth = node->parent != NULL ? node->parent->depth + 1 : 1;
    node->fl = (int8_t) i;
    if (i >= 0) {
        node->fl_prev = f->fl_top[i];
        f->fl_top[i] = node;
    }
}

static inline void kai_fl_pop(KaiFiber *f, KaiEvidence *node) {
    if (node->fl >= 0) f->fl_top[node->fl] = node->fl_prev;
}

/* Push an Evidence node onto the current fiber's stack. The caller
 * owns the node's storage — typically `alloca`'d inside a compiled
 * `handle` prologue. This primitive only fills its fields and
 * links it as the new top. */
static void kai_evidence_push_l(KaiEvidence *node, const char *eff_label, void *handler) {
    KaiFiber *f = kai_current_fiber();
    node->parent       = f->evidence_top;
    node->eff_label    = eff_label;
    node->handler      = handler;
    node->handle_jmp   = NULL;
    node->discard_slot = NULL;
    node->cleanup      = NULL;
    node->cleanup_env  = NULL;
    node->cleanup_done = 0;
    node->disp_hops    = kai_disp_hops(f);
    node->unw_mark     = f->unw_top;
    node->rc           = INT32_MAX;
    node->tag          = KAI_UNIT;
    f->evidence_top    = node;
    kai_fl_push(f, node);
}
/* Connect a frame-supplied default node to its Ev WITHOUT pushing it on the
 * evidence stack: the call site addresses this node directly through the frame,
 * so it needs `handler` / `eff_label` set and the discard fields NULL, but no
 * `parent` / `evidence_top` linkage (a default never discards, never unwinds). */
static void kai_evidence_init_default_l(KaiEvidence *node, const char *eff_label, void *handler) {
    node->parent       = NULL;
    node->eff_label    = eff_label;
    node->handler      = handler;
    node->handle_jmp   = NULL;
    node->discard_slot = NULL;
    node->cleanup      = NULL;
    node->cleanup_env  = NULL;
    node->cleanup_done = 0;
    node->disp_hops    = 0;
    node->fl           = -1;
    node->unw_mark     = 0;
    node->rc           = INT32_MAX;
    node->tag          = KAI_UNIT;
}

/* m7a #6e: like kai_evidence_push but also stamps the handle's
 * setjmp/longjmp landing pad. The op-call site uses the node's
 * `handle_jmp` to abandon the body when the clause discards
 * `resume`. Default handlers (m7a #7) always resume, so they call
 * the simpler push variant. */
static void kai_evidence_push_with_jmp_l(KaiEvidence *node, const char *eff_label,
                                          void *handler, jmp_buf *jmp,
                                          KaiValue **discard_slot) {
    KaiFiber *f = kai_current_fiber();
    node->parent       = f->evidence_top;
    node->eff_label    = eff_label;
    node->handler      = handler;
    node->handle_jmp   = jmp;
    node->discard_slot = discard_slot;
    node->cleanup      = NULL;
    node->cleanup_env  = NULL;
    node->cleanup_done = 0;
    node->disp_hops    = kai_disp_hops(f);
    node->unw_mark     = f->unw_top;
    node->rc           = INT32_MAX;
    node->tag          = KAI_UNIT;
    f->evidence_top    = node;
    kai_fl_push(f, node);
}

/* Arm a pushed node's `finally`. Separate from the push so the common
 * handler — no `finally` — keeps the existing two-call prologue and the
 * cleanup fields stay NULL. */
static void kai_evidence_set_cleanup(KaiEvidence *node, KaiValue *(*fn)(void *), void *env) {
    node->cleanup      = fn;
    node->cleanup_env  = env;
    node->cleanup_done = 0;
}

/* Pop the topmost Evidence node. The compiled `handle` epilogue
 * always pairs with a matching push (Doc C §*Per-fiber isolation*
 * balance invariant). Pop on an empty stack is a compiler bug; it
 * silently no-ops here so a malformed prologue/epilogue cannot
 * corrupt unrelated memory. */
static void kai_evidence_pop(void) {
    KaiFiber *f = kai_current_fiber();
    if (f->evidence_top != NULL) {
        kai_fl_pop(f, f->evidence_top);
        f->evidence_top = f->evidence_top->parent;
    }
}

/* ---------- Evidence across stack segments ----------
 *
 * Code on a segment keeps evidence nodes in its frames across a suspension,
 * and the segment may be resumed under other handlers. So a lookup on a
 * segment that resolves below the segment's base returns a relay: a node the
 * segment owns, standing for that handler. Every switch-in re-aims each relay
 * from the new base, so a perform through one reaches the handler current at
 * the resume, never one captured before. Relays are off every chain.
 *
 * A clause of a handler below the running segment runs in that handler's
 * context: its lookups start under the handler, so it never reaches the
 * segment's own handlers and cannot suspend the segment out from under the
 * handler. */

typedef struct KaiSegRelay {
    KaiEvidence         node;     /* first: a relay is used as its node */
    KaiEvidence        *target;   /* the chain node it stands for */
    KaiEvidence        *def;      /* the default it falls back to, or NULL */
    struct KaiSegRelay *next;
} KaiSegRelay;

/* A relay is on no chain: its parent slot holds this mark instead. */
#define KAI_EV_RELAY ((KaiEvidence *) (uintptr_t) 1)

/* The chain node `n` stands for. */
static inline KaiEvidence *kai_ev_real(KaiEvidence *n) {
    return n != NULL && n->parent == KAI_EV_RELAY ? ((KaiSegRelay *) n)->target : n;
}

static uint8_t kai_disp_hops(KaiFiber *f) {
    KaiEvidence *disp = kai_ev_real(f->in_dispatch_node);
    if (disp == NULL) return 0;
    unsigned hops = 1;
    for (KaiEvidence *n = f->evidence_top; n != NULL && n != KAI_EV_RELAY; n = n->parent, hops++)
        if (n == disp) return hops < 255 ? (uint8_t) hops : 0;
    return 0;
}

/* The dispatch mask in force when `node` was pushed. */
static KaiEvidence *kai_disp_at_push(KaiEvidence *node) {
    KaiEvidence *n = node->parent;
    for (unsigned h = node->disp_hops; h > 1 && n != NULL; h--) n = n->parent;
    return node->disp_hops ? n : NULL;
}

static int kai_ev_on_chain(KaiEvidence *n, KaiEvidence *stop, KaiEvidence *node) {
    for (; n != stop && n != NULL; n = n->parent) {
        if (n == node) return 1;
    }
    return 0;
}

/* The nearest node handling `eff_label` from `n` down to `stop`, in the
 * context of the clause `skip` is dispatching. That clause runs outside its
 * handler: `skip` and the nodes above it that were already pushed when the
 * clause began (handlers nested inside the handle) are out of reach. The
 * nodes the clause pushed since remember `skip` in their dispatch mask. A
 * `skip` pushed by another clause was itself outside that clause's handler,
 * so the same rule continues from there. */
static KaiEvidence *kai_ev_find(KaiEvidence *n, KaiEvidence *stop, const char *eff_label, KaiEvidence *skip) {
    KaiEvidence *cur = skip != NULL && kai_ev_on_chain(n, stop, skip) ? skip : NULL;
    for (; n != stop; n = n->parent) {
        if (n == cur) {
            cur = kai_disp_at_push(n);
            if (cur != NULL && !kai_ev_on_chain(n->parent, stop, cur)) cur = NULL;
            continue;
        }
        if (cur != NULL && kai_disp_at_push(n) != cur) continue;
        if (kai_eff_match(n->eff_label, eff_label)) return n;
    }
    return NULL;
}

/* `kai_ev_find` over the fiber's whole chain. A fiber-local builtin resolves
 * through its slot's chain of same-effect nodes, whatever the handler depth,
 * applying the dispatch mask of `kai_ev_find` as the candidates descend: the
 * mask changes only at the dispatching nodes, so `cur` advances through them
 * by depth instead of by walking every node between. A segment keeps the
 * full walk. */
static KaiEvidence *kai_ev_find_fiber(KaiFiber *f, const char *eff_label) {
    int i = kai_eff_fl(eff_label);
    if (i < 0 || f->seg != NULL)
        return kai_ev_find(f->evidence_top, NULL, eff_label, f->in_dispatch_node);
    KaiEvidence *cur = f->in_dispatch_node;
    for (KaiEvidence *n = f->fl_top[i]; n != NULL; n = n->fl_prev) {
        while (cur != NULL && cur->depth > n->depth) cur = kai_disp_at_push(cur);
        if (cur != NULL && (n == cur || kai_disp_at_push(n) != cur)) continue;
        if (kai_eff_match(n->eff_label, eff_label)) return n;
    }
    return NULL;
}

/* Whether `node` is on the chain a context with this top and segment owns;
 * the fiber's own stack owns everything below. */
static int kai_ctx_holds(KaiEvidence *top, KaiSegment *seg, KaiEvidence *node) {
    if (seg == NULL) return 1;
    for (KaiEvidence *n = top; n != seg->base; n = n->parent) {
        if (n == node) return 1;
    }
    return 0;
}

/* Whether `node` is on the running segment's own chain. */
static int kai_seg_holds(KaiFiber *f, KaiEvidence *node) {
    return kai_ctx_holds(f->evidence_top, f->seg, node);
}

/* The context owning `node`: the running one (NULL) or a suspended resumer's. */
static KaiSegCtx *kai_ctx_of(KaiFiber *f, KaiEvidence *node) {
    if (kai_seg_holds(f, node)) return NULL;
    KaiSegCtx *c = &f->seg->outer;
    while (!kai_ctx_holds(c->evidence_top, c->seg, node)) c = &c->seg->outer;
    return c;
}

/* Where a lookup in the running context starts and the node it skips. */
static KaiEvidence *kai_ev_view(KaiFiber *f, KaiEvidence **skip) {
    KaiEvidence *d = f->in_dispatch_node;
    if (f->seg == NULL || d == NULL || kai_seg_holds(f, kai_ev_real(d))) {
        *skip = kai_ev_real(d);
        return f->evidence_top;
    }
    /* A default node is on no chain: its clause runs from the base. */
    KaiEvidence *r = kai_ev_real(d);
    *skip = NULL;
    return r->parent != NULL ? r->parent : f->seg->base;
}

/* The node a lookup below `seg`'s base skips: the resumer's in-dispatch
 * node, unless the segment hangs from that clause's handler context. */
static KaiEvidence *kai_seg_skip(KaiSegment *seg) {
    return seg->base == seg->outer.evidence_top ? kai_ev_real(seg->outer.in_dispatch_node) : NULL;
}

static void kai_seg_relay_aim(KaiSegRelay *r, KaiEvidence *to) {
    r->target            = to;
    r->node.handler      = to->handler;
    r->node.handle_jmp   = to->handle_jmp;
    r->node.discard_slot = to->discard_slot;
}

__attribute__((noreturn))
static void kai_seg_unhandled(const char *eff_label) {
    kai_eprintf("kai: a continuation was resumed or dropped where `%s` is not handled\n", eff_label);
    kai_exit(1);
    __builtin_unreachable();
}

/* `seg`'s relay for `eff_label`, made on first use; NULL when nothing below
 * the segment handles it and there is no default. */
static KaiEvidence *kai_seg_relay(KaiSegment *seg, const char *eff_label, KaiEvidence *def) {
    KaiSegRelay *spare = NULL;
    for (KaiSegRelay *r = seg->relays; r != NULL; r = r->next) {
        if (r->node.eff_label == NULL) spare = r;
        else if (r->node.eff_label == eff_label) return &r->node;
    }
    KaiEvidence *to = kai_ev_find(seg->base, NULL, eff_label, kai_seg_skip(seg));
    if (to == NULL) to = def;
    if (to == NULL) return NULL;
    KaiSegRelay *r = spare;
    if (r == NULL) {
        r = (KaiSegRelay *) calloc(1, sizeof *r);
        if (r == NULL) {
            kai_eputs("kai: out of memory allocating an evidence relay\n");
            kai_exit(1);
        }
        r->node.parent       = KAI_EV_RELAY;
        r->node.cleanup_done = 1;
        r->next = seg->relays;
        seg->relays = r;
    }
    r->node.eff_label = eff_label;
    r->def = def;
    kai_seg_relay_aim(r, to);
    return &r->node;
}

/* Re-aim `seg`'s relays from its base, just rebased. */
static void kai_seg_reaim(KaiSegment *seg) {
    for (KaiSegRelay *r = seg->relays; r != NULL; r = r->next) {
        if (r->node.eff_label == NULL) continue;
        KaiEvidence *to = kai_ev_find(seg->base, NULL, r->node.eff_label, kai_seg_skip(seg));
        if (to == NULL) to = r->def;
        if (to == NULL) kai_seg_unhandled(r->node.eff_label);
        kai_seg_relay_aim(r, to);
    }
}

/* A lookup on a segment: the running context's chain, then the relay of the
 * segment whose base the walk leaves. */
static KaiEvidence *kai_seg_lookup(KaiFiber *f, const char *eff_label, KaiEvidence *def) {
    int i = kai_eff_fl(eff_label);
    if (i >= 0 && f->in_dispatch_node == NULL) {
        for (KaiEvidence *n = f->fl_top[i]; n != NULL; n = n->fl_prev) {
            if (kai_eff_match(n->eff_label, eff_label)) return n;
        }
        return kai_seg_relay(f->seg, eff_label, def);
    }
    KaiEvidence *skip;
    KaiEvidence *from = kai_ev_view(f, &skip);
    KaiSegCtx *c = from == f->evidence_top ? NULL : kai_ctx_of(f, from);
    KaiSegment *seg = c == NULL ? f->seg : c->seg;
    KaiEvidence *n = kai_ev_find(from, seg != NULL ? seg->base : NULL, eff_label, skip);
    if (n != NULL) return n;
    if (seg == NULL) return def;
    return kai_seg_relay(seg, eff_label, def);
}

/* A non-tail `resume` defers the rest of its clause: the clause pushes it
 * here, on the node whose clause is running, and the handle applies it to
 * its result on exit. The frame goes to the context owning the node, whose
 * handle reads it there. Takes the clause's reference to the closure. */
static KaiValue *kai_resume_frame_push(KaiValue *clo) {
    KaiFiber *f = kai_current_fiber();
    KaiEvidence *node = kai_ev_real(f->in_dispatch_node);
    if (node == NULL) {
        kai_eputs("kai: internal error: a resume frame pushed outside a handler clause\n");
        kai_exit(1);
    }
    KaiSegCtx *c = f->seg != NULL ? kai_ctx_of(f, node) : NULL;
    KaiResumeFrame **buf_p = c ? &c->rframe_buf : &f->rframe_buf;
    uint32_t *top_p = c ? &c->rframe_top : &f->rframe_top;
    uint32_t *cap_p = c ? &c->rframe_cap : &f->rframe_cap;
    if (*top_p == *cap_p) {
        uint32_t cap = *cap_p ? *cap_p * 2 : 16;
        KaiResumeFrame *buf = (KaiResumeFrame *) realloc(*buf_p, (size_t) cap * sizeof(KaiResumeFrame));
        if (buf == NULL) {
            kai_eputs("kai: out of memory growing the resume frames\n");
            kai_exit(1);
        }
        *buf_p = buf;
        *cap_p = cap;
    }
    (*buf_p)[*top_p].node = node;
    (*buf_p)[*top_p].clo  = clo;
    (*top_p)++;
    return kai_unit();
}

/* Take `node`'s innermost frame off the fiber's stack, or NULL. */
static KaiValue *kai_resume_frame_take(KaiFiber *f, KaiEvidence *node) {
    for (uint32_t i = f->rframe_top; i > 0; i--) {
        if (f->rframe_buf[i - 1].node != node) continue;
        KaiValue *clo = f->rframe_buf[i - 1].clo;
        memmove(&f->rframe_buf[i - 1], &f->rframe_buf[i],
                (size_t) (f->rframe_top - i) * sizeof(KaiResumeFrame));
        f->rframe_top--;
        return clo;
    }
    return NULL;
}

static int kai_resume_frames_pending(KaiFiber *f, KaiEvidence *node) {
    for (uint32_t i = f->rframe_top; i > 0; i--) {
        if (f->rframe_buf[i - 1].node == node) return 1;
    }
    return 0;
}

/* Feed a handle's result through its pending frames, innermost first:
 * each one's value is the next one's `resume` result. Runs after the
 * node is popped, so a frame performing the handled effect reaches the
 * handler outside. While it runs, the node is gone from the evidence
 * chain, so an unwind entry stands in for it: an exit that jumps past
 * releases the frames not yet run. Consumes `v`. */
static KaiValue *kai_resume_frames_run(KaiEvidence *node, KaiValue *v) {
    KaiFiber *f = kai_current_fiber();
    if (!kai_resume_frames_pending(f, node)) return v;
    KaiFiber *uf = kai_unw_push_on(f, (KaiValue **) node, KAI_UNW_HANDLE);
    for (;;) {
        /* Re-read: a frame may park and resume on another thread. */
        KaiValue *clo = kai_resume_frame_take(kai_current_fiber(), node);
        if (clo == NULL) break;
        /* Held across the call: a frame that exits past this handle must
         * still release its own closure. */
        KaiValue *held[1] = { clo };
        KaiFiber *cf = kai_unw_push(held, 1);
        KaiValue *argv[1] = { v };
        v = kai_apply_borrow(clo, 1, argv);
        kai_unw_pop(cf);
        kai_decref(clo);
    }
    kai_unw_pop(uf);
    return v;
}

/* Release `node`'s frames: an exit that skips its handle abandons them. */
static void kai_resume_frames_drop(KaiFiber *f, KaiEvidence *node) {
    KaiValue *clo;
    while ((clo = kai_resume_frame_take(f, node)) != NULL) kai_decref(clo);
}

/* Unwind the evidence stack past `node` — the abandon path, where a
 * clause discarded `resume` and the op site longjmps to `node`'s handle.
 * That jump destroys every frame the handle body pushed, `node` included,
 * so popping a single entry is only correct when `node` is already the
 * top. With a resumed handler frame still between the op site and its
 * handle, a lone pop leaves the dead node linked as top: the next handle
 * to reuse that stack storage links itself as its own parent, and every
 * later lookup walks the resulting cycle forever. */
static void kai_nursery_abandon_inside(KaiFiber *f, KaiEvidence *node);

static void kai_evidence_unwind_to(KaiEvidence *node) {
    node = kai_ev_real(node);
    if (node == NULL) return;
    kai_nursery_abandon_inside(kai_current_fiber(), node);
    /* Run every `finally` between the jump site and the target, innermost
     * first, before the chain is truncated. `node` itself is included:
     * the jump lands past its handler, so its scope is exiting too.
     * Bounded by the same chain the truncation already walked implicitly;
     * a handler with no `finally` costs one NULL check. */
    KaiFiber *f = kai_current_fiber();
    if (f->seg != NULL && !kai_seg_holds(f, node)) kai_seg_escape(f, node, 0);
    KaiEvidence *stop = node->parent;
    for (KaiEvidence *n = f->evidence_top; n != NULL && n != stop; n = n->parent) {
        kai_unw_release_to(f, n->unw_mark);
        kai_evidence_run_cleanup(n);
        /* `node`'s own frames survive: its landing pad applies them. */
        if (n != node) kai_resume_frames_drop(f, n);
        kai_fl_pop(f, n);
    }
    f->evidence_top = stop;
    f->in_dispatch_node = kai_disp_at_push(node);
}

/* Cancellation longjmps straight to the fiber's `cancel_pad` without a
 * target node, so it needs the whole remaining chain drained. */
KAI_RT_SHARED void kai_evidence_unwind_all(void) {
    KaiFiber *f = kai_current_fiber();
    /* Inside a segment the chain below its base belongs to the resumer. */
    KaiEvidence *stop = f->seg != NULL ? f->seg->base : NULL;
    for (KaiEvidence *n = f->evidence_top; n != stop; n = n->parent) {
        kai_unw_release_to(f, n->unw_mark);
        kai_evidence_run_cleanup(n);
        kai_resume_frames_drop(f, n);
        kai_fl_pop(f, n);
    }
    kai_unw_release_to(f, 0);
    f->evidence_top = stop;
}

