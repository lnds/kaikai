# Lane experience — issue #1207 F2: reactor serving N schedulers

## Scope

**As planned:** extract the I/O reactor from the scheduler thread onto its
own dedicated thread serving N schedulers, with ready fibers handed back to
their home thread (design §4 step 2). F0 left the reactor inline on thread
0, drained only when that thread went idle; F1 added the N work-stealing
schedulers. F2's job is the boundary F0 isolated.

**As shipped:** the *infrastructure* landed (deterministic I/O+CPU fixture,
golden, and a binary no-starvation bench that fails on F1 by design). The
*runtime* did **not** ship: the reactor extraction has a residual liveness
deadlock (~50% at `KAI_THREADS≥2` under a multi-sleeper workload) that a
session without a working debugger could not close. The full diagnosis,
design, the five races fixed, and the repro are in #1230; the runtime diff
is preserved as a reference patch for the follow-up session.

This is the first F-phase of the M:N arc that did not land its runtime.
The honest read: F2 is a *harder* concurrency problem than F0/F1, and the
tooling gap (below) made the last mile unclosable here.

## The gap F2 closes, measured

Under F1 the reactor's `poll()` runs inline on a scheduler thread, so that
thread cannot run CPU work while it waits for I/O. Measured with 8 CPU hogs
pinning 4 threads + one concurrent 20 ms sleeper: the sleeper's elapsed is
**~363 ms** (≈18× its request) — the poll starves the CPU. This is the
whole point of F2: move the poll to its own thread so I/O and CPU progress
together. `tools/run-mn-reactor-bench.sh` is the binary gate — it fails on
F1 (363 ms > 80 ms) and should pass under F2.

The gate discriminates the *right* direction. An earlier version of the
fixture measured sleeper starvation (does the sleeper wake late?) — that
does NOT reproduce, because between fibers thread 0 returns to its loop and
drains the reactor often enough. The measurable failure is the *inverse*:
CPU throughput collapses because the poll blocks the compute. Building the
fixture that fails for the right reason took two iterations.

## Why the runtime is hard: F1 serialized a family of races implicitly

The core structural surprise: **F1's inline reactor was serializing a whole
family of latent races that only become real once the reactor is on its own
thread.** In F1, the reactor drained on thread 0 only when idle, so any
window between "mark a fiber PARKED" and "link it into the reactor wheel"
was almost never hit — thread 0 was not draining concurrently with the park
site. A dedicated reactor thread drains *constantly*, so every such window
fires on every run.

Five distinct races surfaced and were fixed (hang ~90% → ~50%), each a
different instance of "publish or link a shared reactor/scheduler structure
without full mutual exclusion against a concurrent wake":

1. **commit_park applied to the wrong fiber.** The gopark yields to the
   root and the loop links the fiber post-swap — but keying the commit off
   the loop's `next` local is wrong: a direct fiber→fiber switch can restore
   the root with `next` pointing at a different fiber than the one that just
   goparked. Fix: a per-thread pending-commit *stack* (`commit_next`); a
   single pointer is overwritten when two goparks interpose.
2. **`kai_sched_yield` published the fiber as stealable before its
   swapcontext wrote its ctx.** A thief resumes a half-written `ucontext_t`
   — a real memory race on the saved SP/registers, **invisible to TSAN**
   because `swapcontext`/`ucontext_t` is opaque libc. This is the one to
   remember: TSAN is not a safety net for the swap machinery itself.
3. **`kai_nursery_join_child` check-then-link race** against the child's
   trampoline tail: the parent reads `child->state` (not DONE), the child
   terminates on another thread and drains `awaiters_head=NULL` before the
   parent links, and the parent parks forever. The nursery-await analog of
   the mailbox `wake_pending` race — never propagated to this path.
4. **`kai_reactor_detach_fiber` walked the timer wheel + `parked_count`
   without `reactor_mu`.**
5. **The commit_park window** between releasing `s->mu` (PARKED) and taking
   `reactor_mu` (wheel link): a `remote_unpark` in that gap readies the
   fiber stealable, then commit_park links it into the wheel anyway — a
   phantom wheel entry, the `commits>tdrain` count desync. Fix: fuse both
   critical sections under one lock, order `reactor_mu → s->mu` (audited to
   not nest against `remote_unpark`/drains/detach in the opposite order).

