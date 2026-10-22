/* Fragment of runtime.h; include runtime.h, never this file. */
/* =================================================================
 * Contexts on WebAssembly
 * =================================================================
 *
 * A context is one WebAssembly stack the host keeps suspended. WebAssembly
 * cannot install a stack it holds the address of, so a switch is two host
 * calls: the running context suspends through `kai.ctx_switch`, a
 * `WebAssembly.Suspending` import whose promise settles when something
 * switches back, and the host starts or wakes the target. Exactly one
 * context runs at a time, and the host does the dispatch.
 *
 * Each context also owns a slice of linear memory for the C stack, which
 * `__stack_pointer` addresses; a switch saves and restores it around the
 * call. The host sets it when it starts a fresh context.
 *
 * Contexts are identified by id. A `ucontext_t` carries its id, so a
 * `swapcontext` save slot names the stack that was running. A segment's
 * switch token is the id, tagged in bit 0, or the record of a fresh stack. */

#define KAI_WCTX_IMPORT(n) __attribute__((import_module("kai"), import_name(n)))

KAI_WCTX_IMPORT("ctx_switch") extern void kai_wctx_host_switch(int from, int to, void *sp, void (*enter)(int));
KAI_WCTX_IMPORT("ctx_exit") extern void kai_wctx_host_exit(int to, void *sp, void (*enter)(int)) __attribute__((noreturn));

/* Called before the first context is created; a module that imports it needs
 * suspension, one that does not never switches. */
KAI_WCTX_IMPORT("needs_suspension") extern void kai_wctx_host_mark(void);

static inline void kai_wctx_mark(void) {
    static int marked;
    if (!marked) {
        marked = 1;
        kai_wctx_host_mark();
    }
}

typedef struct kai_wctx {
    struct kai_wctx *uc_link;
    struct { void *ss_sp; size_t ss_size; } uc_stack;
    void (*fn)(void);
    void *sp;
    int id;
} ucontext_t;

typedef struct { void (*entry)(void *); void *sp; int id; } KaiWSegRec;

#define KAI_WCTX_ROOT 1

#if defined(KAI_SEPARATE_COMPILATION)
extern int kai_wctx_cur;
extern int kai_wctx_next_id;
extern void (*kai_wctx_boot_fn0)(void);
extern void (*kai_wctx_boot_fn1)(void *);
extern int kai_wctx_boot_link;
extern void *kai_wctx_xfer;
#  if defined(KAI_RUNTIME_OWNER)
int kai_wctx_cur = KAI_WCTX_ROOT;
int kai_wctx_next_id = KAI_WCTX_ROOT;
void (*kai_wctx_boot_fn0)(void);
void (*kai_wctx_boot_fn1)(void *);
int kai_wctx_boot_link;
void *kai_wctx_xfer;
#  endif
#else
static int kai_wctx_cur = KAI_WCTX_ROOT;
static int kai_wctx_next_id = KAI_WCTX_ROOT;
static void (*kai_wctx_boot_fn0)(void);
static void (*kai_wctx_boot_fn1)(void *);
static int kai_wctx_boot_link;
static void *kai_wctx_xfer;
#endif

static inline void *kai_wctx_sp_get(void) {
    void *p;
    __asm__ volatile("global.get __stack_pointer\n\tlocal.set %0" : "=r"(p));
    return p;
}

static inline void kai_wctx_sp_set(void *p) {
    __asm__ volatile("local.get %0\n\tglobal.set __stack_pointer" :: "r"(p));
}

/* The stack pointer a fresh target starts on, NULL for one already running. */
static void *kai_wctx_arm(const ucontext_t *to) {
    if (to->fn == NULL) return NULL;
    kai_wctx_boot_fn0 = to->fn;
    kai_wctx_boot_fn1 = NULL;
    kai_wctx_boot_link = to->uc_link != NULL ? to->uc_link->id : 0;
    ((ucontext_t *) to)->fn = NULL;
    return to->sp;
}

