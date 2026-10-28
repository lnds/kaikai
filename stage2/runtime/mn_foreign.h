/* Fragment of runtime.h; include runtime.h, never this file. */
/* ==================================================================
 * C-ABI exports — the entry every `extern "C" fn` with a body goes through.
 * A call arrives on a kaikai fiber (C called back synchronously) or on a
 * thread the runtime does not own. The generated adapter boxes its C
 * arguments and hands `kai_export_call` the fn's thunk.
 * ================================================================== */

typedef struct {
    const char     *name;
    KaiFn           thunk;
    KaiValue      **args;
    int             n;
    KaiValue       *result;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int             done;
} KaiExportCall;

#if !KAI_SCHED_DECL_ONLY
/* A trap or a cancel inside the body must not unwind through C frames: the
 * fiber's pad is pointed at this frame for the call, and landing here ends
 * the process. `cross` copies the arguments and the result across the thread
 * border: the caller's cells belong to a thread the scheduler does not own. */
static void kai_export_guarded(KaiExportCall *c, int cross) {
    KaiFiber *f = kai_current_fiber();
    jmp_buf saved_pad;
    memcpy(&saved_pad, &f->cancel_pad, sizeof saved_pad);
    int saved_set = f->cancel_pad_set;
    uintptr_t saved_frame = f->unwind_frame;
    f->unwind_frame = (uintptr_t) __builtin_frame_address(0);
    if (setjmp(f->cancel_pad) == 0) {
        f->cancel_pad_set = 1;
        KaiValue *argv[c->n > 0 ? c->n : 1];
        for (int i = 0; i < c->n; i++) argv[i] = cross ? kai_deep_copy_cross(c->args[i]) : c->args[i];
        KaiValue *r = c->thunk(NULL, argv, c->n);
        if (cross) { c->result = kai_deep_copy_cross(r); kai_decref(r); } else { c->result = r; }
        f->cancel_pad_set = saved_set;
        memcpy(&f->cancel_pad, &saved_pad, sizeof saved_pad);
        f->unwind_frame = saved_frame;
    } else {
        kai_eprintf(f->trapped ? "kai: panic in exported function %s\n"
                               : "kai: cancelled inside exported function %s\n", c->name);
        abort();
    }
}

/* The fiber that runs an export on behalf of a foreign thread. */
static KaiValue *kai_export_thunk(KaiValue *self, KaiValue **args, int n_args) {
    (void) args; (void) n_args;
    KaiExportCall *c = (KaiExportCall *) (intptr_t) kai_intf(self->as.clo.captures[0]);
    kai_export_guarded(c, 1);
    pthread_mutex_lock(&c->mu);
    c->done = 1;
    pthread_cond_signal(&c->cv);
    pthread_mutex_unlock(&c->mu);
    return kai_unit();
}
#endif

/* The entry of every exported fn: runs `thunk` on `args` (consumed) and
 * returns its owned result. */
KAI_SCHED_FN KaiValue *kai_export_call(const char *name, KaiFn thunk, KaiValue **args, int n)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    KaiExportCall c = { .name = name, .thunk = thunk, .args = args, .n = n, .result = NULL, .done = 0 };
    if (kai_worker_here()->sched) {
        kai_export_guarded(&c, 0);
#ifdef KAI_CELL_POOL_ACTIVE
        kai_cell_pool_tick();
#endif
        return c.result;
    }
    if (kai_nthreads <= 1 || kai_sched_shutting_down) {
        kai_eprintf("kai: exported function %s called from a foreign thread %s\n", name,
                    kai_nthreads <= 1 ? "while KAI_THREADS=1: set KAI_THREADS>1 (or unset it)"
                                      : "after the program ended");
        abort();
    }
    pthread_mutex_init(&c.mu, NULL);
    pthread_cond_init(&c.cv, NULL);
    KaiValue *cell  = kai_int((int64_t) (intptr_t) &c);
    KaiValue *body  = kai_closure(kai_export_thunk, 0, 1, &cell);
    KaiValue *fiber = kai_spawn_fiber_stamped(body, NULL);
    pthread_mutex_lock(&c.mu);
    while (!c.done) pthread_cond_wait(&c.cv, &c.mu);
    pthread_mutex_unlock(&c.mu);
    for (int i = 0; i < n; i++) kai_decref(args[i]);
    kai_decref(fiber);
    kai_decref(body);
    kai_decref(cell);
    pthread_cond_destroy(&c.cv);
    pthread_mutex_destroy(&c.mu);
#ifdef KAI_CELL_POOL_ACTIVE
    kai_cell_pool_tick();
#endif
    return c.result;
}
#endif

/* A kaikai value handed to C as `void *`: the owner of the handle holds a
 * private copy, so any thread may read it and it outlives the caller's
 * references. `take` yields a fresh copy and leaves the handle usable. */
static KaiValue *kai_ffi_retain(KaiValue *v) {
    KaiValue *copy = kai_deep_copy_cross(v);
    kai_decref(v);
    return kai_foreign(copy);
}

static KaiValue *kai_ffi_take(KaiValue *u) {
    KaiValue *copy = kai_deep_copy_cross((KaiValue *) kai_foreign_ptr(u));
    kai_decref(u);
    return copy;
}

static KaiValue *kai_ffi_release(KaiValue *u) {
    kai_decref((KaiValue *) kai_foreign_ptr(u));
    kai_decref(u);
    return kai_unit();
}
