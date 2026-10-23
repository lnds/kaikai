/* Fragment of runtime.h; include runtime.h, never this file. */
/* =================================================================
 * Log effect — issue #141. Tier S2 #7 of `docs/stdlib-roadmap.md`.
 *
 * Four leveled ops (debug / info / warn / error) routed through the
 * Log effect. The default handler installed by `kai_main_install_
 * defaults` writes to stderr in
 *
 *     [YYYY-MM-DDTHH:MM:SSZ] LEVEL message\n
 *
 * form. The level field is left-padded to 5 chars so the column
 * after it is aligned across all four ops:
 *     "DEBUG", "INFO ", "WARN ", "ERROR".
 *
 * Timestamp source: `clock_gettime(CLOCK_REALTIME)` + `gmtime_r`
 * + `strftime`. Calling `clock_gettime` directly here (rather than
 * routing through the kaikai-side `Clock` effect) keeps the `Log`
 * row free of `Clock` — a program can declare `: Unit / Log` and
 * have its main install the Log default without also pulling
 * `Clock`'s default handler into the row.
 *
 * The runtime flushes stderr after each write so test harnesses
 * see the output deterministically (no buffer hold-back if a
 * later abort kills the process before exit-time flushing fires).
 *
 * v1 limitations (mirrored in stdlib/log.kai):
 *   - No level filtering. All four levels write unconditionally.
 *   - No structured fields, redaction, rotation, color, async
 *     batching, or trace-context propagation. All of those are
 *     ahu.log territory (a higher layer that wraps Log). */

static void _kai_log_emit(const char *level_padded, KaiValue *msg) {
    /* Format the timestamp into a stack buffer. clock_gettime /
     * gmtime_r failure is rare; fall back to a sentinel rather
     * than swallow the message — the user should still see what
     * level + body was intended. */
    char ts[32];
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
        struct tm tm;
        if (gmtime_r(&now.tv_sec, &tm) != NULL &&
            strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm) > 0) {
            /* ok */
        } else {
            memcpy(ts, "?????", 6);
        }
    } else {
        memcpy(ts, "?????", 6);
    }

    /* Header `[<ts>] <LEVEL> ` then the message bytes then newline.
     * fputs on the prefix, fwrite on the message body so embedded
     * NULs in user strings don't truncate the line. */
    kai_flush_stdout();
    flockfile(stderr);
    fputc('[', stderr);
    kai_eputs(ts);
    kai_eputs("] ");
    kai_eputs(level_padded);
    fputc(' ', stderr);
    if (msg && msg->tag == KAI_STR && msg->as.s.bytes && msg->as.s.len > 0) {
        fwrite(msg->as.s.bytes, 1, msg->as.s.len, stderr);
    }
    fputc('\n', stderr);
    funlockfile(stderr);
    fflush(stderr);
}

static KaiValue *kai_default_log_debug(void *self, KaiValue *msg, KaiCont *k) {
    (void) self;
    _kai_log_emit("DEBUG", msg);
    return kai_cont_resume(k, kai_unit());
}

static KaiValue *kai_default_log_info(void *self, KaiValue *msg, KaiCont *k) {
    (void) self;
    _kai_log_emit("INFO ", msg);
    return kai_cont_resume(k, kai_unit());
}

static KaiValue *kai_default_log_warn(void *self, KaiValue *msg, KaiCont *k) {
    (void) self;
    _kai_log_emit("WARN ", msg);
    return kai_cont_resume(k, kai_unit());
}

static KaiValue *kai_default_log_error(void *self, KaiValue *msg, KaiCont *k) {
    (void) self;
    _kai_log_emit("ERROR", msg);
    return kai_cont_resume(k, kai_unit());
}

/* =================================================================
 * SecureRandom effect — issue #140. Cryptographically-secure RNG
 * deliberately separated from Random so test handlers stubbing the
 * latter cannot weaken security-sensitive paths.
 *
 * Default handler uses the platform CSPRNG, never a userspace PRNG:
 *   - Linux: getrandom(2) syscall (works pre-prng-init; we loop on
 *     EINTR but otherwise treat any failure as fatal — the kernel
 *     pool being unavailable is catastrophic and there is no
 *     meaningful Result for callers who already opted into crypto-
 *     grade randomness).
 *   - macOS / *BSD: arc4random_buf(3) — returns void; the libc
 *     implementation reseeds itself from /dev/urandom and panics
 *     internally on failure, matching the same "infallible from
 *     callers' perspective" contract.
 *
 * v1 limitations (mirrored in docs/effects-stdlib.md §SecureRandom):
 *   - POSIX only. Windows BCryptGenRandom is post-MVP.
 *   - No /dev/urandom fallback. getrandom is the entire Linux story.
 *   - No userspace ChaCha20 CSPRNG layer. Each draw hits the kernel.
 *   - Uniform `int(min, max)` uses `% delta` reduction. The modulo
 *     bias is acceptable for v1 (security-grade rejection sampling
 *     is a follow-up); cryptographic-protocol callers that need a
 *     strictly-uniform integer should draw `bytes` and reduce
 *     themselves.
 */

