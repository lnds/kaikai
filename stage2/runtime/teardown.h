/* Fragment of runtime.h; include runtime.h, never this file. */
/* ==================================================================
 * Runtime shutdown. Each thread of the scheduler releases what only it
 * reaches as it leaves the scheduler; thread 0 then releases the shared
 * state, with the others joined. A release leaves its holder empty, so
 * code that runs afterwards (a foreign callback) rebuilds it. Only a run
 * of main that returned gets here: an exit from inside one leaves other
 * threads running over all of this.
 * ================================================================== */
#if !KAI_SCHED_DECL_ONLY

/* A build with KAI_TEARDOWN_AUDIT stops the process when a holder is still
 * full: LeakSanitizer sees neither a mapping nor memory a root still names. */
#ifdef KAI_TEARDOWN_AUDIT
static void kai_teardown_expect(int released, const char *what) {
    if (released) return;
    kai_eprintf("kai: teardown: %s still held\n", what);
    _exit(70);
}
#else
#  define kai_teardown_expect(released, what) ((void) 0)
#endif

/* What a finished fiber holds beside its stack and its struct. */
static void kai_fiber_release(KaiFiber *f) {
    KaiLinkNode    *links;
    KaiMonitorNode *monitors;
    kai_fiber_detach_watchers(f, KAI_EXIT_NORMAL, &links, &monitors);
    kai_link_chain_free(links);
    while (monitors != NULL) {
        KaiMonitorNode *next = monitors->next;
        if (monitors->target_pid) kai_decref(monitors->target_pid);
        kai_fiber_exit_unref(monitors->observer);
        free(monitors);
        monitors = next;
    }
    kai_fiber_consts_drop(f);
    kai_fiber_exit_orphan(f);
    atomic_store(&f->exit_rec, NULL);
    free(f->unw_buf);
    f->unw_buf = NULL;
    f->unw_top = f->unw_cap = 0;
    free(f->rframe_buf);
    f->rframe_buf = NULL;
    f->rframe_top = f->rframe_cap = 0;
}

/* The fiber main ran on above one thread. */
static void kai_boot_root_release(void) {
    KaiFiber *root = kai_boot_root;
    if (root == NULL) return;
    kai_boot_root = NULL;
    kai_fiber_release(root);
#if KAI_HOST_FIBERS
    if (root->stack_base) kai_stack_unmap(root->stack_base, root->stack_size);
#endif
    kai_tsan_fiber_free(root);
    free(root->free_stack);
    free(root);
}

/* An alternate stack this thread installed. One it no longer has installed
 * stays: whoever replaced it may put it back. */
static void kai_sigaltstack_release(KaiWorker *w) {
#if KAI_HOST_FIBERS && !defined(__wasm__)
    if (w->sigalt_stack == NULL) return;
    stack_t cur;
    if (sigaltstack(NULL, &cur) != 0 || cur.ss_sp != w->sigalt_stack || (cur.ss_flags & SS_ONSTACK)) return;
    stack_t off;
    memset(&off, 0, sizeof off);
    off.ss_flags = SS_DISABLE;
    off.ss_size  = cur.ss_size;
    if (sigaltstack(&off, NULL) != 0) return;
    free(w->sigalt_stack);
    w->sigalt_stack = NULL;
    w->sigalt_ready = 0;
#else
    (void) w;
#endif
}

#ifdef KAI_CELL_POOL_ACTIVE
static pthread_mutex_t kai_slab_kept_mu = PTHREAD_MUTEX_INITIALIZER;
static char          **kai_slab_kept;
static size_t          kai_slab_kept_n;

/* A leaving thread's slabs stay for the process: their blocks migrate to other
 * threads, so none can be freed, and the list moves here to keep naming them.
 * Slab state is thread-local: this activation never switches. */
__attribute__((noinline))
static void kai_slab_hand_over(void) {
    if (kai_slab_count == 0) return;
    pthread_mutex_lock(&kai_slab_kept_mu);
    char **grown = (char **) realloc(kai_slab_kept, (kai_slab_kept_n + (size_t) kai_slab_count) * sizeof *grown);
    if (grown == NULL) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
    memcpy(grown + kai_slab_kept_n, kai_slab_list, (size_t) kai_slab_count * sizeof *grown);
    kai_slab_kept = grown;
    kai_slab_kept_n += (size_t) kai_slab_count;
    pthread_mutex_unlock(&kai_slab_kept_mu);
    free(kai_slab_list);
    kai_slab_list = NULL;
    kai_slab_count = kai_slab_cap = 0;
    kai_bheap.slab_cur = NULL;
    kai_bheap.slab_off = 0;
}
#endif

/* Reads thread-local state: this activation never switches. */
__attribute__((noinline))
static void kai_thread_pools_audit(int id) {
    kai_teardown_expect(!kai_seg_pool, "a pooled stack segment");
#ifdef KAI_CELL_POOL_ACTIVE
    kai_teardown_expect(!kai_cell_pool, "a cell pool");
    kai_teardown_expect(id == 0 || !kai_slab_list, "a worker's slab list");
#endif
    (void) id;
}

