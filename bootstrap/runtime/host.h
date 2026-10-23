/* Fragment of runtime.h; include runtime.h, never this file. */

/* Host facilities, one macro each, 1 where the host provides it. At 0 the
 * runtime includes none of the facility's headers and calls none of its libc
 * entry points; reaching it stops the program with a diagnostic naming it.
 *
 *   KAI_HOST_FIBERS       context switch by ucontext; without it the program
 *                         runs on one thread and has one fiber
 *   KAI_HOST_STACK_GUARD  mapped stacks, their guard page, the overflow
 *                         handler; without it no stack segment either
 *   KAI_HOST_REACTOR      self-pipes, poll, the file pool
 *   KAI_HOST_SOCKETS      sockets and DNS
 *   KAI_HOST_PROCESS      child processes
 *   KAI_HOST_SIGNALS      signal subscription
 *   KAI_HOST_BACKTRACE    the stack trace of a debug build's panic; without
 *                         it the panic says there is none
 *   KAI_HOST_UNWIND       the frame unwinder; without it a contained trap
 *                         lands at once and the frames it crosses keep
 *                         their references */
#if defined(__wasm__)
#  ifndef KAI_HOST_FIBERS
#    define KAI_HOST_FIBERS 1
#  endif
#  ifndef KAI_HOST_STACK_GUARD
#    define KAI_HOST_STACK_GUARD 0
#  endif
#  ifndef KAI_HOST_REACTOR
#    define KAI_HOST_REACTOR 0
#  endif
#  ifndef KAI_HOST_SOCKETS
#    define KAI_HOST_SOCKETS 0
#  endif
#  ifndef KAI_HOST_PROCESS
#    define KAI_HOST_PROCESS 0
#  endif
#  ifndef KAI_HOST_SIGNALS
#    define KAI_HOST_SIGNALS 0
#  endif
#  ifndef KAI_HOST_BACKTRACE
#    define KAI_HOST_BACKTRACE 0
#  endif
#  ifndef KAI_HOST_UNWIND
#    define KAI_HOST_UNWIND 0
#  endif
#endif
#ifndef KAI_HOST_FIBERS
#  define KAI_HOST_FIBERS 1
#endif
#ifndef KAI_HOST_STACK_GUARD
#  define KAI_HOST_STACK_GUARD 1
#endif
#ifndef KAI_HOST_REACTOR
#  define KAI_HOST_REACTOR 1
#endif
#ifndef KAI_HOST_SOCKETS
#  define KAI_HOST_SOCKETS 1
#endif
#ifndef KAI_HOST_PROCESS
#  define KAI_HOST_PROCESS 1
#endif
#ifndef KAI_HOST_SIGNALS
#  define KAI_HOST_SIGNALS 1
#endif
#ifndef KAI_HOST_BACKTRACE
#  define KAI_HOST_BACKTRACE 1
#endif
#ifndef KAI_HOST_UNWIND
#  define KAI_HOST_UNWIND 1
#endif
#if KAI_HOST_FIBERS && !KAI_HOST_STACK_GUARD && !defined(__wasm__)
#  error "KAI_HOST_FIBERS needs KAI_HOST_STACK_GUARD: a fiber runs on a mapped stack"
#endif
#if (KAI_HOST_SOCKETS || KAI_HOST_PROCESS || KAI_HOST_SIGNALS) && !KAI_HOST_REACTOR
#  error "KAI_HOST_SOCKETS, KAI_HOST_PROCESS and KAI_HOST_SIGNALS need KAI_HOST_REACTOR: their waits park on it"
#endif
#if defined(KAI_MAIN_STACK_SIZE) && !KAI_HOST_FIBERS
#  error "KAI_MAIN_STACK_SIZE needs KAI_HOST_FIBERS: main moves to its stack by a context switch"
#endif
#define KAI_HOST_COMPLETE (KAI_HOST_FIBERS && KAI_HOST_STACK_GUARD && KAI_HOST_REACTOR \
                           && KAI_HOST_SOCKETS && KAI_HOST_PROCESS && KAI_HOST_SIGNALS \
                           && KAI_HOST_BACKTRACE && KAI_HOST_UNWIND)