#if defined(__linux__)
#  include <sys/random.h>
#elif defined(__APPLE__) || defined(__MACH__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
   /* arc4random_buf is in <stdlib.h> on macOS/BSD, already included
    * above. No extra header needed. */
#elif defined(__wasi__)
#  include <wasi/api.h>
#else
#  error "kai SecureRandom: platform unsupported (POSIX getrandom / arc4random_buf required)"
#endif

static void _kai_securerandom_fill(unsigned char *buf, size_t n) {
#if defined(__linux__)
    size_t off = 0;
    while (off < n) {
        ssize_t got = getrandom(buf + off, n - off, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            kai_eprintf("kai: SecureRandom: getrandom failed: %s\n", strerror(errno));
            kai_exit(1);
        }
        off += (size_t) got;
    }
#elif defined(__wasi__)
    if (__wasi_random_get(buf, n) != 0) {
        kai_eprintf("kai: SecureRandom: random_get failed\n");
        kai_exit(1);
    }
#else
    arc4random_buf(buf, n);
#endif
}

/* int_range(min, max) -> Int in [min, max] (inclusive). Pulls 8
 * bytes from the platform CSPRNG, interprets them as a uint64,
 * reduces modulo (max - min + 1), and adds min. min > max is a
 * panic: there is no meaningful uniform draw over an empty range.
 * Op named `int_range` (not the doc's `int()`) because `int` is a
 * C keyword and the C backend emits each effect op as a struct
 * field by name. */
static KaiValue *kai_default_securerandom_int_range(void *self, KaiValue *min_v, KaiValue *max_v, KaiCont *k) {
    (void) self;
    int64_t lo = (kai_is_int(min_v)) ? kai_intf(min_v) : 0;
    int64_t hi = (kai_is_int(max_v)) ? kai_intf(max_v) : 0;
    if (lo > hi) {
        kai_eprintf("kai: SecureRandom.int: min (%lld) > max (%lld)\n",
                (long long) lo, (long long) hi);
        kai_exit(1);
    }
    unsigned char buf[8];
    _kai_securerandom_fill(buf, sizeof(buf));
    uint64_t draw = 0;
    for (int i = 0; i < 8; ++i) draw = (draw << 8) | (uint64_t) buf[i];
    uint64_t span = (uint64_t) (hi - lo) + 1ULL;
    uint64_t pick = (span == 0) ? draw : (draw % span);
    return kai_cont_resume(k, kai_int(lo + (int64_t) pick));
}

/* bytes(n) -> [Int] (each in [0, 256), surface-typed [Byte] but
 * stage 2 has no first-class Byte yet — same divergence the NetTcp
 * decl documents). n <= 0 returns the empty list; n > 1 MiB is
 * capped (matches NetTcp.recv's v1 ceiling — multi-megabyte single
 * draws are out of scope and almost always wrong for crypto use,
 * which is happier with smaller, repeated draws). */
static KaiValue *kai_default_securerandom_bytes(void *self, KaiValue *n_v, KaiCont *k) {
    (void) self;
    int64_t n = (kai_is_int(n_v)) ? kai_intf(n_v) : 0;
    if (n <= 0) {
        return kai_cont_resume(k, kai_nil());
    }
    if (n > (1 << 20)) n = 1 << 20;
    unsigned char *buf = (unsigned char *) malloc((size_t) n);
    if (!buf) {
        kai_eputs("kai: SecureRandom.bytes: out of memory\n");
        kai_exit(1);
    }
    _kai_securerandom_fill(buf, (size_t) n);
    KaiValue *acc = kai_nil();
    for (int64_t i = n; i > 0;) { --i; acc = kai_cons(kai_int((int64_t) buf[i]), acc); }
    free(buf);
    return kai_cont_resume(k, acc);
}

/* =================================================================
 * m8.x cooperative scheduler primitives
 * =================================================================
 *
 * Spec: docs/fibers-impl.md §*Scheduler* and §*Yield primitives*.
 *
 * Single-threaded cooperative scheduler. The OS thread starts in
 * its worker's root fiber (state=RUNNING); spawned fibers each get a private
 * heap-allocated stack and a ucontext_t. Yield/park use swapcontext
 * to hand control between fibers. The dispatcher is implicit — there
 * is no separate scheduler context, just whoever was running before
 * the current fiber gets resumed when the queue empties.
 *
 * v1 ships Phase 2 (scheduler core); Phase 3 adds Cancel delivery at
 * yield points; Phase 4 adds blocking primitives (BlockSender mailbox
 * + Actor.receive parking); Phase 5 adds Link/Monitor runtime.
 */