/* The host starts a fresh context by calling this through the function table. */
__attribute__((noinline))
static void kai_ctx_enter(int id) {
    (void) id;
    void (*f0)(void) = kai_wctx_boot_fn0;
    void (*f1)(void *) = kai_wctx_boot_fn1;
    void *arg = kai_wctx_xfer;
    int link = kai_wctx_boot_link;
    if (f0 != NULL) f0(); else f1(arg);
    if (link == 0) abort();
    kai_wctx_cur = link;
    kai_wctx_host_exit(link, NULL, NULL);
}

/* The entry the host needs when `sp` names a fresh stack. */
#define KAI_WCTX_ENTER(sp) ((sp) != NULL ? kai_ctx_enter : NULL)

static int getcontext(ucontext_t *c) {
    c->fn = NULL;
    c->id = 0;
    return 0;
}

static void makecontext(ucontext_t *c, void (*fn)(void), int argc, ...) {
    (void) argc;
    uintptr_t top = ((uintptr_t) c->uc_stack.ss_sp + c->uc_stack.ss_size) & ~(uintptr_t) 15;
    c->fn = fn;
    c->sp = (void *) top;
    c->id = ++kai_wctx_next_id;
}

static int swapcontext(ucontext_t *save, const ucontext_t *to) {
    int from = kai_wctx_cur;
    save->id = from;
    save->fn = NULL;
    void *mysp = kai_wctx_sp_get();
    void *sp = kai_wctx_arm(to);
    kai_wctx_cur = to->id;
    kai_wctx_host_switch(from, to->id, sp, KAI_WCTX_ENTER(sp));
    kai_wctx_sp_set(mysp);
    return 0;
}

__attribute__((noreturn))
static int setcontext(const ucontext_t *to) {
    void *sp = kai_wctx_arm(to);
    kai_wctx_cur = to->id;
    kai_wctx_host_exit(to->id, sp, KAI_WCTX_ENTER(sp));
}

/* ---------- Segment switch ---------- */

#define KAI_SEG_SWITCH 1

static void *kai_seg_frame_init(void *top, void (*entry)(void *)) {
    uintptr_t t = ((uintptr_t) top & ~(uintptr_t) 15) - 16 * ((sizeof(KaiWSegRec) + 15) / 16);
    KaiWSegRec *rec = (KaiWSegRec *) t;
    rec->entry = entry;
    rec->sp = (void *) t;
    rec->id = ++kai_wctx_next_id;
    return rec;
}

static int kai_wctx_seg_target(void *to, void *arg, void **sp) {
    kai_wctx_xfer = arg;
    if ((uintptr_t) to & 1) {
        *sp = NULL;
        return (int) ((uintptr_t) to >> 1);
    }
    KaiWSegRec *rec = (KaiWSegRec *) to;
    kai_wctx_boot_fn0 = NULL;
    kai_wctx_boot_fn1 = rec->entry;
    kai_wctx_boot_link = 0;
    *sp = rec->sp;
    return rec->id;
}

static void *kai_seg_switch(void **save, void *to, void *arg) {
    int from = kai_wctx_cur;
    *save = (void *) (((uintptr_t) from << 1) | 1);
    void *mysp = kai_wctx_sp_get();
    void *sp;
    int id = kai_wctx_seg_target(to, arg, &sp);
    kai_wctx_cur = id;
    kai_wctx_host_switch(from, id, sp, KAI_WCTX_ENTER(sp));
    kai_wctx_sp_set(mysp);
    return kai_wctx_xfer;
}

/* Leave the running stack for good: the host drops it once it is off. */
__attribute__((noreturn))
static void kai_seg_switch_final(void *to) {
    void *sp;
    int id = kai_wctx_seg_target(to, NULL, &sp);
    kai_wctx_cur = id;
    kai_wctx_host_exit(id, sp, KAI_WCTX_ENTER(sp));
}
