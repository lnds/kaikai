Runtime mutable-global classification allow-list — M:N scheduler §1.

Every mutable file-scope global in stage2/runtime.h, stage0/runtime_llvm.c,
and stage0/runtime.h has an entry here with its partition class. The gate
`tools/runtime-global-audit.sh` fails the build on any global not listed
(unclassified) or any listed symbol no longer defined (stale). This makes
the TLS partition TOTAL BY CONSTRUCTION.

One file per symbol; the row is `<symbol>` TAB `<class>`.

Classes:
  tls            Class A — per-scheduler-thread; _Thread_local. The hot
                 per-fiber machinery + per-thread trace ledgers (summed at
                 exit). A fiber runs on one thread at a time, so it only
                 ever touches its own thread's copy.
  immutable      Class B — written once at startup (before any worker
                 thread), read-only after. No lock: publication is ordered
                 by the thread-create barrier.
  immortal       Class B — rc=INT32_MAX; shared read-only across threads
                 because RC never applies, so there is no write to race.
  shared-locked  Class C — process-global mutable, touched by >1 thread;
                 guarded by a mutex or an atomic.
  reactor-owned  Class C — single reactor owner; touched by exactly one
                 thread (the F2 reactor thread) or written once at init,
                 so no lock is needed to preserve its identity. The wheel
                 and waiter lists moved to shared-locked when F2 landed
                 (scheduler threads now link/unlink them under
                 kai_reactor_mu); what stays here is init-once fds/flags,
                 the reactor's private ready-batch, and the file pool.
  scratch        per-function return/scratch buffer, referenced by exactly
                 one function; no cross-thread identity to preserve.
── Class A — TLS: allocator pools + slab ──────────────────────────────
── Class A — TLS: region arena stack ──────────────────────────────────
── Class A — TLS: scheduler core ──────────────────────────────────────
One struct per thread: the active fiber, the root fiber, the N=1 ready
queue, the stacks drained on the root after each switch, the pending
frees, the alternate signal stack and the uc_link landing. Read only
through kai_worker_here; code that crossed a switch reaches it through
the running fiber's `worker`.
The same scheduler state as plain globals in the single-threaded stage0
runtime.
Constructor metadata as growable plain globals in the single-threaded
stage0 runtime.
── Class A — TLS: RC / arena / vec trace ledgers (summed at exit) ─────
── Class C — shared-locked: cross-thread ledger reduction ─────────────
The registry the exit report walks to sum the Class A trace ledgers,
plus the accumulator exited threads fold into. All under kai_rc_ledger_mu.
The per-cell counters while the process has one thread, and the switch to
per-thread blocks: written only before the runtime starts a second thread.
── Class C — shared-locked: per-block coverage sites ──────────────────
The set and its counts move under kai_cov_mu; the recent-site cache is
read without it through atomics and cleared under it.
── Class C — shared-locked: RC traffic ledgers ────────────────────────
Not TLS: kai_incref/kai_decref are inlined into fiber bodies, where clang
materialises the thread-pointer and spills it across the park swapcontext.
A work-stolen fiber would then charge the creator thread's counter. Relaxed
atomics on a process-global instead — the address is thread-independent.
── Class A — TLS: RC history / site / leak-site debug tables ─────────
── Class A — TLS: profiler ledgers + stack ───────────────────────────
── Class A — TLS: per-thread heap accounting + RNG ──────────────────────
── Class A — TLS: test / bench / check harness (sequential runner) ────
── Class A — TLS: RC report atexit latches (per-thread, idempotent) ──
── Class B — immutable after init: dispatch catalogs + metadata ──────
── Class B — immutable after init: env config + argv + lazy latches ──
The runtime-bitcode parse thread writes these before the first partition module joins it.
── Class B — immortal (rc=INT32_MAX): singletons + interned caches ───
── Class C — shared-locked / atomic: interning, ids, signal latches ──
Recorded at a tag's first construction, on any thread; atomic words in
a table that never moves.
Pooled stack segments: a segment can finish on another thread than the
one that mapped it, and lands in that thread's pool.
── Class C — shared-locked: M:N work-stealing scheduler registry ─────
Per-thread steal deques, each guarded by its own `mu`. The shutdown
flag is a single-writer/many-reader _Atomic (no lock). At N=1 no worker
spawns and none of this is touched.
Global-quiescence deadlock detection: an idle worker fires when every
worker is idle, the reactor asleep, all deques empty, and a fiber PARKED.
All three are _Atomic, bumped cross-thread on the idle/park/unpark paths;
kai_deadlock_reported is CAS-claimed so exactly one worker reports.
Park/wake ring the deadlock banner dumps under KAI_TRACE_PARK. The slot
counter is atomic and the on-flag is a CAS-free idempotent latch; ring
entries are written unsynchronised on purpose, so a torn entry costs one
garbled trace line instead of perturbing the timing under investigation.
── Class B — immutable after init: worker pthread ids ────────────────
── Class C — shared-locked: F2 reactor thread + its wheel/waiter lists ─
F2 moved the reactor onto its own thread. The timer wheel + waiter lists
+ parked_count are now mutated by scheduler threads (kai_sched_commit_park
links a park, kai_reactor_detach_fiber unlinks a cancel) AND the reactor
thread (the drains), all under kai_reactor_mu — so they are shared-locked,
not single-owner. kai_reactor_mu is that guard; kai_reactor_thread is the
pthread id, written once at bootstrap.
_Atomic flag the reactor thread sets under kai_reactor_mu when it blocks in
poll() with nothing armed and clears the instant poll() returns; the
deadlock check reads it to tell a quiescent reactor from one mid-drain.
The reactor thread is the sole owner of the ready-batch: it collects
fibers a drain readied (under kai_reactor_mu) and unparks them after
releasing the lock. No other thread touches it.
── Class C — reactor-owned: init-once fds/flags + the file thread pool ─
Native emit pool (kaic2 only): the walk and the emit workers share these
under kai_emit_mu.
Interned effect labels: the table is mutated only under kai_eff_mu; entries are immutable once published.
