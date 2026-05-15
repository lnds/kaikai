# Lane retro — issue #611 Phase R1 reactor (file + sleep + process)

Closed: 2026-05-15. Branch: `issue-611-reactor-r1`. Parent: #474.

## Scope as planned vs. scope as shipped

**Planned (issue #611 body):** three surfaces park the fiber instead
of the OS thread — `Spawn.sleep` via a timer wheel,
`File.read_file`/`write_file` via a thread-pool offload, and
`Process.wait` via a SIGCHLD-driven reactor. Wait primitive
sketched as `kqueue` on macOS / `epoll` on Linux with platform
branching; 10 LOC estimate around 500 total; ~0.7–1.0 agent day.
TCP sockets (R2) and stdin (R3) explicitly deferred.

**Shipped:** the three surfaces all park the fiber, exactly as
specified. The wait primitive is `poll()` on two self-pipes
(SIGCHLD + a 4-worker file pool) — see the design-decision note
below — and the three new demos plus `ping_pong` byte-identical
prove the wake path runs. Real total: ~600 LOC added to
`stage0/runtime.h`, three new demos with goldens, three doc
sidebars flipped, this retro. Coverage holds at baseline 31
(was 27 pre-lane; +3 demos here, +1 from a parallel lane).

## Design decisions and alternatives considered

### Wait primitive: `poll()`, not `kqueue` / `epoll`

The issue body sketched the platform-native readiness primitives.
The lane decided to use `poll()` instead.

**Reason:** R1's three surfaces never wait directly on a regular
file descriptor — sleep is timer-driven, process.wait is SIGCHLD-
driven, file ops are thread-pool-offloaded. The only FDs the
reactor watches are two internal self-pipes. `poll()` covers
that case portably in ~10 lines; `kqueue`/`epoll` would add
~100 lines of `#ifdef __APPLE__` / `__linux__` branching for
zero new capability inside R1's scope.

`kqueue`/`epoll` become load-bearing when TCP sockets land in R2:
sockets ARE regular FDs that need readiness notifications, and
the worker count for a kqueue-backed reactor can be 1 (the
scheduler thread itself) rather than 4. The R2 lane should swap
the wait primitive at that point — `kai_reactor_wait` is the
only function that needs to change.

### File I/O: 4 worker threads (Linux epoll-blocker mitigation)

`epoll` on Linux explicitly does NOT notify on regular files —
the kernel always reports them as readable, so a fiber waiting
on `read()` would spin. The portable shape is a thread-pool
offload: hand the blocking `fopen`/`fread`/`fwrite` to a worker
thread and wake the scheduler when it completes.

macOS `kqueue` *does* notify on regular files, but the lane
chose to use the thread-pool path on both platforms for uniformity.
Once R2 lands `kqueue`/`epoll`, the macOS file path can short-
circuit through the readiness reactor; for v1 the cost of a
worker thread per file op is invisible compared to the disk I/O
itself.

The pool is fixed at 4 workers. Anecdotally that covers the
nursery-with-N-writers patterns in the demo and book chapter 17;
a dynamic pool with backpressure is post-MVP.

### SIGCHLD: additive, masked off workers

Three signal-correctness concerns:

1. **Additive install.** The runtime already grabs `SIGSEGV` for
   the stack-guard handler. The reactor's `kai_reactor_init`
   checks `sigaction(SIGCHLD, NULL, &old)` and only installs if
   the slot is `SIG_DFL` / `SIG_IGN`. A user-installed SIGCHLD
   handler in scope panics with a clear diagnostic — the runtime
   does not silently override.

2. **Self-pipe write only inside the signal context.** The
   handler does the minimum allowed: `write(fd, &b, 1)` on the
   non-blocking pipe and an `errno` save/restore. The actual
   `waitpid` drain runs from the scheduler thread on the next
   `kai_reactor_wait`.

3. **Block SIGCHLD in worker threads.** SIGCHLD is process-wide,
   so the kernel can deliver it to any thread whose mask does
   not block it. We need it on the scheduler thread (the one
   doing `poll()`); the workers `pthread_sigmask(SIG_BLOCK, …)`
   it during `pthread_create` and restore the parent's mask
   afterwards. Without this the wake byte could land on a
   worker's pipe-fd, which the scheduler never reads.

### `kai_sched_park` reorder

The original `kai_sched_park` set `current->state = PARKED` AFTER
dequeueing the next fiber. With the reactor in the loop the order
matters: the reactor drain helper calls `kai_sched_unpark(f)`
which early-returns when `f->state != PARKED`. If the dequeue
loops through `kai_reactor_wait` before `current->state` is set,
a wake fires on a still-RUNNING current and the unpark is a no-op
— deadlock.

Fix: set `current->state = PARKED` up front. Added a same-fiber
fast path for the rare case where the reactor wakes the parking
fiber before any other fiber gets a chance to dispatch (so the
swapcontext is skipped on its own stack).

### Per-fiber slots, not a side table

A parked fiber lives on exactly one reactor structure at a time
(timer wheel, pid map, or file-pool list). The fiber struct grew
five slots:

```
KaiFiber *reactor_next;       /* intrusive list link  */
uint64_t  reactor_deadline_ns;
int       reactor_wait_pid;
int       reactor_wait_status;
void     *reactor_data;
```

`awaiters_next` was almost re-usable — it carries the same "fiber
is in one waiter chain" invariant — but mailbox waits and reactor
waits can compose (a fiber asleep inside `Process.wait` would not
be on a mailbox chain, but a future "wait_or_kill"-shaped surface
might want both). Keeping the slots separate costs ~40 bytes per
fiber and removes a footgun.

### Lazy thread-pool init

`kai_reactor_init` runs at the first sleep / process / file op
and installs the SIGCHLD handler + the self-pipes. The 4 worker
threads only spin up at the first *file* op via a separate
`kai_reactor_init_filepool` — pure sleep workloads
(`demos/sleep_concurrent`) never pay the `pthread_create × 4`
cost. Initial measurements showed ~300 ms of pthread overhead on
the first call; the split makes that overhead localised to
file-using programs.

## Fixtures added

Three new demos under `demos/`, each with a `main.out.expected`:

| Demo | Wake source | Wall (cold) | Wall (warm) |
|---|---|---:|---:|
| `demos/sleep_concurrent`         | 10 timers in the wheel    | ~400 ms | ~100 ms |
| `demos/file_concurrent`          | 4 file-pool offloads      | ~300 ms | ~10 ms  |
| `demos/process_wait_concurrent`  | 3 SIGCHLD-driven wakes    | ~600 ms | ~210 ms |

Cold runs pay one-time init (`pthread_create` × 4, dyld, prelude
cache miss). Warm runs reflect the actual scheduler cost. The
goldens pin the *exit status* ("ok"), not the wall — fixture
timing assertions belong in the bench harness; this demo set is a
correctness gate, not a performance gate.

`demos/ping_pong/` remains byte-identical to its pre-lane golden
(`make demos-no-regression` baseline now 31, was 27).

`demos/baseline.txt` bumped 27 → 31 in this PR.

## Coverage gaps

- `examples/effects/reactor_*` test cases for cancel-during-park
  semantics are NOT in this PR. R1 keeps the existing
  cooperative-cancellation contract (cancel arrives at the next
  yield-point hook after the wake), so there is no new behaviour
  to test; the cancel-aware redesign queues for R2.
- `read_bytes` / `write_bytes` are prelude functions, not effect
  ops, and are NOT offloaded. They still run on the scheduler
  thread. Routing them through the pool is a separate lane.
- `file_append` is a `read_file` + `write_file` polyfill in
  `stdlib/fs/file.kai`, so it inherits the parking shape for free.
- LLVM backend wiring was not exercised: the `--backend=llvm`
  path emits the same default-handler dispatch as the C backend,
  so my edits in `kai_default_*` route through it unchanged, but
  the lane only validated `--backend=c`. The LLVM smoke fixtures
  in `stage2/Makefile` cover Spawn / mailbox / File and should
  catch any regression at tier1.

## Real cost vs estimate

Estimated 0.7–1.0 agent day. Actual: ~1 day. Main time sinks:

- ~30 min: scheduler-primitive inventory + correctly identifying
  that `kai_sched_park` panics on empty queue and must be lifted.
- ~3 hours: writing the reactor core, forward-decl plumbing,
  thread-pool with the SIGCHLD signal-mask gymnastics.
- ~1 hour: tracking down the `state = PARKED` reorder bug. The
  initial run printed `kai: deadlock — fiber parked with empty
  run queue (1 parked total)` because the reactor drained the
  timer wheel and called `kai_sched_unpark` on a still-RUNNING
  fiber. Two `fprintf` probes (`DEBUG park_timer called`,
  `DEBUG sched_park dequeue=NULL reactor_count=N`) localised it
  in under five minutes.
- ~30 min: the first-`Process.wait` race window (child exited
  before SIGCHLD handler installed) — fixed by moving
  `kai_reactor_init()` to `Process.start` instead of waiting for
  `Process.wait`.
- ~1 hour: docs + this retro.

## Follow-ups for next lanes

- **R2 (TCP sockets) — Orongo.** The reactor's `kai_reactor_wait`
  needs to grow real readiness FDs in its `poll()` set (or swap
  to `kqueue`/`epoll`). TCP `recv`/`send`/`accept`/`connect` move
  from the `NetTcp` blocking path to the readiness reactor.
  Cancel mid-syscall is the heavy lifting — see the R2 design
  doc that asu has queued.
- **R3 (stdin) — Orongo.** `read_line` parks the fiber. Likely
  needs an additional pool worker because terminal I/O blocks
  on tty modes that epoll cannot probe.
- **Cancel-aware sleep / wait.** R1 explicitly does NOT deliver
  Cancel mid-park; the cancel pad fires at the next yield-point
  after the wake. Cancel mid-syscall is the same redesign the
  R2 lane drives.
- **`read_bytes` / `write_bytes` parking.** The prelude builtins
  bypass the effect dispatch, so they still run inline. A small
  lane can route them through `kai_reactor_run_in_pool` once
  someone hits the use case.
- **`file_exists` / `file_delete` / `file_rename` parking.**
  Same shape; cheap follow-up.
- **kqueue / epoll fast path on macOS.** macOS `kqueue` already
  notifies on regular files, so the file pool could short-circuit
  there. Premature optimisation for v1; revisit if file-heavy
  workloads show up on a profile.
- **Timer-wheel data structure.** v1 ships a sorted singly-linked
  list — O(n) insert, O(1) drain. A heap is the textbook upgrade
  once concurrent sleeper counts cross ~100. The existing API
  (`kai_reactor_timer_insert` + the drain helper) is small enough
  to swap in place.
- **`KAI_RESET_REACTOR_STATE` for tests.** The reactor is process-
  scoped and never tears down. A test harness that wants to
  re-init between runs has no path today; not currently a problem
  but worth a paragraph in the test discipline doc.

## Lessons load-bearing for future runtime lanes

1. **Order matters when wake paths and park paths share state.**
   Any new park-and-park-on-X structure must set the PARKED state
   BEFORE arming the wake source. The reactor wake is a kernel
   event — it can fire arbitrarily early.

2. **Signal masks on worker threads are not automatic.** The
   default pthread mask inherits the creating thread's. Forgetting
   to block SIGCHLD on the workers cost ~30 min of "why is the
   wall time still 3× too long" debugging.

3. **`poll()` is enough until you need readiness FDs.** R1 was
   sketched as `kqueue`/`epoll` because the parent design doc
   anticipated R2's socket shape. Doing the smaller thing first
   shipped the same observable behaviour in half the code.

4. **`SA_RESTART` and `poll()` interact badly.** If poll restarts
   on signal, you lose the wake hook the signal handler installed.
   Reactors specifically want EINTR.

5. **Cold cache is bigger than scheduler overhead.** Demos that
   look slow on the first run almost always pay dyld + prelude
   cache miss; warm runs are the load-bearing measurement.
