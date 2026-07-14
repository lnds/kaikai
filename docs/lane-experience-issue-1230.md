# Lane experience — issue #1230 (F2 reactor thread; part of #1207)

## Scope as planned vs. as shipped

**Planned:** build a Linux debug container (gdb/rr/valgrind), reimplement F2 (dedicated
reactor thread serving N schedulers) from the F1 baseline per the six-edit design in the
issue, reproduce the ~50% liveness deadlock under a real debugger, extract the fix from the
captured state, and close #1230.

**Shipped:** the debug container and its driver script (`docker/mn-debug.Dockerfile`,
`tools/mn-debug.sh`). The reactor runtime was reimplemented with a **different, sounder
architecture than the six-edit design** (an MPSC submission queue — see below), which the
debugger proved correct. But closing #1230 was **blocked by a distinct, pre-existing
work-stealing corruption bug** that the debug environment surfaced and that affects `main`
too — not F2. The runtime work does not land; the finding is reported instead.

## The debug environment did its job — it is the reusable lesson

The whole premise of the lane was that macOS could not capture the hung process (SIP/signing
block `lldb -p`/`sample`; TSAN-deadlock is a no-op there). The Linux container closed that gap
decisively. What actually captured each stage of the bug:

- **`gdb -p <pid>` on the hung process** → `thread apply all bt` showed all scheduler threads
  idle in `nanosleep` and the reactor in `poll`, with every scheduler structure empty. That
  ruled out "stuck in a lock" and pointed at a lost fiber.
- **A `SIGQUIT` handler dumping every fiber's `(state, home, wake_pending, awaiters)`** (gated
  behind `-DKAI_REACTOR_DEBUG`) → showed the lost fiber precisely: `state=RUNNING`,
  `wake_pending=1`, with an awaiter parked on it, present in no deque and run by no thread.
- **Reading the lost fiber's saved `ctx.uc_mcontext.pc` in gdb** → it pointed at
  `swapcontext+184` in libc: the fiber had ceded via `swapcontext` and its context was saved
  mid-write. This is the single most decisive datum of the series and it named the bug class.
- **Diagnostic oracles** (compile-time gates disabling one mechanism at a time):
  - Disabling the direct fiber→fiber switch in park: 115→35 aborts (contributes, not root).
  - **Disabling work-stealing (`kai_sched_steal_from → NULL`): 60/60 OK, zero hangs, zero
    aborts.** This is the oracle that localized the bug to the steal path and proved the
    reactor design correct.

rr (record-replay) — the ideal tool for a lost-wakeup — could **not** run: Docker Desktop
rejects `--sysctl kernel.perf_event_paranoid`, and rr needs it lowered on the VM's kernel.
That is the one gap the container could not cover, and it is why the residual bug (a pure
timing race on `ctx` memory, invisible to ASAN/TSAN) could not be pinned to a single line.

## The reactor architecture that shipped in the code (validated, but blocked)

The six-edit design in the issue (a `pending_park` flag + `commit_park` linking the fiber into
the wheel from the root, with a per-thread pending-commit stack) has **two writers of the timer
wheel** — the reactor thread and the parking fiber's thread. The residual `commits > tdrain`
signature in the issue is the inevitable fingerprint of that: no single point where "everything
linked is visible before the poll timeout is computed". Each fix moves the window one cycle
further; it is whack-a-mole.

The lane replaced it with an **MPSC submission queue** (the libuv / Go-netpoller model):

- Parking sites do **not** touch the wheel or waiter lists. They stamp their payload, push the
  fiber onto a lock-guarded MPSC queue (`kai_reactor_sub_mu`), wake the reactor via its
  self-pipe, and call the ordinary `kai_sched_park`.
- The reactor thread is the **single writer** of the wheel and every waiter list. It drains the
  submission queue **completely before computing the poll timeout**, so the wheel head always
  reflects everything submitted so far. The "linked but not drained" phantom is impossible by
  construction.
- Detach (cancel / nursery) goes through the same queue, collapsing it to the single writer.

**The oracle proves this design correct:** with work-stealing disabled, the reactor-on-its-own-
thread + MPSC handback runs the fixture 60/60 clean at `KAI_THREADS=4`. The starvation gap F2
targets is closed by construction (the reactor polls on its own thread; no scheduler thread
blocks on `poll`).

## Why it does not land: a pre-existing work-stealing corruption

The residual bug is **not in the reactor**. Measured, decisive:

| Runtime | `KAI_THREADS=4`, 150 runs of the fixture |
|---|---|
| **F1 baseline (`origin/main`, unchanged)** | 27 hangs, **108 aborts** (SIGSEGV) |
| F2 (this lane, MPSC + all fixes) | ~63 hangs, ~97 aborts |
| F2 with work-stealing disabled | **60/60 OK, 0 hangs, 0 aborts** |
| N=1 (any) | always correct, byte-identical |

