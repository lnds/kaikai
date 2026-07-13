# Lane experience — issue #1207 F0: runtime state partitioning for M:N

## Scope

F0 is the first phase of the M:N work-stealing scheduler (#1207, design in
`docs/mn-scheduler-design.md`). It prepares the runtime for multi-thread
**without turning it on**: partition every mutable runtime global into the
three classes of §1, gate the partition statically so it stays total, and
isolate the reactor so its F2 thread-extraction is mechanical. At `N=1`
(the only configuration F0 ships) behaviour is byte-identical to before.

- **Planned:** TLS-ify Class A, classify Class B/C, add the static
  global-annotation gate, extract the reactor `poll()` to its own thread.
- **Shipped:** TLS-ified Class A via a `KAI_TLS` storage-class macro (146
  globals classified, 70 made `_Thread_local`, 14 counters via the
  `KAI_RT_COUNTER` macro), the `tools/runtime-global-audit.sh` gate + its
  `tools/runtime-globals.allow` allow-list wired into tier0/tier1 with a
  self-test that proves it breaks, and the reactor **isolated behind a
  single-owner interface** — NOT extracted to a thread (see below). Plus a
  cache-correctness fix the TLS change surfaced.

## The reactor: isolate, do not extract — the design §4 mismatch

The design §4 said "today `poll()` runs inline in the scheduler loop;
extract it into `kai_reactor_thread`." Reading the runtime showed there **is
no scheduler run-loop**: the runtime is cooperative over `swapcontext`, and
`kai_reactor_wait()` runs *on the stack of the fiber that parked*
(`kai_sched_park` calls it when the local ready-queue is empty), drains the
timer wheel / self-pipes / waiter lists in a **fixed order**, and calls
`kai_sched_unpark` directly.

A real reactor thread in F0 would touch the scheduler thread's
`kai_ready_head/tail` and `kai_parked_count` — which are Class A (TLS) — from
another thread. That is a cross-thread write the "pools are per-thread, no
lock" rule forbids; avoiding it needs the handback queue + condvar handshake
that only pays off once multiple schedulers exist to route to by
`home_thread` (F2). Worse, an asynchronous reactor would break the
deterministic wake order that makes `N=1` byte-identical.

Decision (owner-confirmed, asu-reviewed): **F0 isolates, F2 extracts.** The
reactor state is classified `reactor-owned` in the audit gate, and the
ownership invariant is pinned in the code: all reactor state is read/mutated
only inside `kai_reactor_wait` and its `kai_reactor_*_drain` helpers, which
are called only from `kai_reactor_wait`. Verified structurally: every reactor
drain has exactly one caller (`kai_reactor_wait`), and every reactor-path
`kai_sched_unpark` is paired with a `kai_reactor_parked_count--`. That
single-owner discipline is what makes the F2 lift mechanical — only the
handback (`kai_sched_unpark` → route to `home_thread`) changes; the state
does not move. The issue #1207 body was corrected so the published plan does
not lie (F0 = isolate, F2 = extract).

## The classification — three classes, one macro each where possible