/* Page size, queried once via sysconf and cached. macOS arm64 pages
 * are 16 KiB, x86_64 / Linux are 4 KiB; the guard arithmetic must use
 * the runtime value or mprotect rejects the call. Workers race to fill the
 * cache with one value, so a relaxed atomic is enough. */
static size_t kai_page_size(void) {
#if defined(__wasm__)
    return 16;
#endif
    static _Atomic size_t cached = 0;
    size_t ps = atomic_load_explicit(&cached, memory_order_relaxed);
    if (ps == 0) {
        long q = sysconf(_SC_PAGESIZE);
        ps = (q > 0) ? (size_t) q : 4096;
        atomic_store_explicit(&cached, ps, memory_order_relaxed);
    }
    return ps;
}

/* Read KAI_FIBER_STACK_SIZE once and cache. Out-of-range values
 * fall back to the default and log a warning. The result is rounded
 * up to a page-size multiple — mmap + mprotect both require it, and
 * 16 KiB pages on macOS arm64 silently break sub-page values
 * otherwise. */
static size_t kai_fiber_stack_size(void) {
    static _Atomic size_t cached = 0;
    size_t hit = atomic_load_explicit(&cached, memory_order_relaxed);
    if (hit != 0) return hit;
    const char *env = getenv("KAI_FIBER_STACK_SIZE");
    size_t sz = 64 * 1024;  /* default 64 KiB */
    if (env && *env) {
        char *end = NULL;
        long long parsed = strtoll(env, &end, 10);
        if (end != env && *end == '\0' && parsed >= 4096 && parsed <= (long long)(64 * 1024 * 1024)) {
            sz = (size_t) parsed;
        } else {
            kai_eprintf(
                "kai: KAI_FIBER_STACK_SIZE=%s out of range [4096, 64M]; using default 64K\n",
                env);
        }
    }
    size_t ps = kai_page_size();
    if (sz % ps != 0) sz = ((sz / ps) + 1) * ps;
    atomic_store_explicit(&cached, sz, memory_order_relaxed);
    return sz;
}

/* Stack budget for the fiber that runs kai_main. It is not an ordinary
 * fiber: at one thread main runs directly on the OS thread stack, and at
 * N>1 it runs as a fiber — so handing it the 64 KiB fiber default would
 * make the thread count decide whether a deeply recursive program
 * survives. Track the main thread instead (RLIMIT_STACK, floored at the
 * conventional 8 MiB), and let a larger explicit KAI_FIBER_STACK_SIZE
 * still win. A build defining KAI_MAIN_STACK_SIZE fixes the budget
 * outright, at every thread count, whatever the environment says. */
#if KAI_HOST_STACK_GUARD
static int kai_stack_map_flags(void) {
    int flags = MAP_PRIVATE | MAP_ANON;
#if defined(MAP_NORESERVE)
    flags |= MAP_NORESERVE;
#endif
    return flags;
}
#endif

/* ---------- Stack-segment switch ----------
 *
 * `kai_seg_switch(save, to, arg)` pushes the callee-saved registers on the
 * current stack, stores the stack pointer in `*save`, loads `to`, pops the
 * registers saved there and returns `arg` on that stack. Not swapcontext: no
 * signal-mask syscall, and to the optimiser an ordinary opaque call, so no
 * returns_twice.
 *
 * Trap: no frame may keep a thread-local address across the call. The other
 * side can park the fiber, which may then resume on another OS thread. Only
 * the owner calls it; the owner gate checks its frames and an nm gate keeps
 * the symbol out of the hot bitcode.
 *
 * The way back is an indirect jump, not a return: a return to the other
 * stack always misses the return-address predictor. On aarch64 the restores
 * also read through the incoming pointer rather than through a freshly moved
 * sp. Together they cut a round trip on an M4 from 21.8 ns to 4.9 ns.
 *
 * GCC gets `noipa`: its IPA register allocation would otherwise read the asm
 * body as clobbering nothing and keep caller-saved values across the call.
 *
 * GCC ignores `naked` on aarch64 and would wrap the body in a frame of its
 * own, so there both functions are assembled at file scope and declared to
 * C: a body the compiler never sees is an opaque call without `noipa`.
 * Every unit assembles a copy, weak so the copies link as one: GCC at -O0
 * emits a static function nothing calls, and with it the reference. Hidden,
 * so position-independent code takes the stub's address pc-relative; through
 * the GOT, a symbol local to the unit loses its offset in the section. */
