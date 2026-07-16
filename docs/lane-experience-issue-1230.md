# Lane experience — issue #1230: F2 dedicated reactor thread

Part of #1207. Extract the I/O reactor from the scheduler threads onto its own
dedicated thread serving N schedulers, with ready fibers handed back to their
home thread. Close the intermittent liveness deadlock the previous session left
open.

> This supersedes the earlier retro for the blocked attempt. That session built
> the Linux debug container + driver (`docker/mn-debug.Dockerfile`,
> `tools/mn-debug.sh`, landed in the infra PR) and reimplemented F2 with an
> alternative MPSC architecture that did not land, blocked by the work-stealing
> corruption since fixed as #1234. This lane starts clean from the F1 baseline
> (with #1234 merged) and closes #1230.

## Scope as planned vs as shipped

Planned: implement F2 from the F1 baseline per the six-edit design, reproduce
the ~50% deadlock, capture it with rr, fix the residual, pass the gates.

Shipped: the six-edit reactor extraction, **plus** three scheduler lost-wakeup
fixes the reactor extraction exposed, `_Atomic` fiber state, a recalibrated
starvation bench, and the reactor determinism fixture wired into the M:N tier.
The residual was not closed by rr — it was closed by a systematic lock-order
audit, which is what the issue itself predicted ("the right close is a
systematic audit, not another one-off race hunt").

## What F2 actually looks like

- Reactor state (timer wheel + waiter lists + `parked_count`) is guarded by a
  new `kai_reactor_mu`. A dedicated reactor thread owns the `poll()` loop; it
  drains ready fibers into a private batch under the lock, then `remote_unpark`s
  them after releasing it (never nests reactor_mu → a slot lock).
- Reactor park sites stamp a `pending_park` reason and yield UNCONDITIONALLY to
  the scheduler root. The root, running on `kai_main_fiber` after the parking
  fiber's exit swap has finished saving its ctx, links the fiber into the wheel
  from `kai_sched_commit_park` (reached via a per-thread commit stack drained in
  `kai_worker_loop`). Linking after the swap is what stops a reactor drain from
  resuming a half-written `ucontext_t`.
- `kai_sched_commit_park` sets state + links the waiter + pokes the reactor's
  self-pipe. **The self-pipe poke is the residual-deadlock fix**: a timer linked
  while the reactor already slept in `poll(-1)` was never drained; waking it
  forces a timeout recompute.
- N=1 is byte-identical: every new path is gated on `nthreads>1`, the lock
  helpers are no-ops, the reactor thread is never started, and the inline F1
  reactor keeps its lock-free path. selfhost byte-id holds (runtime changes
  don't touch codegen).

## The residual was not one bug — it was a family of lost-wakeups

By construction the clean design avoided the issue's stated signature
(`commits > tdrain` phantom timer). But heavy stress (hundreds of runs) kept
surfacing a ~0.5% hang. Each turned out to be a *different* lost-wakeup that F1
serialized implicitly and the reactor thread's timing exposed:

1. **await/nursery/select check-then-link.** A parent reads `child->state`
   (not terminal) then links onto `child->awaiters_head`, racing the child's
   terminate walk that snapshots the chain. Fix: serialize both under the
   child's slot lock.
2. **park self-dequeue orphan.** `kai_sched_park` marks PARKED, unlocks, then
   `dequeue`s. A concurrent `remote_unpark` in that window enqueues `current`
   itself → `dequeue` returns `current` → the old code swapped to root,
   stranding a READY fiber off every queue. Fix: `next == current` means
   "re-readied", resume instead of parking.
3. **home_thread migration during the await lock.** The #1 fix locked
   `slots[child->home_thread]`, but a READY child can be **stolen** (home_thread
   changes) between the awaiter reading it and the trampoline locking it — the
   two sides then take *different* slot locks and the race reopens. Fix:
   validate-under-lock (`kai_fiber_slot_lock` re-reads home_thread after
   acquiring and retries; once held, the slot lock itself blocks migration).

Also fixed for completeness: the yield publish-before-swap ucontext race
(`kai_sched_yield` now defers the requeue to the root post-swap, like a park).

`_Atomic` fiber `state` closes the last class: `state` transitions under five
different lock contexts (parker's slot, target's slot, reactor_mu, child's slot,
and locklessly), so no single lock covers it — atomic is the honest fix, and it
made TSAN clean.

## Which tool captured the deadlock — and which didn't

The brief pointed at rr reverse-continue + helgrind. Reality on prodigy:

- **helgrind/drd:** valgrind is not installed and there's no passwordless sudo.
  Unavailable.
- **rr (`record --chaos`):** could NOT reproduce the hang in 500+ recorded runs.
  rr serializes onto one core; the race is a fine cross-thread window that
  single-core execution closes. rr is the wrong tool for a *timing-window*
  lost-wakeup (it is excellent for a deterministic one).
- **gdb attach:** blocked by `yama ptrace_scope=1`. gdb-as-parent works and did
  capture the one crash that was reproducible (the TSAN teardown crash).
- **Instrumentation (fiber registry + watchdog):** perturbed the timing enough
  to HIDE the race entirely — 500 runs, zero reproductions with the watchdog on,
  even after reducing the per-spawn cost to a single flag check. A Heisenbug in
  the strict sense.

What actually closed it: a **systematic lock-discipline audit** of every
shared-state access (state, awaiters_head, the steal deques, home_thread,
parked_count) against every concurrent writer, reasoning out the exact
interleaving for each — then **TSAN (ASLR disabled via `setarch -R`) confirmed
zero data races** once state was atomic. The reusable lesson: for a fine
cross-thread lost-wakeup, the audit beats the debugger, and the debugger that
finally confirms it is the sanitizer, not the tracer.

## The bench premise was unsound (surfaced to the owner)

`tools/run-mn-reactor-bench.sh` spawned 8 CPU hogs on 4 threads and expected the
sleeper to wake at ~20ms under F2. On a **non-preemptive** cooperative scheduler
that is physically impossible: with hogs ≥ threads there is no free thread to
dispatch the reactor-woken sleeper, so F1 and F2 measure the same full CPU burst
(~1s on prodigy) — the metric cannot distinguish them. The "~20ms under F2" was
an untested hypothesis (F2 didn't exist when the bench landed).

Recalibrated to `hogs = threads - 1` (one free thread for I/O dispatch, default
capped to `min(4, nproc)` so CI runners don't oversubscribe). Now a valid gate:
measured F1 = ~1045ms (reactor pinned to thread 0), F2 = 20ms (reactor drains on
its own thread, free thread runs the sleeper). This is the property F2 truly
fixes: I/O draining independent of *which* scheduler threads are busy.

## Fixtures + gates

- `examples/effects/mn_reactor_io_cpu_mix.kai` (already landed) wired into
  `tools/run-mn-determinism.sh` (tier1): total = 12 at N=1/N=4/N=8, and the
  shutdown-hang check catches the deadlock class this lane closed.
- `tools/run-mn-reactor-bench.sh` recalibrated + wired into the M:N tier
  (`tier1-tsan.yml`, path-gated) via `make test-mn-reactor-bench`.
- Stress: 1300+ runs of the issue repro + the mix fixture across N=2/3/4, zero
  hangs after the three scheduler fixes. TSAN: official copy-stress gate clean
  (0 races); reactor fixtures show 0 races but crash in TSAN's own teardown
  stack-unwind on `makecontext` stacks (a TSAN/ucontext limitation, not a bug —
  the fixtures run clean thousands of times without TSAN).
- selfhost byte-id, demos-no-regression, arena/heap/evidence gates, runtime
  global audit: all green (7 new globals classified; the reactor wheel + waiter
  lists moved reactor-owned → shared-locked now that scheduler threads mutate
  them under `kai_reactor_mu`).

## Rebase onto #1238 (v0.99.20)

The lane branched from v0.99.19, before #1238 landed. #1238's C-backend
owner-object split flips the scheduler TLS from static (local-exec) to extern
(general-dynamic under -fPIC → `__tls_get_addr`), which TSAN mis-models as a
shared access — the tier1-tsan red on this branch (2 races through
`kai_active_fiber_anchor`/`kai_current_fiber`) was exactly that false positive.
#1238 pins `tls_model("initial-exec")` on the scheduler TLS to fix it. Rebasing
inherited the fix cleanly (no conflicts; the new commit-stack/requeue-stack TLS
use the `KAI_TLS` macro, which now carries initial-exec, and every internal
scheduler function this lane added is reachable only through a `KAI_SCHED_FN`
entry point, so DCE keeps swapcontext out of the -O2 program object — the nm
assert passes). Post-rebase: **copy-stress TSAN 0 races (was 2)**, determinism +
bench + 150-run deadlock stress all green.

#1238 also independently fixed the runtime-cache-key bug this lane tripped over
(the cached owner `.o` now hashes `RUNTIME_INC_C/runtime.h` = stage2, so a
stage2/runtime.h edit invalidates it) — no longer a follow-up.

## Follow-ups left for next lanes

- **TSAN + fibers.** Reactor fixtures can't run under TSAN because it unwinds
  `makecontext` stacks during malloc and crashes in its own runtime. Proper
  support needs `__tsan_switch_to_fiber`/`__tsan_create_fiber` annotations at
  every swapcontext — a separable lane that would let the reactor path join the
  TSAN gate.
- **Bench timing sensitivity.** The starvation gate is wall-clock; on a heavily
  loaded shared runner even the recalibrated bench could spike past the 4×
  ceiling. If it proves flaky, tune the ceiling or move it to a self-hosted
  runner. The determinism fixture is the robust regression gate; the bench is
  the throughput witness.
- **Idle energy.** Scheduler threads still nanosleep-poll at 200µs; a condvar
  idle is the pre-existing F1 follow-up, untouched here.