The `KAI_SEPARATE_COMPILATION`/`KAI_RUNTIME_OWNER` guard already existed for a
*different* reason (#989: one shared copy per cross-TU global, not per-TU).
Its axis — "referenced by >1 function" — is orthogonal to TLS ("touched by >1
thread"), but the guard sites are exactly where a TLS storage-class inserts,
so the edit rode the existing structure.

- **Class A (`tls`, 70 globals + 14 counters):** allocator pools, slab, region
  arena stack, scheduler core, RC/arena/vec/prof trace ledgers, RC history /
  site / leak-site tables, per-thread heap accounting, RNG, and the
  sequential test/bench/check harness state. A `KAI_TLS` macro expands to
  `_Thread_local`; under one thread it is byte-identical to a plain global.
  The 14 `KAI_RT_COUNTER` ledgers became TLS by making the macro emit
  `KAI_TLS` — one edit covers them all (verified they are all Class A first).
- **Class B (`immutable` / `immortal`):** dispatch catalogs, protocol
  impl-table, varname/slotmask tables, env config, argv, and the immortal
  `rc=INT32_MAX` singletons + char/int caches. Written once at startup,
  read-only after — no lock needed, no TLS.
- **Class C (`shared-locked` / `reactor-owned`):** string interning, the
  handler-id counter, signal-installed flags (shared-locked), and the whole
  reactor + file pool (reactor-owned).

### Two decisions the design left open, taken here

- **`kai_heap_committed` → `tls`, not atomic.** The design's §1 was ambiguous
  and the inventory flagged it as a Class-C atomic candidate. It sits on the
  alloc hot path (`kai_heap_charge` runs per `kai_alloc`), so an atomic there
  would move the rb-tree bench (the gate-4 canary). Per-thread heap budget is
  the jemalloc thread-cache pattern and is byte-identical at `N=1`. Classified
  `tls`. The `KAI_MAX_HEAP` cap becomes per-thread at `N>1` — the correct
  trade for keeping the RC path atomic-free; documented for F1.
- **Trap-message scratch buffers and the test harness → `tls`.** The harness
  is a sequential runner (never concurrent with user fibers), so `tls` is the
  conservative sound choice and is identical at `N=1`.

## The `&kai_main_fiber` static-init trap

`kai_active_fiber = &kai_main_fiber` cannot be a static initializer once both
are `_Thread_local`: the address of a thread-local is not a compile-time
constant (clang: "initializer element is not a compile-time constant"). Fixed
by initializing to `NULL` and anchoring each thread's active fiber to its own
root fiber in `kai_active_fiber_anchor()`, run from `kai_set_args` (the
per-thread startup point, before any fiber runs) and lazily from
`kai_current_fiber` for the pre-main path. `kai_trap_abort` already treated a
NULL current fiber as "before runtime init → exit," so the pre-anchor window
is safe.

## The structural surprise — a stale native-modular cache = SIGSEGV

The TLS change built and ran byte-identical on the C backend and on
whole-program native, but the **native-modular** path (the default since
#989) SIGSEGV'd at `kai_alloc+32` inside `_kai_proto_init_llvm`, before main.

Root cause, isolated by diffing whole-program (worked) against modular
(crashed) and reading the crash report: the native-modular link's **runtime
owner object** is `runtime_llvm.c` compiled by `bin/kai` with
`-DKAI_RUNTIME_OWNER=1`, cached content-addressed. Its cache key folded
`CC + CFLAGS + defs + arch + runtime_llvm.c` — but **not `runtime.h`**, which
`runtime_llvm.c` `#include`s and which decides the globals' linkage. So a
runtime.h edit (my new `_Thread_local` globals) did not invalidate the cached
owner: the stale owner defined the state globals as plain globals while the
freshly-emitted partitions accessed them as `extern thread_local`. A
thread-local access to a non-thread-local symbol is UB → NULL deref.

Fix: fold `runtime.h` into the owner cache key (`bin/kai`
`resolve_native_modular_runtime_obj`). The c-modular path already folded all
`*.h` into its key — only the native-modular owner had omitted it. This is a
latent correctness bug that any future runtime-owner-affecting change would
have hit; the TLS lane surfaced it. CI (cold cache) never saw it, which is
exactly why it was dangerous. Landed with F0 because F0 is unsound on a warm
dev cache without it (coupled reshape lands together).

## The gate — total by construction, and it proves it breaks

`tools/runtime-global-audit.sh` enumerates every mutable file-scope global
definition across `stage2/runtime.h`, `stage0/runtime_llvm.c`, and
`stage0/runtime.h` (handling `extern`/owner/`static` dedup, pointer types,
multi-line declarations, and excluding typedefs/functions/macros/func-local
statics), and requires each to appear in `tools/runtime-globals.allow` with
its class. Unclassified global → build breaks; removed global → stale entry
breaks. `--self-test` injects an unclassified global into a scratch copy and
requires the enumerator to catch it — a gate that cannot fail is not a gate.
Wired into tier0 (fast, pure shell, no compiler) and tier1.

## stage0/runtime.h — classified, not TLS-ified

`stage0/runtime.h` (the old bootstrap-compiler runtime, no guards) is
classified by the gate so it cannot drift, but its globals are **not** made
TLS: the stage0/stage1 bootstrap binaries never run the M:N scheduler. The
runtime that executes user fibers is `stage2/runtime.h` (C backend + native
via `#include`), and that is where TLS lives. Documented so a future lane
does not mistake the stage0 mirror for a gap.

## Gates

1. **Byte-parity (serial, both backends):** selfhost byte-id
   (`kaic2b.c == kaic2c.c`); rb-tree deterministic output byte-identical (C +
   native); spawn/reactor fixtures byte-identical; concurrent /
   file_concurrent / sleep_concurrent demos run correct on both backends.
2. **Static gate:** 146 globals, 146 classified, 0 unclassified, 0 stale;
   `--self-test` confirms an unclassified global breaks it.
3. **RC ledger reconciles:** `KAI_TRACE_RC` on rb-tree is byte-identical
   TLS vs baseline (`alloc=1000030 free=1000010 leaked=20 incref=2000006
   decref=2000017`).
4. **rb-tree unregressed:** C median 0.38 s vs baseline 0.36 s (noise; the
   0.61 s baseline first-run is warm-up); native median 0.47 s vs pristine
   0.47 s (identical). The RC hot path paid nothing — the TLS ledgers are the
   same increments, `_Thread_local` under one thread is a plain global.

## Follow-ups (for F1, this same worktree)

- **F1 is the continuation of this arc, same worktree:** per-thread
  work-stealing deques, copy-on-cross-thread-send, atomic cancel flag,
  link/monitor system messages, `KAI_THREADS=N` opt-in with `N=1`
  byte-identical. The deliverable is a multi-actor bench with real >1×
  speedup on 4 cores, measured. F1's gates (TSAN clean, N=1 byte-id, rb-tree
  unregressed) do not relax.
- **Class C locks are declared, not yet installed.** `shared-locked`
  classification names the intent; F1 installs the mutex on `str_intern`
  insert and the atomic on `kai_next_handler_id` when threads actually spawn.
  At `N=1` there is no contention, so F0 ships them unguarded and correct.
- **`kai_heap_committed` per-thread cap semantics** surface only at `N>1`
  (F1).