/* The calling thread's own holdings. Every fiber it ran has ended. The
 * signal stack goes last: the releases before it may still overflow. */
KAI_SCHED_FN void kai_thread_release(KaiWorker *w) {
    kai_drain_pending_free(w);
    kai_fiber_release(&w->main_fiber);
    kai_seg_pool_drain();
#ifdef KAI_CELL_POOL_ACTIVE
    kai_cell_pool_release();
    if (w->id != 0) kai_slab_hand_over();
#endif
    kai_sigaltstack_release(w);
    kai_teardown_expect(!w->pending_free && !w->pending_sched_drop, "a finished fiber");
    kai_teardown_expect(!w->main_fiber.unw_buf && !w->main_fiber.rframe_buf, "a root fiber's buffers");
    kai_teardown_expect(!atomic_load(&w->main_fiber.exit_rec), "a root fiber's exit record");
    kai_thread_pools_audit(w->id);
    kai_teardown_expect(!w->sigalt_stack, "a signal stack");
}

/* The reactor's growable buffers. No fiber is left to wait on them. */
static void kai_reactor_release(void) {
    free(kai_reactor_ready_batch);
    kai_reactor_ready_batch = NULL;
    kai_reactor_ready_n = kai_reactor_ready_cap = 0;
    if (kai_reactor_timers_n == 0) {
        free(kai_reactor_timers);
        kai_reactor_timers = NULL;
        kai_reactor_timers_cap = 0;
    }
    while (kai_reactor_pending_exits != NULL) {
        KaiPendingExit *dead = kai_reactor_pending_exits;
        kai_reactor_pending_exits = dead->next;
        free(dead);
    }
}

#if KAI_HOST_STACK_GUARD
static int kai_stack_slot_order(const void *a, const void *b) {
    uintptr_t x = (uintptr_t) *(void *const *) a, y = (uintptr_t) *(void *const *) b;
    return (x > y) - (x < y);
}
#endif

/* Unmaps the pooled stacks, one call per run of neighbours. A stack still in
 * use is not in the pool and stays mapped. */
static void kai_stack_pool_release(void) {
#if KAI_HOST_STACK_GUARD
    KaiStackPool *pl = &kai_stack_pool;
    pthread_mutex_lock(&pl->mu);
    if (pl->n_free > 0) {
        size_t bytes = kai_fiber_stack_size() + kai_page_size();
        qsort(pl->free_slots, pl->n_free, sizeof *pl->free_slots, kai_stack_slot_order);
        for (size_t i = 0, j; i < pl->n_free; i = j) {
            for (j = i + 1; j < pl->n_free && (char *) pl->free_slots[j] == (char *) pl->free_slots[j - 1] + bytes; j++) { }
            munmap(pl->free_slots[i], (j - i) * bytes);
        }
        pl->vmas_used -= 2 * pl->n_free;
        pl->n_free = 0;
    }
    free(pl->free_slots);
    pl->free_slots = NULL;
    pl->cap_free = 0;
    kai_teardown_expect(pl->vmas_used == 0, "a fiber stack");
    pthread_mutex_unlock(&pl->mu);
#endif
}

/* The userdata table, once no handle is live: a live one is still C's to take.
 * Later slots start past every generation handed out, so an old handle never
 * names a new value. */
static void kai_ud_release(void) {
    pthread_mutex_lock(&kai_ud_mu);
    uintptr_t gen = kai_ud_first_gen;
    int live = 0;
    for (uintptr_t idx = 1; idx <= kai_ud_count; idx++) {
        KaiUdSlot *s = kai_ud_slot(idx);
        live |= s->v != NULL;
        if (s->gen > gen) gen = s->gen;
    }
    if (!live) {
        while (kai_ud_nchunks > 0) free(kai_ud_chunks[--kai_ud_nchunks]);
        free(kai_ud_chunks);
        kai_ud_chunks = NULL;
        kai_ud_count = kai_ud_free = 0;
        kai_ud_first_gen = gen;
    }
    pthread_mutex_unlock(&kai_ud_mu);
}

#endif

KAI_SCHED_FN void kai_runtime_teardown(void)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    kai_boot_root_release();
    kai_reactor_release();
    kai_thread_release(kai_worker_here());
    kai_stack_pool_release();
    kai_ud_release();
    kai_teardown_expect(!kai_boot_root, "main's fiber");
    kai_teardown_expect(!kai_reactor_ready_batch && !kai_reactor_timers && !kai_reactor_pending_exits,
                        "a reactor buffer");
#if KAI_HOST_STACK_GUARD
    kai_teardown_expect(!kai_stack_pool.free_slots, "the stack pool");
#endif
    kai_teardown_expect(!kai_ud_chunks, "the userdata table");
}
#endif
