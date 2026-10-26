/*
 * The stage 2 runtime: included by the C the compiler emits and by the
 * native backend's runtime (stage0/runtime_llvm.c).
 *
 * A boxed value is a `KaiValue *`: a reference-counted block with a tag,
 * or, with the low bit set, an immediate small Int. Where the type is
 * known, scalars travel unboxed (raw parameters and returns, typed
 * variant slots, `Vec` elements).
 *
 * An owned argument is released by the callee or kept in its result; a
 * borrowed (`^`) parameter is not. Closures are called through
 * `kai_apply`.
 *
 * Under KAI_SEPARATE_COMPILATION, state shared between units is `extern`
 * and defined once, by the KAI_RUNTIME_OWNER unit.
 */

/* m8.x cooperative scheduler substrate: ucontext.
 *
 * The _XOPEN_SOURCE feature-test macro must be defined BEFORE ANY
 * system header is included, otherwise sys/types.h (transitively
 * pulled in by stdio.h, time.h, etc.) freezes the legacy POSIX
 * ucontext_t layout (~56 bytes) instead of the full XSI shape
 * (~880 bytes on darwin arm64). swapcontext then writes 880 bytes
 * into a 56-byte buffer, silently corrupting whatever sits next to
 * the embedded ucontext_t — exactly what bit Phase 2 once
 * (the root fiber's evidence_top got clobbered to a saved register
 * value because the static evidence nodes were laid out adjacent).
 * Spec: docs/fibers-impl.md §*macOS deprecation handling*.
 * A header $CFLAGS forces in ahead of this one can reach a system header
 * first, so `kai` then force-includes this block before it:
 * KAI_RUNTIME_FEATURES_ONLY stops that pass here and lets the TU's own
 * include read the rest. The block runs once per translation unit: glibc
 * moves _XOPEN_SOURCE to 700 under _GNU_SOURCE, and a second run would
 * redefine it and warn. */
#ifndef KAI_RUNTIME_FEATURES_SET
#define KAI_RUNTIME_FEATURES_SET
#define _XOPEN_SOURCE 600
/* mmap(MAP_ANON) is a BSD extension hidden by strict _XOPEN_SOURCE.
 * Re-expose it: _DARWIN_C_SOURCE on macOS, _DEFAULT_SOURCE on glibc.
 * The fiber stack allocator (m8.x guard pages) needs anonymous mmap. */
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE 1
#endif
#if defined(__linux__)
#  define _DEFAULT_SOURCE 1
/* glibc hides `dladdr` + `Dl_info` (a GNU extension, not C99) behind
 * _GNU_SOURCE; the --debug panic backtrace (#500) needs them to de-slide
 * PIE frames. Must be defined BEFORE any include, like the macros above —
 * a strict `-std=c99` C-only bootstrap rejects the implicit declaration
 * otherwise (macOS libSystem exposes them regardless, which is why a mac
 * build did not catch it). _GNU_SOURCE implies _DEFAULT_SOURCE. */
#  define _GNU_SOURCE 1
#endif
#endif

#if defined(KAI_RUNTIME_FEATURES_ONLY)
#  undef KAI_RUNTIME_FEATURES_ONLY
#elif !defined(KAI_RUNTIME_H)
#define KAI_RUNTIME_H
#include "runtime/host.h"
#include "runtime/types.h"
#include "runtime/rc_trace.h"
#include "runtime/heap.h"
#include "runtime/alloc.h"
#include "runtime/mn_infra.h"
#include "runtime/ctors.h"
#include "runtime/ranges.h"
#include "runtime/perceus_vec.h"
#include "runtime/vec_ops.h"
#include "runtime/core_basic.h"
#include "runtime/core_collections.h"
#include "runtime/core_ops_io.h"
#include "runtime/core_fileio.h"
#include "runtime/core_parse.h"
#include "runtime/harness.h"
#ifndef KAI_NO_BENCH_CHECK
#include "runtime/harness_extra.h"
#endif
#include "runtime/effects.h"
#include "runtime/net_handlers.h"
#include "runtime/signal_process.h"
#include "runtime/sched_base.h"
#include "runtime/reactor.h"
#include "runtime/reactor_thread.h"
#include "runtime/mn_worker.h"
#include "runtime/mn_boot.h"
#include "runtime/mn_foreign.h"
#include "runtime/evidence.h"
#include "runtime/segments.h"

#if defined(KAI_LLVM) || !defined(KAI_NO_LLVM_BRIDGE)
#include "runtime/llvm_bridge.h"
#endif

#endif /* KAI_RUNTIME_H */