## The residual, and why it stayed open

After all five, the hang is still ~50%, same signature: **`commits >
tdrain`** — timers linked into the wheel that the reactor never drains, so
it computes `timeout=-1` and blocks in `poll()` forever on phantoms. It is
another instance of the same family. The right close is a **systematic
audit** of every access to the shared reactor/scheduler structures
(`reactor_next`, the wheel head, `awaiters_head`, the steal list,
`parked_count`) against a single covering lock — not another one-off race
hunt. That is follow-up work with a debugger, not more instrumentation.

## Tooling gap — the load-bearing reason F2 didn't land here

Three debuggers failed in this environment, and that is why the last mile
was unclosable:

- **`lldb -p` and `sample`** could not attach to the hung process
  (SIP/signing), so the deadlock's actual thread state was never captured.
- **TSAN** reports **zero** races — it is a pure liveness deadlock plus the
  ucontext race it structurally cannot see. TSAN clean is not evidence of
  correctness here.
- **ASAN** reports nothing (it is a deadlock, not memory corruption).

The entire diagnosis came from `KAI_REACTOR_DEBUG`/`KAI_SPIN` counter
instrumentation (commit/drain/unpark tallies). That was enough to *localize*
the family of races and fix five of them, but not to see the sixth in the
act. A session with a working debugger — or the trylock-canary + ASAN pass
suggested during review, on a machine where they attach — closes it.

The takeaway for the arc: **M:N reactor work needs a machine where a
debugger attaches.** F0/F1 were closable by stdout-diff + TSAN because their
bugs were data races and byte-parity divergences; F2's are liveness
deadlocks in swap/park machinery, a class where a counter trace is a poor
substitute for a stack.

## Design collaboration

The design is not the bottleneck — it is sound and was validated
end-to-end. The language architect consulted repeatedly produced a
consolidated one-pass design (reactor thread + `reactor_mu` + gopark
commit + handback, with a three-level disjoint lock order); a critical
reviewer found bugs 2–5 by inspection where instrumentation alone had
stalled. The pattern that worked: instrument to localize, hand the reviewer
the *exact* failing signature (not "it hangs"), get the mechanism, apply,
re-measure. Each cycle closed a real bug. The residual is the one where the
signature (`commits>tdrain` with no captured stack) was not sharp enough for
inspection to finish the job.

## Gates

- **N=1 byte-identical:** held throughout — all F2 code is under
  `nthreads>1`, so the single-thread path is unchanged. This never
  regressed across any of the five fixes.
- **bench mixed I/O+CPU without starvation:** `run-mn-reactor-bench.sh`
  written and confirmed to fail on F1 (the gap it measures) — the binary
  gate for the follow-up.
- **TSAN clean / determinism N=1 vs N=4 / rb-tree intact:** not reachable
  because the runtime does not run reliably at N>1 yet.

F3 was correctly NOT attempted (owner decision, its design pinned:
`min(effective cores, cgroup quota)` + `num_cpus()` in stdlib).

## Docs note

**F1 shipped without updating `kai info fibers`** to reflect the M:N
parallelism it delivered — the integrator covered that gap after the fact.
Recorded here so the F2/F3 follow-up updates the info page *in the same
lane* as the runtime, not as a separate cleanup: when the reactor lands,
`kai info fibers` (and `kai info actors`) should state that fibers run in
true parallel across scheduler threads with I/O demultiplexed on a
dedicated reactor thread.

## Follow-ups

- **#1230** — the residual liveness deadlock, with the full design, the
  five fixes, the repro, and the reference patch. The next session should
  start from a systematic lock-coverage audit with a working debugger.
- Wire `mn_reactor_io_cpu_mix.kai` into the TSAN + determinism tiers *when
  the F2 runtime lands* — not before (it exercises the reactor waiter lists
  F1 mutates without a lock, so it hangs under TSAN on F1).
- Update `kai info fibers` / `kai info actors` with the parallelism story
  (see Docs note).