#if KAI_HOST_STACK_GUARD && (defined(__x86_64__) || (defined(__aarch64__) && (defined(__clang__) || defined(__ELF__))))
#  define KAI_SEG_SWITCH 1
#  if defined(__clang__)
#    define KAI_SEG_NAKED __attribute__((naked, noinline, unused))
#  else
#    define KAI_SEG_NAKED __attribute__((naked, noinline, noipa, unused))
#  endif

/* Under BTI an indirect branch may land only on a landing pad. */
#  if defined(__ARM_FEATURE_BTI_DEFAULT)
#    define KAI_SEG_A64_RETURN "ret\n\t"
#  else
#    define KAI_SEG_A64_RETURN "br x30\n\t"
#  endif

#  if defined(__x86_64__)
KAI_SEG_NAKED
static void *kai_seg_switch(void **save, void *to, void *arg) {
    __asm__ volatile(
        "pushq %rbp\n\t"
        "pushq %rbx\n\t"
        "pushq %r12\n\t"
        "pushq %r13\n\t"
        "pushq %r14\n\t"
        "pushq %r15\n\t"
        "movq %rsp, (%rdi)\n\t"
        "movq %rsi, %rsp\n\t"
        "popq %r15\n\t"
        "popq %r14\n\t"
        "popq %r13\n\t"
        "popq %r12\n\t"
        "popq %rbx\n\t"
        "popq %rbp\n\t"
        "movq %rdx, %rax\n\t"
        "popq %rcx\n\t"
        "jmpq *%rcx\n\t");
}

/* First return target of a fresh frame: `entry` waits in r12, `arg` arrives
 * as the switch's return value. */
KAI_SEG_NAKED
static void kai_seg_boot(void) {
    __asm__ volatile(
        "movq %rax, %rdi\n\t"
        "callq *%r12\n\t"
        "ud2\n\t");
}
#  else
#    define KAI_SEG_A64_SWITCH \
        "sub sp, sp, #160\n\t" \
        "stp x19, x20, [sp, #0]\n\t" \
        "stp x21, x22, [sp, #16]\n\t" \
        "stp x23, x24, [sp, #32]\n\t" \
        "stp x25, x26, [sp, #48]\n\t" \
        "stp x27, x28, [sp, #64]\n\t" \
        "stp x29, x30, [sp, #80]\n\t" \
        "stp d8, d9, [sp, #96]\n\t" \
        "stp d10, d11, [sp, #112]\n\t" \
        "stp d12, d13, [sp, #128]\n\t" \
        "stp d14, d15, [sp, #144]\n\t" \
        "mov x9, sp\n\t" \
        "str x9, [x0]\n\t" \
        "ldp x19, x20, [x1, #0]\n\t" \
        "ldp x21, x22, [x1, #16]\n\t" \
        "ldp x23, x24, [x1, #32]\n\t" \
        "ldp x25, x26, [x1, #48]\n\t" \
        "ldp x27, x28, [x1, #64]\n\t" \
        "ldp x29, x30, [x1, #80]\n\t" \
        "ldp d8, d9, [x1, #96]\n\t" \
        "ldp d10, d11, [x1, #112]\n\t" \
        "ldp d12, d13, [x1, #128]\n\t" \
        "ldp d14, d15, [x1, #144]\n\t" \
        "add sp, x1, #160\n\t" \
        "mov x0, x2\n\t" \
        KAI_SEG_A64_RETURN

/* `entry` waits in x19 and `arg` is already in x0. The undefined return
 * address ends an unwinder's walk here. */
#    define KAI_SEG_A64_BOOT \
        ".cfi_undefined x30\n\t" \
        "blr x19\n\t" \
        "brk #0\n\t"

#    if defined(__clang__)
KAI_SEG_NAKED
static void *kai_seg_switch(void **save, void *to, void *arg) {
    __asm__ volatile(KAI_SEG_A64_SWITCH);
}

KAI_SEG_NAKED
static void kai_seg_boot(void) {
    __asm__ volatile(KAI_SEG_A64_BOOT);
}
#    else
extern void *kai_seg_switch(void **save, void *to, void *arg) __attribute__((visibility("hidden")));
extern void kai_seg_boot(void) __attribute__((visibility("hidden")));
__asm__(
    ".pushsection .text\n\t"
    ".p2align 2\n\t"
    ".weak kai_seg_switch\n\t"
    ".hidden kai_seg_switch\n\t"
    ".type kai_seg_switch, %function\n"
    "kai_seg_switch:\n\t"
    ".cfi_startproc\n\t"
    KAI_SEG_A64_SWITCH
    ".cfi_endproc\n\t"
    ".size kai_seg_switch, .-kai_seg_switch\n\t"
    ".weak kai_seg_boot\n\t"
    ".hidden kai_seg_boot\n\t"
    ".type kai_seg_boot, %function\n"
    "kai_seg_boot:\n\t"
    ".cfi_startproc\n\t"
    KAI_SEG_A64_BOOT
    ".cfi_endproc\n\t"
    ".size kai_seg_boot, .-kai_seg_boot\n\t"
    ".popsection\n");
