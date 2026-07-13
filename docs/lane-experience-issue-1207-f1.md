# Lane experience — issue #1207 F1: M:N work-stealing scheduler

## Scope

F1 is the second phase of the M:N scheduler (#1207, design in
`docs/mn-scheduler-design.md`, foundation in F0 / PR #1210). F0 partitioned
the runtime state into TLS / immutable / shared-locked / reactor-owned and
gated that partition statically. F1 turns parallelism **on**: per-thread
work-stealing deques, copy-on-cross-thread-send, an atomic cancel flag,
link/monitor delivery that is safe across threads, and a `KAI_THREADS=N`
opt-in whose default (`N=1`) is byte-identical to the pre-M:N runtime.

- **Planned:** work-stealing scheduler (Chase-Lev granularity = fiber,
  actors migrate), copy-on-cross-thread-send, cancel flag → atomic,
  link/monitor as messages, `KAI_THREADS=N` with `N=1` byte-identical,
  TSAN CI tier, ≥3× speedup on 4 cores measured.
- **Shipped:** all of the above, with one deliberate mechanism swap
  (mutex-guarded deque instead of lock-free Chase-Lev — see below) and one
  residual (a TSAN-only shutdown hang under investigation, §Residual).

## The central trick, unchanged from the design

kaikai already had a shared-nothing, per-fiber-heap, non-atomic-RC actor
runtime on one OS thread. F1 keeps the non-atomic RC — the load-bearing
non-negotiable — because the **cross-thread boundary gets a physical
copy**. `kai_core_mailbox_send` now checks whether the destination fiber's
`home_thread` differs from the sender's; if so it `kai_deep_copy_out`s the
message into the *sender's* heap (rc=1, single-owner) and enqueues the copy
under the mailbox lock. The receiver adopts and later frees it on its own
thread, so no fiber-local heap object is ever touched by two threads. Same
thread → the pointer transfer of issue #82, unchanged. At `N=1` the copy
branch is never taken, so the send path is byte-identical to before.

`kai_deep_copy_out` already existed (region copy-out); it allocates through
the running thread's TLS pools, which is exactly right — the sender copies
into its own pools, the tree migrates like a fiber.

## The architecture: one runtime, two frontends

A pleasant surprise: `stage0/runtime_llvm.c` `#include`s `stage2/runtime.h`
and only exposes `kaix_*` forwarders that delegate to `kai_*`. There is no
duplicated scheduler / mailbox / pool logic between the C and native
backends — one implementation in `runtime.h`, two link frontends. Every F1
change lives in `runtime.h`; the native path inherited it for free. This
collapsed what the brief warned would be "two runtimes to keep coherent"
into a single edit surface.

## The cooperative-switch constraint drove every scheduler decision

The runtime is cooperative over `ucontext` `swapcontext` with **no
scheduler run-loop**: `kai_main_fiber` (TLS, one per OS thread) is the
OS-thread context itself, and switches go fiber→fiber directly. Under M:N
that raised the load-bearing question a language-architect review (asu)
settled before a line was written:

- **Switch stays direct fiber→fiber while local work exists** (Go's
  `gogo`, not a trip through a loop); a fiber falls back to its thread's
  `kai_main_fiber` scheduler loop only when its local deque drains. That
  loop is where stealing and I/O happen.
- **Nothing that can block the OS thread (poll, a wait) runs on a user
  fiber's stack.** `kai_reactor_wait` (poll) therefore runs on the owner
  thread's `kai_main_fiber`, never inline on a parked fiber's stack — a
  worker whose deque drained steals or idles, it never polls.

So `kai_sched_park` at `N>1` switches directly to a local successor if one
exists, else swaps back to `kai_main_fiber`, which runs `kai_worker_loop`:
find work locally, steal round-robin from other threads, and (owner only)
drive the reactor. The main thread's `kai_main_fiber` *is* that loop;
`kai_main` runs as a spawned fiber via `kai_sched_bootstrap`, so at `N=1`
bootstrap is exactly `kai_main()` — no threads, byte-identical.

## Mechanism swap: mutex-guarded deque, not lock-free Chase-Lev

The design §2 specified Chase-Lev lock-free deques. F1 ships a **mutex-per-
slot deque** instead, and the swap is justified by the design's own §3
reasoning: steals are rare (only when a thread's own deque is empty), so a
mutex on the steal path is cheap and removes the entire ABA / memory-
reclamation subtlety of Chase-Lev — which matters for passing TSAN clean on
the first pass. At `N=1` the mutex is never taken (`kai_nthreads > 1`
guards every cross-thread branch). This is the same "the cost is the copy,
not the lock" argument the design made for mailboxes, applied to the deque.

## What went atomic, and what did not

Exactly one per-fiber field: `cancel_requested` → `_Atomic int`. A
cross-thread `Spawn.cancel` writes it; the target reads it at its yield
points. Everything else routes through the mailbox (the sanctioned cross-
thread channel) or a slot lock. The **object-RC path stays atomic-free** —
that is non-negotiable #1, and the rb-tree canary proves it held (below).

## The slab free-path (design risk 1), resolved

The design flagged that a variant block allocated on thread A's slab can
migrate (with a message copy) into thread B's pool and be reused there. The
audit confirmed: `KaiValue` cells go to `calloc` and a size-agnostic cell
pool (free-to-local, sound); only variant var-blocks live slab-interior.
The fix is minimal: **at `N>1` the per-thread slab teardown atexit is not
registered** — the OS reclaims all slabs at process exit, so no thread pulls
a slab out from under a block another thread still holds. The teardown's
only purpose was a clean ASAN "0 still-reachable" at `N=1`, which is
unchanged.

## Gates

Measured on this machine (14 physical cores, macOS arm64):

1. **Speedup (the deliverable):** `demos/parallel_actors` (8 CPU-bound
   worker actors). On the **native default backend**: N=1 **1.06s**, N=4
   **0.29s → 3.65×**, N=8 **0.16s → 6.6×**. On the C backend: N=4 3.4×.
   Exceeds the ≥3×-on-4-cores gate on both backends. (3 runs each, medians.)
2. **Determinism:** the deterministic total (7200000024 for the bench,
   1000 for the copy stress) is byte-identical at N=1, N=4, N=8. 300
   consecutive N=4 runs of the cross-thread copy stress: 0 wrong, 0 hangs.
3. **N=1 byte-identical:** spawn/actor/link/monitor/trap fixtures produce
   identical output at `KAI_THREADS=1` (default) vs the pre-lane runtime.
   Selfhost byte-id green (`kaic2b.c == kaic2c.c`) — the runtime change did
   not perturb the path that compiles the compiler.
4. **rb-tree RC unregressed (non-atomic-RC canary):** C median **0.36s**
   vs F0's documented 0.38s baseline — noise. The RC hot path paid nothing;
   the atomic cancel flag is nowhere near it, and every M:N branch is
   `kai_nthreads > 1`-gated.
5. **Static global audit:** 155 globals, all classified; `--self-test`
   confirms an unclassified global breaks it. Every new M:N global
   (`kai_nthreads`, `kai_thread_id`, `kai_sched_slots`, the registry
   counters, `home_thread`, `wake_pending`) is classified.
6. **TSAN:** cross-thread copy stress at N=4 and N=8 under
   `-fsanitize=thread` reports **zero data races** and terminates cleanly
   (N=4 ×8 runs + N=8 ×4 runs). Wired as `test-mn-tsan` into a dedicated
   `tier1-tsan.yml` CI tier, off the tier1 light path.

## Two frontends, one entry point (the native gap)

The C backend's `int main` is emitted by `emit_main_wrapper`
(`emit_c.kai`); the **native backend has no `int main`** — `stage0/
runtime_llvm.c` owns it and calls `kai_main()` directly. So routing the
program through `kai_sched_bootstrap` took *two* edits: the emitter for C,
and `runtime_llvm.c`'s `main` for native. Missing the second showed as a
native binary that ran correct but at 1× (no threads started) — a reminder
that "verify on the default backend" is not optional: the C-backend
speedup would have shipped a native no-op.

## Cross-thread copy stress fixture

`examples/effects/mn_cross_thread_copy_stress.kai` sends heap-carrying
messages (lists) across threads — an Int message stays unboxed and skips
the copy path, so the fixture uses lists to exercise `kai_deep_copy_out`
across the boundary for real. Deterministic (total 1000) so it doubles as
the determinism and TSAN fixture.

## The TSAN-only shutdown deadlock — found and fixed

The scariest bug of the lane, because it was invisible without TSAN: at
`N≥2` under `-fsanitize=thread`, the stress printed the correct result and
TSAN reported zero races, but the last worker sometimes never exited
`kai_worker_loop`, so `pthread_join` hung. 300/300 uninstrumented N=4 runs
terminated cleanly in ~0.02s — it was purely a TSAN-scheduling artifact.

Two dead ends and the root cause:

- **Dead end 1 — the idle condvar.** The first idle wait was a
  `pthread_cond_timedwait(50ms)`. Under TSAN on macOS its timeout proved
  unreliable — a worker that missed the shutdown broadcast slept forever.
  Replaced with a plain `nanosleep(200µs)` poll: shutdown is observed at
  the top of the loop within one tick, no wakeup to lose. (A condvar-driven
  idle is a later energy refinement.)
- **Root cause — a dead lock, literally.** `kai_sched_global_mu` guarded
  only `kai_sched_runnable_count`, a counter that **nothing read** (left
  over from an earlier worker-loop design). Every deque op took `slot->mu`,
  released it, then took `global_mu` to bump the counter — sequential at
  each site, never nested. But that dead lock created a latent ordering
  hazard TSAN's aggressive scheduling surfaced as a hang. Deleting
  `global_mu` and `runnable_count` entirely (and making the shutdown flag a
  lock-free `_Atomic int`) fixed it: N=4 ×8 + N=8 ×4 under TSAN now
  terminate in <1s with zero races. The lesson: a lock guarding
  never-read state is not free — remove dead synchronization, don't keep
  it "just in case."

### The race macOS TSAN missed and Linux CI caught

The first CI run of `tier1-tsan.yml` (Linux) reported a real data race the
macOS TSAN runs never hit: `KaiSchedSlot.live` was a plain `int`, written
by a worker at startup (`slots[me].live = 1`) and read by every thief in
`kai_sched_steal_from` (`if (!s->live) return`) **outside the slot lock**
(the thief reads `live` to decide *whether* to lock). macOS TSAN's
scheduling never interleaved the startup write against a concurrent steal;
Linux's did on the first run. `live` is the one piece of slot metadata read
lock-free, so it becomes `_Atomic int` — the store publishes and every
thief acquire-loads it. Only the *deque metadata* went atomic, never the
object RC: `live` is off the RC hot path (read once per steal attempt, and
never at all at N=1 where no steal runs), so the rb-tree canary is
unaffected. Validated by running the TSAN stress in a 20-iteration loop
(15× N=4 + 5× N=8) with zero races — the interleaving that macOS missed
needs repetition to surface. **The gate paid for itself on day one.**

## Follow-ups (F2 and beyond)

- **F2 — multi-scheduler reactor:** the reactor is fixed on thread 0 for
  F1; F2 shards the ready-handback to the owning `home_thread`. The
  `KAI_MN_DEBUG`-gated tracing added here is the tool for that lift.
- **Condvar-driven idle** as an energy refinement once the TSAN-safe
  wakeup mechanism is settled (the nanosleep poll is correct but spins).
- **`__tsan_switch_to_fiber` annotations** around `swapcontext` /
  `makecontext`. The runtime switches fibers with no TSAN fiber
  annotations; TSAN's happens-before tracking cannot follow a stack that
  migrates across `swapcontext`, so once `tier1-tsan.yml` runs on Linux
  it may report false positives on fiber-crossing accesses. Annotating the
  three switch sites (`kai_sched_yield`, `kai_sched_park`, the trampoline)
  makes the fiber model legible to TSAN. Not needed for the macOS run
  (which is clean post-fix) but likely for the Linux CI tier.
- **Cross-thread link teardown** as a peer-owned system message (design
  §5); F1 makes the *propagation signal* safe (atomic cancel flag + copied
  mailbox delivery) but leaves the far-chain edit unsynchronized.
- **Default flip to `N=ncpu`** is its own PR after the TSAN gate closes and
  a soak — trivially revertible via `KAI_THREADS=1`.
