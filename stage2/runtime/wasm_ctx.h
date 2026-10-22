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

/* Parks the running context until `deadline_ms` on the monotonic clock (forever
 * when negative) or until the host posts a wake. Returns the milliseconds by
 * which the host may have woken early, which the caller counts as elapsed. */
KAI_WCTX_IMPORT("wait_until") extern double kai_wctx_host_wait(double deadline_ms);

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

typedef struct { void (*entry)(void *); void *sp; uintptr_t limit; int id; } KaiWSegRec;

#define KAI_WCTX_ROOT 1
#define KAI_WCTX_NOINSTR __attribute__((no_instrument_function))

/* `kai_wctx_limit` is the lowest stack pointer the running context may reach
 * before the entry hook reports an overflow; zero for the main stack, whose
 * overflow runs off the bottom of memory and traps. */
#if defined(KAI_SEPARATE_COMPILATION)
extern KAI_TLS int kai_wctx_cur;
extern KAI_TLS int kai_wctx_next_id;
extern KAI_TLS void (*kai_wctx_boot_fn0)(void);
extern KAI_TLS void (*kai_wctx_boot_fn1)(void *);
extern KAI_TLS int kai_wctx_boot_link;
extern KAI_TLS uintptr_t kai_wctx_boot_limit;
extern KAI_TLS uintptr_t kai_wctx_limit;
extern KAI_TLS void *kai_wctx_xfer;
#  if defined(KAI_RUNTIME_OWNER)
KAI_TLS int kai_wctx_cur = KAI_WCTX_ROOT;
KAI_TLS int kai_wctx_next_id = KAI_WCTX_ROOT;
KAI_TLS void (*kai_wctx_boot_fn0)(void);
KAI_TLS void (*kai_wctx_boot_fn1)(void *);
KAI_TLS int kai_wctx_boot_link;
KAI_TLS uintptr_t kai_wctx_boot_limit;
KAI_TLS uintptr_t kai_wctx_limit;
KAI_TLS void *kai_wctx_xfer;
#  endif
#else
static KAI_TLS int kai_wctx_cur = KAI_WCTX_ROOT;
static KAI_TLS int kai_wctx_next_id = KAI_WCTX_ROOT;
static KAI_TLS void (*kai_wctx_boot_fn0)(void);
static KAI_TLS void (*kai_wctx_boot_fn1)(void *);
static KAI_TLS int kai_wctx_boot_link;
static KAI_TLS uintptr_t kai_wctx_boot_limit;
static KAI_TLS uintptr_t kai_wctx_limit;
static KAI_TLS void *kai_wctx_xfer;
#endif

KAI_WCTX_NOINSTR static inline void *kai_wctx_sp_get(void) {
    void *p;
    __asm__ volatile("global.get __stack_pointer\n\tlocal.set %0" : "=r"(p));
    return p;
}

KAI_WCTX_NOINSTR static inline void kai_wctx_sp_set(void *p) {
    __asm__ volatile("local.get %0\n\tglobal.set __stack_pointer" :: "r"(p));
}

/* The lowest address a stack of `size` bytes at `lo` may be used down to: the
 * room left below it is what the overflow report itself runs in. */
KAI_WCTX_NOINSTR static uintptr_t kai_wctx_floor(uintptr_t lo, size_t size) {
    size_t room = size / 4;
    return lo + (room < 8192 ? room : 8192);
}

__attribute__((noreturn, noinline, cold)) KAI_WCTX_NOINSTR
static void kai_wctx_overflow(void) {
    static const char msg[] = "kai: fiber stack overflow\n";
    write(2, msg, sizeof msg - 1);
    _Exit(139);
}

/* Emitted at the entry of every kaikai function; the runtime's own recursion is bounded and unchecked. */
#ifndef KAI_STACK_CHECK
#define KAI_STACK_CHECK() \
    do { if (__builtin_expect((uintptr_t) kai_wctx_sp_get() < kai_wctx_limit, 0)) kai_wctx_overflow(); } while (0)
#endif

/* The stack pointer a fresh target starts on, NULL for one already running. */
KAI_WCTX_NOINSTR static void *kai_wctx_arm(const ucontext_t *to) {
    if (to->fn == NULL) return NULL;
    kai_wctx_boot_fn0 = to->fn;
    kai_wctx_boot_fn1 = NULL;
    kai_wctx_boot_link = to->uc_link != NULL ? to->uc_link->id : 0;
    kai_wctx_boot_limit = kai_wctx_floor((uintptr_t) to->uc_stack.ss_sp, to->uc_stack.ss_size);
    ((ucontext_t *) to)->fn = NULL;
    return to->sp;
}