#    endif
#  endif

/* Lay out a fresh frame below `top` so the first switch to the returned
 * stack pointer runs `entry(arg)`. `entry` must never return. The zeroed
 * words above the frame are a null return address and frame pointer, where
 * stack walkers stop. */
static void *kai_seg_frame_init(void *top, void (*entry)(void *)) {
    uintptr_t t = ((uintptr_t) top & ~(uintptr_t) 15) - 16;
    ((void **) t)[0] = NULL;
    ((void **) t)[1] = NULL;
#  if defined(__x86_64__)
    void **sp = (void **) (t - 7 * sizeof(void *));
    memset(sp, 0, 7 * sizeof(void *));
    sp[3] = (void *) (uintptr_t) entry;              /* r12 */
    sp[6] = (void *) (uintptr_t) &kai_seg_boot;      /* return address */
#  else
    void **sp = (void **) (t - 160);
    memset(sp, 0, 160);
    sp[0]  = (void *) (uintptr_t) entry;             /* x19 */
    sp[11] = (void *) (uintptr_t) &kai_seg_boot;     /* x30 */
#  endif
    return sp;
}
#endif /* KAI_SEG_SWITCH */

#if defined(KAI_MAIN_STACK_SIZE)
/* Whether the host grants a stack reservation of `size` bytes plus guard. */
static int kai_stack_fits(size_t size) {
    size_t total = size + kai_page_size();
    void *r = mmap(NULL, total, PROT_READ | PROT_WRITE, kai_stack_map_flags(), -1, 0);
    if (r == MAP_FAILED) return 0;
    munmap(r, total);
    return 1;
}
#endif

#if KAI_HOST_STACK_GUARD
static size_t kai_main_fiber_stack_size(void) {
#if defined(KAI_MAIN_STACK_SIZE)
    /* A host that refuses the reservation (RLIMIT_AS, strict overcommit)
     * gets the largest halving that fits, never less than 8 MiB. */
    static size_t granted = 0;
    if (granted == 0) {
        size_t want = (size_t) (KAI_MAIN_STACK_SIZE);
        while (want / 2 >= 8 * 1024 * 1024 && !kai_stack_fits(want)) want /= 2;
        granted = want;
    }
    return granted;
#endif
    size_t sz = 8 * 1024 * 1024;
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) == 0
        && rl.rlim_cur != RLIM_INFINITY
        && (size_t) rl.rlim_cur > sz) {
        sz = (size_t) rl.rlim_cur;
    }
    if (sz > 64 * 1024 * 1024) sz = 64 * 1024 * 1024;
    size_t fiber = kai_fiber_stack_size();
    return fiber > sz ? fiber : sz;
}

/* SIGSEGV / SIGBUS handler for fiber stack overflow. The fault lands
 * on the active fiber's guard page (PROT_NONE); we print a diagnostic
 * and re-raise with the default disposition. Faults outside any guard
 * (e.g. NULL deref in user code) fall through to default — we only
 * decorate the stack-overflow case. The handler runs on a sigaltstack
 * so the overflowed stack is never used to format the message. Spec:
 * `docs/fibers-honesty-targets.md` Tier 1. */
/* Two different scopes live here, and conflating them loses the diagnostic:
 * the SIGSEGV/SIGBUS disposition is process-wide (installed once, guarded by
 * kai_sigsegv_installed), while sigaltstack is PER-THREAD. A thread without
 * an alternate stack gives SA_ONSTACK nothing to switch to, so the handler
 * would run on the stack that just overflowed, fault again, and the process
 * dies with no message. Every thread that can run a fiber installs its own. */
#if defined(KAI_SEPARATE_COMPILATION)
extern int   kai_sigsegv_installed;
#  if defined(KAI_RUNTIME_OWNER)
int   kai_sigsegv_installed = 0;
#  endif
#else
static int   kai_sigsegv_installed = 0;
#endif