#include <dirent.h>
#include <math.h>
#include <setjmp.h>
#if KAI_HOST_UNWIND
#include <unwind.h>
#endif
#if KAI_HOST_SIGNALS || KAI_HOST_PROCESS || KAI_HOST_REACTOR || KAI_HOST_STACK_GUARD || defined(KAI_LLVM)
#include <signal.h>
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>      /* sched_getaffinity() — CPU count for the KAI_THREADS default */
#if KAI_HOST_STACK_GUARD
#include <sys/mman.h>
#include <sys/resource.h>  /* getrlimit(RLIMIT_STACK) — stack budget for the kai_main fiber */
#endif
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if KAI_HOST_BACKTRACE
#include <execinfo.h>   /* backtrace() for the --debug panic stack trace (#500) */
#include <dlfcn.h>      /* dladdr() — main-image load address to de-slide PIE frames */
#endif

/* stdout is block-buffered off a TTY: flush before anything that can
 * outlive or reorder buffered bytes (a wait, a stderr write, a stdin read). */
#ifdef __APPLE__
static inline int kai_stdout_pending(void) { return stdout->_p != stdout->_bf._base; }
#else
#include <stdio_ext.h>
static inline int kai_stdout_pending(void) { return __fpending(stdout) != 0; }
#endif
static inline void kai_flush_stdout(void) { if (kai_stdout_pending()) fflush(stdout); }

/* Every runtime stderr write goes through these so earlier stdout bytes
 * stay ahead of it in a merged log. */