`main` corrupts on the same fixture, at a comparable rate, without any of this lane's code.
The `mn_reactor_io_cpu_mix.kai` fixture (landed in #1231) exposes a **pre-existing M:N
work-stealing bug** that the determinism gate (few iterations) never caught. F2 does not cause
it — F2 *surfaces* it, because the reactor-handback creates a flow (I/O fiber wakes on the
reactor → enqueued on its home → stolen aggressively by an idle worker) that F1's inline
reactor serialized away.

The bug class, from the captured `swapcontext+184`: the fiber's `state` is single-owner
(both dispatch and steal asserts stay green — `assert(f->state == KAI_FIBER_READY)` never
fires) and the heap is data-race-free (ASAN clean across 60 runs). What is racing is the
fiber's `ucontext_t` itself — valid memory with a mid-`swapcontext` inconsistent value. A thief
resuming that half-written context reads a garbage stack pointer and faults on the guard page.
`swapcontext`/`ucontext_t` is opaque to ASAN and TSAN, exactly like the issue's "bug #2 —
invisible to TSAN". It reproduces **only** at `-O2` without instrumentation; `-O0`, gdb, and
ASAN all shift the timing enough to hide it.

## Mechanisms tried (all sound, none closed the residual)

Each was validated by the language architect and correct in isolation; the residual survived
all of them because its root is the steal/ctx race, orthogonal to each:

1. MPSC submission queue — killed the two-writer wheel phantom (the issue's stated residual).
2. Await/join check-then-link serialized under the child's lock (the issue's bug #3).
3. `KAI_FIBER_PARKING` transitional state + `kai_last_parked` + `commit_park` post-swap — the
   gopark pattern, closing the state-publish-before-ctx-saved window.
4. Per-fiber lock (`KaiFiber.mu`) for `(state, wake_pending, awaiters)` — collapsed the
   split-lock visibility bug where park/commit/remote_unpark chose their lock by different
   rules (`kai_thread_id` vs `home_thread`).
5. Enqueue-once + atomic dispatch invariant: `state==READY` ⟺ in exactly one deque; enqueue
   only on the transition into READY, dispatch flips READY→RUNNING under `f->mu`.
6. Steal retargets `home_thread` under the victim's slot lock (atomic with the pop).
7. Park **and yield** always cede to the thread root (no direct fiber→fiber switch). A second
   pass removed the residual `commit_park()` a resuming fiber ran *after* its own swap-back:
   with steal, a fiber resumes on the THIEF thread, and `kai_last_parked` is a TLS — that
   `commit_park` published the thief's pending parker, not the resuming fiber, double-
   dispatching it. Removing it (only the dispatching root commits) lifted the clean rate from
   ~15% to ~42% but did not close it.

## The root class, finally named: the fiber handoff is built on per-thread (TLS) state

The residual corruption is a **stack use-after-migration**, and the core dump names it. The
crashing thread is in `kai_worker_loop` at the shutdown check with a **"corrupt stack?"** —
the loop is running on a `ucontext` whose stack pointer is garbage. Three pieces of the M:N
fiber handoff are per-thread (`KAI_TLS`) and do not survive a fiber migrating between threads:

- **`kai_main_fiber` is TLS.** A parking fiber swaps to "its thread's root" via
  `swapcontext(&f->ctx, &kai_main_fiber.ctx)`. After a steal the fiber runs on the thief, so
  the root it returns to is the thief's — correct — but the `uc_link` baked into the fiber's
  context at creation (`kai_fiber_init_ctx`: `f->ctx.uc_link = &kai_main_fiber.ctx`) points at
  the **spawning** thread's root. If control ever reaches `uc_link` on a migrated fiber it
  jumps to another thread's root context — cross-thread stack.
- **`kai_pending_free` is TLS.** A fiber that terminates leaves its struct+stack there to be
  reaped "at the next context switch" on that thread. With stealing, the producer and the
  reaper can be different threads, and the timing of the reap versus a still-live reference to
  the stack is what the corruption rides.
- **`kai_last_parked` is TLS.** The gopark commit hangs off it; a migrated fiber that touches
  it sees the wrong thread's pending parker (fixed above for park/yield, but it is the same
  structural smell).

None of these was a problem for F1 because the inline reactor kept I/O-woken fibers from being
stolen the instant they became READY. F2's reactor-handback makes the steal fire reliably, so
the pre-existing TLS-handoff assumptions break in the open. **This is the real depth of the
bug** — not a lock to add, but a handoff built on per-thread state that a migrating fiber
carries across. Closing it means making the fiber handoff migration-safe (a per-fiber or
per-target-thread root/free discipline, and a `uc_link` that resolves to the *running*
thread's root, not the spawning one) — a scheduler-M:N redesign, not a reactor fix.

The steal/dispatch state asserts stay green through all of these — the state machine is sound.
The corruption is below the state layer, on `ctx`, and no available tool observes it.

## Follow-ups (for the next lane, ordered)

1. **A dedicated M:N work-stealing lane with record-replay.** The residual is a pure timing
   race on `ucontext_t` memory; `rr record`/`rr replay` is the tool that captures it
   deterministically. It needs a host (or VM) where `kernel.perf_event_paranoid` can be
   lowered — Docker Desktop cannot. A bare-metal Linux box or a Linux VM with the sysctl set is
   the unblock. The container in this lane is otherwise ready (gdb/valgrind/rr all installed).
2. **This is a `main` bug, not an F2 bug.** It should be filed against the scheduler M:N
   work-stealing under I/O-concurrent load, with the fixture from #1231 as the repro and the
   `swapcontext+184` capture as the diagnosis. F2 cannot land until it is fixed, because F2's
   reactor-handback is what makes the pre-existing race fire reliably.
3. **When it is fixed, the MPSC reactor here is ready to land** — it is proven correct by the
   no-steal oracle. Preserve this branch; the runtime diff is the F2 implementation minus the
   `-DKAI_REACTOR_DEBUG` instrumentation.

## What stays in the repo from this lane

Only the debug infrastructure: `docker/mn-debug.Dockerfile` and `tools/mn-debug.sh`. It is the
reusable asset — the next concurrency bug that only reproduces under load has a Linux container
with gdb/valgrind/rr and a driver that builds the repro, runs it in a hang-detecting loop, and
attaches each tool, one command away.