static void kai_fiber_sigsegv_handler(int sig, siginfo_t *info, void *ucp) {
    (void) ucp;
    KaiFiber *f = kai_worker_here()->active;
    if (f && f->stack_base && info && info->si_addr) {
        char *guard_lo = (char *) f->stack_base;
        char *guard_hi = guard_lo + kai_page_size();
        char *addr     = (char *) info->si_addr;
        if (addr >= guard_lo && addr < guard_hi) {
            char buf[96];
            int n = snprintf(buf, sizeof(buf),
                             "kai: fiber stack overflow at %p\n", (void *) f);
            if (n > 0) {
                ssize_t w = write(2, buf, (size_t) n);
                (void) w;
            }
        }
    }
    /* Re-raise with default disposition so the process terminates with
     * the original signal — preserving the standard SIGSEGV exit
     * status for callers (shell `$?` = 139) without swallowing
     * unrelated faults. */
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Give the calling thread an alternate signal stack, adopting one that is
 * already installed rather than displacing it.
 *
 * TRAP: a sanitizer runtime (and any embedding host that follows the same
 * pattern) installs its own alternate stack per thread and, at thread exit,
 * munmaps whatever `sigaltstack` hands back. Displacing it therefore makes
 * that owner unmap OUR allocation — a malloc'd, unaligned pointer — which
 * fails with EINVAL and is fatal under ASAN. Adopting costs nothing:
 * SA_ONSTACK only needs a stack that is not the one that overflowed.
 *
 * Idempotent; an allocation we make stays owned by the worker for the
 * thread's life. */
static void kai_install_thread_sigaltstack(KaiWorker *w) {
    if (w->sigalt_ready) return;

    stack_t cur;
    if (sigaltstack(NULL, &cur) == 0 && cur.ss_sp && !(cur.ss_flags & SS_DISABLE)) {
        w->sigalt_ready = 1;
        return;
    }

    size_t altsize = (size_t) SIGSTKSZ;
    if (altsize < 32 * 1024) altsize = 32 * 1024;
    void *sp = malloc(altsize);
    if (!sp) return;
    w->sigalt_stack = sp;
    w->sigalt_ready = 1;

    stack_t ss;
    ss.ss_sp    = sp;
    ss.ss_size  = altsize;
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
}

static void kai_install_fiber_sigsegv_handler(void) {
    kai_install_thread_sigaltstack(kai_worker_here());
    if (kai_sigsegv_installed) return;
    kai_sigsegv_installed = 1;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = kai_fiber_sigsegv_handler;
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
}
#elif defined(__wasm__)
static size_t kai_main_fiber_stack_size(void) { return kai_fiber_stack_size(); }
static void kai_install_thread_sigaltstack(KaiWorker *w) { (void) w; }
static void kai_install_fiber_sigsegv_handler(void) {}
#endif

/* =================================================================
 * Phase R1 reactor — issue #611
 * =================================================================
 *
 * Single-threaded readiness reactor wired into the cooperative
 * scheduler. Three surfaces park the calling fiber instead of the
 * OS thread:
 *   - Spawn.sleep(ns)        — timer wheel keyed on CLOCK_MONOTONIC.
 *   - File defaults          — thread-pool offload (4 workers).
 *   - Process.wait(child)    — SIGCHLD self-pipe + pid waiter map.
 *
 * OWNERSHIP INVARIANT (M:N §4, F2-ready boundary): all reactor state
 * below — timer wheel, waiter lists, self-pipes, parked count, the file
 * pool — is single-owner: it is read and mutated ONLY from inside
 * `kai_reactor_wait` and its drain helpers (kai_reactor_*_drain), which
 * are in turn called ONLY from `kai_reactor_wait`. No other code path
 * touches it, and every ready-fiber handback goes through `kai_sched_unpark`
 * from a drain. That single-owner discipline is what makes the F2 lift of
 * `poll()` onto its own thread mechanical: only the handback (unpark →
 * route to home_thread) changes; the state does not move.
 *
 * Wait primitive is `poll()` on two self-pipes (one for SIGCHLD,
 * one for file-pool completions) plus a deadline-derived timeout.
 * The kqueue/epoll path the parent issue (#474) sketched is queued
 * for R2 when TCP sockets need readiness notifications; R1's three
 * surfaces never wait directly on regular FDs, so the simpler
 * `poll()` shape covers them portably without per-platform
 * branching.
 *
 * Sockets (`NetTcp`) intentionally stay on the blocking syscall
 * path until R2 ships in Orongo together with the Cancel redesign.
 * The `NetTcp` runtime note in this file is left untouched and the
 * `docs/effects-stdlib.md` sidebar for the surface continues to
 * advertise the blocking shape.
 */

/* `kai_sched_unpark` is forward-declared earlier (right after the
 * KaiFiber typedef) so the reactor drain helpers below can wake
 * promoted fibers without needing a second prototype. */

/* Self-pipe halves. The SIGCHLD handler writes one byte to
 * kai_reactor_sigchld_pipe[1] from signal context (write(2) is
 * async-signal-safe per POSIX). The reactor poll watches the read
 * half. The file-pool worker threads write one byte to
 * kai_reactor_filepool_pipe[1] on completion to wake the main
 * thread from poll(). Both pipes are O_NONBLOCK so neither writer
 * ever blocks; the reactor drains them with read() in a loop. */
/* The reactor is one process-global structure: a fiber parks on a waiter
 * list in one TU and kai_reactor_wait (compiled in a single TU) polls the
 * self-pipes and drains those same lists. All reactor state — pipes, timer
 * wheel, waiter lists, parked count, and the file-pool below — is therefore
 * shared (owner defines, others extern); split ownership would let one TU
 * enqueue a waiter the polling TU never sees, or poll a pipe another TU
 * never opened. */
#if defined(KAI_SEPARATE_COMPILATION)
extern int kai_reactor_sigchld_pipe[2];
extern int kai_reactor_filepool_pipe[2];
#  if defined(KAI_RUNTIME_OWNER)
int kai_reactor_sigchld_pipe[2]  = { -1, -1 };
int kai_reactor_filepool_pipe[2] = { -1, -1 };
#  endif
#else
static int kai_reactor_sigchld_pipe[2]  = { -1, -1 };
static int kai_reactor_filepool_pipe[2] = { -1, -1 };
#endif

/* Issue #671 — Phase R4 reactor: Signal effect self-pipe. The
 * sa_handler installed by `signal_on` writes the signo to
 * kai_reactor_signal_pipe[1] from signal context (one byte per
 * delivery; write(2) is async-signal-safe per POSIX). The reactor
 * poll watches the read half and drains it on wake, mapping signo
 * → variant and waking the parked waiter. Replaces the v1
 * sigwait body of `kai_default_signal_await` which blocked the
 * entire OS thread. */
#if defined(KAI_SEPARATE_COMPILATION)
extern int kai_reactor_signal_pipe[2];
#  if defined(KAI_RUNTIME_OWNER)
int kai_reactor_signal_pipe[2]   = { -1, -1 };
#  endif
#else
static int kai_reactor_signal_pipe[2]   = { -1, -1 };
#endif

/* Sorted timer-wheel head (intrusive list of parked fibers chained
 * through f->reactor_next, ordered by ascending deadline). Insertion
 * is O(n); for v1 with handfuls of concurrent sleepers this stays
 * well under the noise floor of poll() itself. A heap is queued for
 * Orongo if the wheel ever shows up on a profile. */
#if defined(KAI_SEPARATE_COMPILATION)
extern KaiFiber *kai_reactor_timer_head;
#  if defined(KAI_RUNTIME_OWNER)
KaiFiber *kai_reactor_timer_head = NULL;
#  endif
#else
static KaiFiber *kai_reactor_timer_head = NULL;
#endif

/* Process-wait map and file-pool waiter list. Both are intrusive
 * single-linked through f->reactor_next, so a fiber can sit on at
 * most one reactor structure at a time (asserted by the parking
 * call sites — a fiber awaiting a pid cannot simultaneously sleep
 * or sit on a file-pool completion). */
#if defined(KAI_SEPARATE_COMPILATION)
extern KaiFiber *kai_reactor_pid_waiters;
extern KaiFiber *kai_reactor_filepool_waiters;
#  if defined(KAI_RUNTIME_OWNER)
KaiFiber *kai_reactor_pid_waiters     = NULL;
KaiFiber *kai_reactor_filepool_waiters = NULL;
#  endif
#else
static KaiFiber *kai_reactor_pid_waiters     = NULL;
static KaiFiber *kai_reactor_filepool_waiters = NULL;
#endif

/* Exit statuses reaped by the SIGCHLD drain before any fiber parked
 * on the pid. A child can die inside the window between the wait
 * op's non-blocking waitpid and its park (or before wait is called
 * at all — start → work → wait is the normal piped-child shape);
 * the drain's waitpid(-1) wins that race and the status would be
 * lost. Buffered nodes are consumed by `kai_reactor_take_child_exit`
 * or by the drain's reconcile pass once the fiber does park. */
typedef struct KaiPendingExit {
    int pid;
    int status;
    struct KaiPendingExit *next;
} KaiPendingExit;

#if defined(KAI_SEPARATE_COMPILATION)
extern KaiPendingExit *kai_reactor_pending_exits;
#  if defined(KAI_RUNTIME_OWNER)
KaiPendingExit *kai_reactor_pending_exits = NULL;
#  endif
#else
static KaiPendingExit *kai_reactor_pending_exits = NULL;
#endif

/* Issue #620 — Phase R3 reactor: stdin slot. Singleton because
 * STDIN_FILENO is process-shared; multiple fibers reading the same
 * pipe concurrently is a logic bug (the bytes would shred). The
 * parking op rejects with a clear panic if a second fiber tries.
 * Wake source is POLLIN on fd 0, drained alongside the SIGCHLD /
 * file-pool self-pipes by `kai_reactor_wait`. */
#if defined(KAI_SEPARATE_COMPILATION)
extern KaiFiber *kai_reactor_stdin_waiter;
extern int       kai_reactor_stdin_orig_flags;
#  if defined(KAI_RUNTIME_OWNER)
KaiFiber *kai_reactor_stdin_waiter = NULL;
int       kai_reactor_stdin_orig_flags = -1;
#  endif
#else
static KaiFiber *kai_reactor_stdin_waiter = NULL;
static int       kai_reactor_stdin_orig_flags = -1;
#endif

/* Issue #630 — Phase R2 reactor: per-direction socket waiter lists.
 * One fiber-per-(fd, direction); the same fd may simultaneously have
 * a reader and a writer parked (rare in v1 — typical HTTP server
 * fibers serialise send/recv on a Conn — but it is the correct
 * semantics for full-duplex sockets and costs nothing to support).
 * Each list is intrusive through `f->reactor_next`; the fd lives in
 * `f->reactor_wait_pid` (the slot doubles as "what are we waiting
 * for"; pid waiters and socket waiters are mutually exclusive). */
#if defined(KAI_SEPARATE_COMPILATION)
extern KaiFiber *kai_reactor_socket_read_waiters;
extern KaiFiber *kai_reactor_socket_write_waiters;
#  if defined(KAI_RUNTIME_OWNER)
KaiFiber *kai_reactor_socket_read_waiters  = NULL;
KaiFiber *kai_reactor_socket_write_waiters = NULL;
#  endif
#else
static KaiFiber *kai_reactor_socket_read_waiters  = NULL;
static KaiFiber *kai_reactor_socket_write_waiters = NULL;
#endif

/* Issue #671 — Phase R4 reactor: Signal waiter slot. Singleton
 * because only one fiber can sit on `Signal.await()` at a time —
 * the signo arrives in the self-pipe regardless of which fiber
 * is waiting, so multiple waiters would race over the byte. A
 * second concurrent `signal_await` panics with a clear message
 * (mirrors the stdin-multiplex panic from R3). Wake source is
 * POLLIN on `kai_reactor_signal_pipe[0]`, drained alongside the
 * SIGCHLD / file-pool / stdin pipes by `kai_reactor_wait`. The
 * delivered signo is parked in `f->reactor_data` (a void * slot)
 * so the await handler can rebuild the variant on resume. */
#if defined(KAI_SEPARATE_COMPILATION)
extern KaiFiber *kai_reactor_signal_waiter;
#  if defined(KAI_RUNTIME_OWNER)
KaiFiber *kai_reactor_signal_waiter = NULL;
#  endif
#else
static KaiFiber *kai_reactor_signal_waiter = NULL;
#endif

/* Signo delivered while no fiber was parked on the Signal waiter
 * slot. Sticky: the drain stashes it here instead of discarding, and
 * the next `Signal.await()` consumes it without parking. Without this
 * slot a signal landing between a subscriber announcing readiness and
 * its park commit is lost and the waiter sleeps forever — the old
 * sigwait body never had that window because the kernel queued
 * blocked signals. Guarded by kai_reactor_mu at N>1. */
#if defined(KAI_SEPARATE_COMPILATION)
extern int kai_reactor_signal_pending;
#  if defined(KAI_RUNTIME_OWNER)
int kai_reactor_signal_pending = 0;
#  endif
#else
static int kai_reactor_signal_pending = 0;
#endif

/* Aggregate count of fibers parked on any reactor structure.
 * kai_sched_park reads this to decide between "no one can wake us
 * up — deadlock" (count == 0) and "block on the reactor until a
 * timer/SIGCHLD/file-pool event arrives" (count > 0). */
#if defined(KAI_SEPARATE_COMPILATION)
extern int kai_reactor_parked_count;
#  if defined(KAI_RUNTIME_OWNER)
int kai_reactor_parked_count = 0;
#  endif
#else
static int kai_reactor_parked_count = 0;
#endif

/* Set while the reactor thread is blocked in poll() with nothing armed —
 * kai_reactor_parked_count was 0 when it built the poll set, so poll waits
 * forever on the self-pipes and can promote no one until a commit pokes it.
 * The global-quiescence deadlock check reads this to tell a genuinely idle
 * reactor from one mid-drain: the flag is 0 from poll-return all the way
 * through kai_reactor_flush_ready, so the check can never fire on a fiber
 * the reactor is handing back to a deque. Written only under kai_reactor_mu
 * (the set-idle store) and right after poll() (the clear); N>1 only. */
#if defined(KAI_SEPARATE_COMPILATION)
extern _Atomic int kai_reactor_idle;
#  if defined(KAI_RUNTIME_OWNER)
_Atomic int kai_reactor_idle = 0;
#  endif
#else
static _Atomic int kai_reactor_idle = 0;
#endif