/* The host starts a fresh context by calling this through the function table. */
KAI_WCTX_NOINSTR __attribute__((noinline))
static void kai_ctx_enter(int id) {
    (void) id;
    void (*f0)(void) = kai_wctx_boot_fn0;
    void (*f1)(void *) = kai_wctx_boot_fn1;
    void *arg = kai_wctx_xfer;
    int link = kai_wctx_boot_link;
    kai_wctx_limit = kai_wctx_boot_limit;
    if (f0 != NULL) f0(); else f1(arg);
    if (link == 0) abort();
    kai_wctx_cur = link;
    kai_wctx_host_exit(link, NULL, NULL);
}

/* The entry the host needs when `sp` names a fresh stack. */
#define KAI_WCTX_ENTER(sp) ((sp) != NULL ? kai_ctx_enter : NULL)

KAI_WCTX_NOINSTR static int getcontext(ucontext_t *c) {
    c->fn = NULL;
    c->id = 0;
    return 0;
}

KAI_WCTX_NOINSTR static void makecontext(ucontext_t *c, void (*fn)(void), int argc, ...) {
    (void) argc;
    uintptr_t top = ((uintptr_t) c->uc_stack.ss_sp + c->uc_stack.ss_size) & ~(uintptr_t) 15;
    c->fn = fn;
    c->sp = (void *) top;
    c->id = ++kai_wctx_next_id;
}

KAI_WCTX_NOINSTR static int swapcontext(ucontext_t *save, const ucontext_t *to) {
    int from = kai_wctx_cur;
    save->id = from;
    save->fn = NULL;
    void *mysp = kai_wctx_sp_get();
    uintptr_t mylimit = kai_wctx_limit;
    void *sp = kai_wctx_arm(to);
    kai_wctx_cur = to->id;
    kai_wctx_host_switch(from, to->id, sp, KAI_WCTX_ENTER(sp));
    kai_wctx_sp_set(mysp);
    kai_wctx_limit = mylimit;
    return 0;
}

KAI_WCTX_NOINSTR __attribute__((noreturn))
static int setcontext(const ucontext_t *to) {
    void *sp = kai_wctx_arm(to);
    kai_wctx_cur = to->id;
    kai_wctx_host_exit(to->id, sp, KAI_WCTX_ENTER(sp));
}

/* ---------- Segment switch ---------- */

#define KAI_SEG_SWITCH 1

static size_t kai_fiber_stack_size(void);

KAI_WCTX_NOINSTR static void *kai_seg_frame_init(void *top, void (*entry)(void *)) {
    uintptr_t t = ((uintptr_t) top & ~(uintptr_t) 15) - 16 * ((sizeof(KaiWSegRec) + 15) / 16);
    size_t size = kai_fiber_stack_size();
    KaiWSegRec *rec = (KaiWSegRec *) t;
    rec->entry = entry;
    rec->sp = (void *) t;
    rec->limit = kai_wctx_floor((uintptr_t) top - size, size);
    rec->id = ++kai_wctx_next_id;
    return rec;
}

KAI_WCTX_NOINSTR static int kai_wctx_seg_target(void *to, void *arg, void **sp) {
    kai_wctx_xfer = arg;
    if ((uintptr_t) to & 1) {
        *sp = NULL;
        return (int) ((uintptr_t) to >> 1);
    }
    KaiWSegRec *rec = (KaiWSegRec *) to;
    kai_wctx_boot_fn0 = NULL;
    kai_wctx_boot_fn1 = rec->entry;
    kai_wctx_boot_link = 0;
    kai_wctx_boot_limit = rec->limit;
    *sp = rec->sp;
    return rec->id;
}

KAI_WCTX_NOINSTR static void *kai_seg_switch(void **save, void *to, void *arg) {
    int from = kai_wctx_cur;
    *save = (void *) (((uintptr_t) from << 1) | 1);
    void *mysp = kai_wctx_sp_get();
    uintptr_t mylimit = kai_wctx_limit;
    void *sp;
    int id = kai_wctx_seg_target(to, arg, &sp);
    kai_wctx_cur = id;
    kai_wctx_host_switch(from, id, sp, KAI_WCTX_ENTER(sp));
    kai_wctx_sp_set(mysp);
    kai_wctx_limit = mylimit;
    return kai_wctx_xfer;
}

/* Leave the running stack for good: the host drops it once it is off. */
KAI_WCTX_NOINSTR __attribute__((noreturn))
static void kai_seg_switch_final(void *to) {
    void *sp;
    int id = kai_wctx_seg_target(to, NULL, &sp);
    kai_wctx_cur = id;
    kai_wctx_host_exit(id, sp, KAI_WCTX_ENTER(sp));
}