__attribute__((unused, cold, noinline, format(printf, 1, 2)))
static void kai_eprintf(const char *fmt, ...) {
    kai_flush_stdout();
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

__attribute__((unused, cold, noinline))
static void kai_eputs(const char *s) {
    kai_flush_stdout();
    fputs(s, stderr);
}

/* Every runtime exit goes through kai_exit: exit() tears down LLVM's lazily
 * built statics, so the native emit pool is drained first. */
#ifdef KAI_LLVM
static void kai_emit_pool_drain(int code);
#endif
static void kai_exit(int code) __attribute__((noreturn));
static void kai_exit(int code) {
#ifdef KAI_LLVM
    kai_emit_pool_drain(code);
#endif
    exit(code);
}

#if !KAI_HOST_COMPLETE
__attribute__((noreturn))
static void kai_host_absent(const char *facility) {
    kai_eprintf("kai: this host has no %s\n", facility);
    kai_exit(1);
}
#endif

/* Purity attribute for a confirmed memory-clean fn (issue #1139): the C
 * emitter prefixes `KAI_CONST` on the signature of a fn whose lowered
 * body performs no memory access, so `cc -O2` can CSE / hoist / fold
 * calls to it. `((const))` promises the result depends only on the
 * arguments and the fn reads no memory through them — the C mirror of
 * LLVM `memory(none)`. Only GCC/Clang honour it; elsewhere it vanishes.
 * Defined unconditionally (NOT inside KAI_TRACE_RC), since every C build
 * the emitter targets references it. */
#if defined(__GNUC__) || defined(__clang__)
#define KAI_CONST __attribute__((const))
#else
#define KAI_CONST
#endif

/* M:N scheduler — Class A per-scheduler-thread state (docs/mn-scheduler-
 * design.md §1). Each OS scheduler thread owns its own copy; a fiber runs on
 * exactly one thread at a time and only ever touches that thread's copy, so
 * the allocator pools, region arena stack, scheduler core, and RC/trace
 * ledgers need no lock on the hot path. Under a single thread `_Thread_local`
 * is semantically identical to a plain global, so N=1 stays byte-identical.
 * Every global carrying KAI_TLS is classified `tls` in the audit gate
 * (tools/runtime-globals.allow).
 *
 * TRAP: the compiler treats a thread-local's address as constant for a whole
 * function activation and may keep it across any call. A fiber can park on
 * one thread and resume on another, so a function that materialises such an
 * address must not be able to switch context. `tools/tls-hoist-gate.sh`
 * checks this on the hot bitcode and on the optimised owner.
 *
 * Under separate compilation these become `extern` across objects, which
 * defaults to the general-dynamic TLS model (resolved via __tls_get_addr).
 * Pin initial-exec: it resolves the address off the thread pointer directly
 * (like the single-TU `static` local-exec), which TSAN models as
 * thread-private — general-dynamic's opaque __tls_get_addr otherwise trips
 * TSAN into a false cross-thread race on per-thread state. Sound because the
 * owner links statically (initial-exec forbids dlopen, not static link). */
#if defined(KAI_SEPARATE_COMPILATION) && (defined(__GNUC__) || defined(__clang__))
#define KAI_TLS _Thread_local __attribute__((tls_model("initial-exec")))
#else
#define KAI_TLS _Thread_local
#endif

/* Linkage of the fiber suspend-point ops the C backend calls by name — the
 * mailbox recv/send surface, spawn_actor, kai_sched_bootstrap, and every
 * default effect handler that parks a fiber (Spawn, the reactor-backed NetTcp /
 * Stdin / Clock / Process / Signal). Default `static` keeps the single-TU C
 * path auto-contained and byte-identical (sound under gcc). Under separate
 * compilation these move to the owner object: the owner defines them with
 * external linkage, the program TU sees a prototype, and DCE drops the
 * suspend-point closure they anchor (kai_sched_park, the reactor, the
 * trampoline) from the program object.
 * The axis is KAI_SEPARATE_COMPILATION (linkage), orthogonal to KAI_HOT_ONLY
 * (which governs the native bitcode's shim elision). */
#if defined(KAI_SEPARATE_COMPILATION)
#  if defined(KAI_RUNTIME_OWNER)
#    define KAI_SCHED_FN
#  else
#    define KAI_SCHED_FN extern
#  endif
#else
#  define KAI_SCHED_FN static
#endif
/* Its body is present in the owner and single-TU builds, elided to a bare
 * prototype in a non-owner separate-compilation TU:
 *   KAI_SCHED_FN <ret> name(args)
 *   #if KAI_SCHED_DECL_ONLY
 *   ;
 *   #else
 *   { ...body... }
 *   #endif
 * The body is never wrapped in a macro — compiler errors and byte-id stay honest. */
#if defined(KAI_SEPARATE_COMPILATION) && !defined(KAI_RUNTIME_OWNER)
#  define KAI_SCHED_DECL_ONLY 1
#else
#  define KAI_SCHED_DECL_ONLY 0
#endif

/* Runtime helpers the owner exports and the hot bitcode defines external, so a
 * native partition holds them only as `available_externally` inlining
 * candidates and calls the owner's copy otherwise. Their bodies must not
 * depend on KAI_HOT_ONLY or KAI_RUNTIME_OWNER: an inlined copy and the
 * owner's must be the same function. Elsewhere they stay `static`. */
#if defined(KAI_SEPARATE_COMPILATION) && (defined(KAI_RUNTIME_OWNER) || defined(KAI_HOT_ONLY))
#  define KAI_RT_SHARED
#  define KAI_RT_SHARED_INLINE extern inline
#else
#  define KAI_RT_SHARED static
#  define KAI_RT_SHARED_INLINE static inline
#endif

/* Number of OS scheduler threads (M:N). Read once from KAI_THREADS at
 * startup (kai_sched_bootstrap), immutable after — published before any
 * worker spawns, so no lock. Class B. Declared here, ahead of the
 * allocator, because the slab/pool hot paths branch on it (N==1 keeps
 * the pre-M:N single-thread behaviour byte-identical). */
#if defined(KAI_SEPARATE_COMPILATION)
extern int kai_nthreads;
#  if defined(KAI_RUNTIME_OWNER)
int kai_nthreads = 1;
#  endif
#else
static int kai_nthreads = 1;
#endif

/* net-tcp-v1 — sockets API for the NetTcp default handler.
 * POSIX everywhere we ship: macOS, Linux, *BSD. The handler is
 * blocking-only: the m8.x cooperative scheduler (landed v0.4.0)
 * suspends fibers on mailbox / await / yield, but we have no
 * readiness reactor yet, so socket reads/writes park the OS thread
 * rather than the fiber. kqueue / epoll integration is a Tier 2
 * follow-up tracked in docs/fibers-honesty-targets.md §Reactor. */
#if KAI_HOST_SOCKETS
#include <arpa/inet.h>
#endif
#include <errno.h>
#if KAI_HOST_SOCKETS
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#include <sys/types.h>
#if KAI_HOST_SOCKETS
#include <sys/un.h>
#endif
#if KAI_HOST_PROCESS || KAI_HOST_REACTOR
#include <sys/wait.h>
#endif
#if KAI_HOST_SOCKETS && !defined(SO_PEERCRED) && defined(LOCAL_PEERCRED)
#include <sys/ucred.h>
#endif

/* Issue #611 — Phase R1 reactor support. `poll()` is the wait
 * primitive (POSIX everywhere we ship), `pthread` powers the file
 * I/O worker pool, and `fcntl` toggles O_NONBLOCK on the self-pipes
 * so the SIGCHLD handler and worker threads never block on write.
 * Sockets stay on the blocking path until R2. */
#include <fcntl.h>
#include <limits.h>
#if KAI_HOST_REACTOR
#include <poll.h>
#endif
#include <pthread.h>
#include <stdatomic.h>

/* Stack buffer size for filesystem path operations (realpath, dirname,
 * join, etc.). Replaces the bare 4096 literal that was copy-pasted across
 * ~14 path sites. Floors at 4096 to preserve the historical buffer size
 * on every platform: darwin defines PATH_MAX as 1024 and Linux as 4096,
 * so a plain `PATH_MAX` would have *shrunk* the macOS buffer and started
 * truncating paths in [1024, 4096). The max() keeps the old behaviour
 * everywhere and only grows on systems with a larger PATH_MAX. POSIX lets
 * PATH_MAX be undefined when the max is unbounded (GNU/Hurd); the floor
 * covers that case too. */
#if defined(PATH_MAX) && PATH_MAX > 4096
#  define KAI_PATH_BUF PATH_MAX
#else
#  define KAI_PATH_BUF 4096
#endif

/* Stack buffer for environment-variable *names* (get_var/set_var/unset_var).
 * 1024 was copy-pasted across the three env entry points; named here so the
 * three stay in lock-step. POSIX does not bound env name length, but 1024 is
 * far past any real variable name; over-long names are truncated, matching
 * the prior behaviour. */
#define KAI_ENV_NAME_BUF 1024

/* Stack buffer for network host strings passed to getaddrinfo (connect and
 * listen sites). NI_MAXHOST is 1025 on most platforms; 256 is the historical
 * pragmatic bound shared by both call sites — kept as-is, just named. */
#define KAI_NET_HOST_BUF 256

/* Initial heap capacity for the grow-by-doubling stdin read buffers
 * (read_line and the stdin event loop). The buffer doubles via realloc, so
 * this is only the starting point, not a ceiling; 128 covers a typical line
 * without a first reallocation. */
#define KAI_READ_BUF_INIT 128

/* listen(2) backlog for server sockets. Linux caps the effective value at
 * net.core.somaxconn regardless; 128 is the conservative v1 bound shared by
 * every listen site. */
#define KAI_LISTEN_BACKLOG 128

/* Default permission bits for directories created by dir_create (rwxr-xr-x,
 * umask-masked by the kernel). Named to document intent; value unchanged. */
#define KAI_DIR_MODE 0755

/* Apple deprecated ucontext in macOS 10.6 (POSIX-2008 obsoletion).
 * The functions still work; the deprecation attribute on the
 * prototypes triggers -Wdeprecated-declarations, which the local
 * pragma silences. Same trick libco / libtask / Boost.Context use.
 * The same envelope wraps every swap/get/makecontext call site. */
#if defined(__APPLE__) || defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#if KAI_HOST_FIBERS && defined(__wasm__)
#include "wasm_ctx.h"
#elif KAI_HOST_FIBERS
#include <ucontext.h>
#endif
#if defined(__APPLE__) || defined(__clang__)
#  pragma clang diagnostic pop
#endif

#ifndef KAI_STACK_CHECK
#define KAI_STACK_CHECK() ((void) 0)
#endif

/* Not every program uses every core function; silence the
   unused-function warnings that would otherwise pile up in `cc` output
   when stage 0 links only the parts a given program needs. */
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#endif

