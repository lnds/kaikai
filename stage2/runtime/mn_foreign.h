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
/* The fiber stays on its OS thread for the call: the C frames below it carry
 * thread-local state, locks and errno that cannot follow a migration.
 * A trap or a cancel inside the body must not unwind through C frames: the
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
        f->seg_pin++;
        KaiValue *r = c->thunk(NULL, argv, c->n);
        f->seg_pin--;
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
 * references. `take` yields a fresh copy and leaves the handle usable.
 * A handle is a slot index plus the slot's generation, never a pointer: a
 * released or forged handle fails the generation check and traps instead of
 * reading freed memory, also once the slot is reused. */
#if !KAI_SCHED_DECL_ONLY
#define KAI_UD_HALF   ((unsigned) (sizeof(uintptr_t) * 4))
#define KAI_UD_CHUNK  1024
#ifndef KAI_UD_IDX_MAX
#define KAI_UD_IDX_MAX (((uintptr_t) 1 << KAI_UD_HALF) - 1)
#endif

typedef struct { KaiValue *v; uintptr_t gen; uintptr_t next; } KaiUdSlot;

static pthread_mutex_t kai_ud_mu = PTHREAD_MUTEX_INITIALIZER;
static KaiUdSlot     **kai_ud_chunks;
static uintptr_t       kai_ud_nchunks;
static uintptr_t       kai_ud_count;
static uintptr_t       kai_ud_free;

static KaiUdSlot *kai_ud_slot(uintptr_t idx) {
    return &kai_ud_chunks[(idx - 1) / KAI_UD_CHUNK][(idx - 1) % KAI_UD_CHUNK];
}

/* The live slot a handle names, or NULL; caller holds `kai_ud_mu`. */
static KaiUdSlot *kai_ud_find(KaiValue *u) {
    uintptr_t h   = (uintptr_t) kai_foreign_ptr(u);
    uintptr_t idx = h & (((uintptr_t) 1 << KAI_UD_HALF) - 1);
    if (idx == 0 || idx > kai_ud_count) return NULL;
    KaiUdSlot *s = kai_ud_slot(idx);
    return (s->v && s->gen == (h >> KAI_UD_HALF)) ? s : NULL;
}

#endif

KAI_SCHED_FN KaiValue *kai_ffi_retain(KaiValue *v)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    KaiValue *copy = kai_deep_copy_cross(v);
    kai_decref(v);
    pthread_mutex_lock(&kai_ud_mu);
    uintptr_t idx = kai_ud_free;
    if (idx) {
        kai_ud_free = kai_ud_slot(idx)->next;
    } else {
        if (kai_ud_count >= KAI_UD_IDX_MAX) {
            pthread_mutex_unlock(&kai_ud_mu);
            kai_trap_abort("retain_userdata: too many live userdata handles");
        }
        if (kai_ud_count == kai_ud_nchunks * KAI_UD_CHUNK) {
            kai_ud_chunks = (KaiUdSlot **) realloc(kai_ud_chunks, (kai_ud_nchunks + 1) * sizeof *kai_ud_chunks);
            kai_ud_chunks[kai_ud_nchunks++] = (KaiUdSlot *) calloc(KAI_UD_CHUNK, sizeof(KaiUdSlot));
        }
        idx = ++kai_ud_count;
        kai_ud_slot(idx)->gen = 1;
    }
    KaiUdSlot *s = kai_ud_slot(idx);
    s->v = copy;
    uintptr_t h = (s->gen << KAI_UD_HALF) | idx;
    pthread_mutex_unlock(&kai_ud_mu);
    return kai_foreign((void *) h);
}
#endif

KAI_SCHED_FN KaiValue *kai_ffi_take(KaiValue *u)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    pthread_mutex_lock(&kai_ud_mu);
    KaiUdSlot *s = kai_ud_find(u);
    KaiValue *copy = s ? kai_deep_copy_cross(s->v) : NULL;
    pthread_mutex_unlock(&kai_ud_mu);
    kai_decref(u);
    if (!copy) kai_trap_abort("take_userdata: handle is released or invalid");
    return copy;
}
#endif

KAI_SCHED_FN KaiValue *kai_ffi_release(KaiValue *u)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    pthread_mutex_lock(&kai_ud_mu);
    KaiUdSlot *s = kai_ud_find(u);
    KaiValue *old = NULL;
    if (s) {
        old = s->v;
        s->v = NULL;
        s->gen = ((s->gen + 1) & (((uintptr_t) 1 << KAI_UD_HALF) - 1)) ?: 1;
        s->next = kai_ud_free;
        kai_ud_free = (uintptr_t) kai_foreign_ptr(u) & (((uintptr_t) 1 << KAI_UD_HALF) - 1);
    }
    pthread_mutex_unlock(&kai_ud_mu);
    kai_decref(u);
    if (!old) kai_trap_abort("release_userdata: handle is already released or invalid");
    kai_decref(old);
    return kai_unit();
}
#endif
