# Lane experience — issues #678 + #679 (2026-05-23)

Two runtime bugs fixed in one lane because they share the same
operational shape: programs that *appear* to hang or lose output
when run under real-world I/O patterns (pipes, redirects,
signal-driven termination, graceful shutdown). Both surfaced
building `lnds/todo-demo` (the integrated demo for the Anga Roa
release, kaikai#606) and both block the canonical `run_app`
HTTP-server pattern from working end-to-end.

## Scope as shipped

### Issue #678 — stdout block-buffered when redirected

**Symptom**: `kai_program > /tmp/log` showed nothing in `/tmp/log`
even after many `Stdout.print` calls; `kill -TERM` dropped the
entire buffered prefix. Surfaced as "the demo session script
captures zero log output during a presentation".

**Root cause**: libc defaults `stdout` to fully-buffered (4 KiB)
when the fd is a pipe / regular file / socket. The runtime never
overrode that. Normal exit flushed via atexit, but signal-driven
termination skipped the flush and lost everything in the buffer.

**Fix** (~3 LOC in `stage0/runtime.h`): call
`setvbuf(stdout, NULL, _IOLBF, 0)` once in `kai_set_args` (the
process-entry hook). Match Go / Rust / Python `-u` defaults.
TTY stdout is already line-buffered so this is a no-op there;
non-TTY stdout flips from block-buffered to line-buffered. One
syscall at startup, zero per-call overhead.

### Issue #679 — Spawn.cancel does not reach a reactor-parked fiber

**Symptom**: A fiber parked in `time.sleep`, `NetTcp.accept`,
`Stdin.read_line`, or `Signal.await` ignored `Spawn.cancel(self)`
issued from a sibling. The cancel flag was set, but the reactor
never woke the fiber, and the cancel-yield-point check happens
only at op-call boundaries which the parked fiber never reaches.
Surfaced as "Ctrl-C does not cleanly shut down an HTTP server
built on the canonical `run_app + accept-loop` pattern".

**Root cause**: `kai_default_spawn_cancel` set
`target->cancel_requested = 1` and returned. The target fiber
sat on its reactor waiter list (`kai_reactor_socket_read_waiters`
for `accept` / `recv`, `kai_reactor_timer_head` for `sleep`,
etc.) until the underlying event fired. No path connected the
cancel to the waiter list.

**Fix** (~50 LOC in `stage0/runtime.h`):

1. New helper `kai_reactor_detach_fiber(target)` that walks all
   seven reactor waiter heads (socket-read, socket-write, pid,
   filepool, timer, stdin, signal) and splices `target` out of
   whichever list it sits on. Returns 1 if found, 0 otherwise.
   O(N) per list; lists are short by the per-fiber-arena
   discipline.

2. `kai_default_spawn_cancel` now calls `detach_fiber` after
   setting the flag and `kai_sched_unpark(target)` when the
   detach found the fiber. Idempotent for non-parked targets
   (the flag still lands and is observed at the next op-call
   boundary the existing pre-#679 path).

3. `kai_check_cancel_yield_point()` runs at the end of
   `kai_sched_park` on both the same-frame and swapcontext
   resume paths. That covers every reactor park site by
   construction — `accept`, `send`, `recv`, `connect`,
   `sleep`, `read_line`, `read_bytes`, signal wait, pid wait,
   file-pool dispatch. No per-site changes needed.

4. `kai_default_spawn_await` accepts state `CANCELLED` in
   addition to `DONE`. A cancelled-mid-flight target's `result`
   may be NULL; await falls back to `kai_unit()` so the awaiter's
   continuation receives a well-typed value. Without this the
   nursery awaiting a cancelled fiber panics with "Spawn.await
   woken but target not DONE (state=5)".

The third change is the one that exposed the gap between
"cancel reaches the fiber" and "await sees the cancel cleanly".
Both reviewers (linus, asu) had focused on the first; the
second only showed up at runtime.

## Design decisions

### Why centralise the yield-point check in `kai_sched_park`

The alternative — adding `kai_check_cancel_yield_point()` after
every `kai_reactor_park_*` call site (eight or so) — would have
worked for the call sites we know about today but would silently
miss any new park site added in the future. Putting the check in
`kai_sched_park`'s tail means every present and future reactor
park inherits the behaviour. Single source of truth.

### Why both flag and unpark, not just unpark

Some call sites for `Spawn.cancel` target a fiber that is *not*
reactor-parked (e.g. cooperatively yielding via `kai_sched_yield`
or actively running on a different OS-thread frame). The flag
needs to land regardless; `detach_fiber` returns 0 in that case
and the unpark is skipped. Idempotent and correct.

### Why `await` accepts CANCELLED

The structured-concurrency model in `docs/structured-concurrency.md`
says nurseries propagate cancel to children on exit and await
completion of all spawned fibers. If await panicked on cancelled
children, the very mechanism the user wants (sibling-cancel to
unwind a server loop) would crash the entire nursery instead of
draining cleanly. Treating CANCELLED as a valid terminal state
is the only consistent semantics.

## Structural surprises

1. **Two separate `parked_count` counters**. `kai_parked_count`
   is the scheduler's deadlock-detection counter (bumped by
   `kai_sched_park`, decremented by `kai_sched_unpark`).
   `kai_reactor_parked_count` is the reactor's poll-or-exit
   counter (bumped by `kai_reactor_park_*`, decremented by the
   reactor drain helpers). Both have to be balanced when we
   detach from a waiter list and unpark. The reactor drain
   helpers already do this; `detach_fiber` mirrors the same
   sequence: decrement reactor counter, then call `unpark`
   (which decrements the scheduler counter).

2. **The unboxed structures (`KAI_FIBER_DONE` vs `_CANCELLED`)
   already existed**. The runtime had the right state for cancel
   termination (`KAI_FIBER_CANCELLED = 5`); only `await` failed
   to recognise it. The state-5 panic was effectively documenting
   "this path is reachable but unhandled" — once the cancel
   reached the parked fiber (issue #679 part 1), part 2 surfaced
   immediately.

3. **The cancel-yield-point's `cancel_pad_set` guard**. The
   yield-point check at line 8870 reads:
   ```
   if (f->cancel_requested && !f->cancel_delivered && f->cancel_pad_set)
   ```
   The third condition matters: `cancel_pad_set` is true only
   when the fiber's body has been entered through the unwind
   trampoline. If a fiber is cancelled *before* its body runs
   (rare but possible — cancel issued in the same tick as
   spawn), the check is a no-op and the fiber runs to completion
   normally. The semantics is "best-effort cancel at next yield
   point"; not "guaranteed unwind at any cost". This matches the
   doctrine in `docs/structured-concurrency.md` §*Non-goals*:
   "Cancel discipline stays cooperative."

## Fixtures added

- `examples/runtime/issue_678_stdout_line_buffered.kai` — prints
  three lines, exits normally. The Makefile target redirects
  stdout to a file and diffs against the golden. The signal-
  driven path the bug report covers is harder to test in a
  diff-only Makefile target; this fixture establishes the
  redirected-stdout path works.
- `examples/effects/issue_679_cancel_reaches_parked.kai` —
  nursery spawns a sleeper (`time.sleep(60s)`) and a canceller
  (`time.sleep(100ms); Spawn.cancel(server)`); the test passes if
  the program exits in ~100ms instead of 60 seconds. Pre-fix the
  sleeper would block the whole minute.
- New Makefile targets `test-issue-678-stdout-buffered` and
  `test-issue-679-cancel-parked`. Added to `.PHONY`, `test`,
  `test-fast`.

## Cost vs estimate

#678: 15 minutes (read, diagnose, fix, test). Trivial one-liner.

#679: ~1.5 hours. The diagnosis was clean from the bug report
(it spelled out the hypothesis correctly). Implementation hit
two follow-on snags:
- First the `detach + unpark` alone wasn't enough — the
  accept-loop retry ate the wake. Added cancel-yield-point check.
- Then `await` panicked on state CANCELLED. Added the
  CANCELLED branch.

Both snags are documented above; the second one is the kind of
thing only runtime testing catches.

## Follow-ups

1. **Fixture for #678 covering the signal-driven path**. The
   current test only validates the normal-exit path
   (redirected-stdout-flushes-three-lines). The signal-driven
   case the bug report cites (the canonical scenario) is
   exercised manually but not asserted. A bash-driven test that
   spawns the binary, waits 1s, sends SIGTERM, checks the
   redirected file is non-empty would close the gap. Left as
   follow-up because the test infra doesn't have a clean place
   for "spawn + signal + diff" yet.

2. **Audit other park sites for the cancel-yield-point**. The
   `kai_sched_park` tail-check covers them all by construction,
   but it's worth grep-ping the runtime for any path that
   marks `KAI_FIBER_PARKED` without going through
   `kai_sched_park` (none found in this pass, but the audit is
   cheap and worth re-running before each runtime change).

3. **Test cancel-into-other-reactor-parks**. The fixture covers
   `time.sleep` (timer waiter). The fix is uniform across all
   seven waiter lists, but a sweep test that exercises each park
   point (sleep, NetTcp.accept, NetTcp.recv, Stdin.read_line,
   Signal.await, run-in-pool, pid-wait) would document the
   contract end-to-end. Left as follow-up.

4. **Eventual revert of the `kai_check_cancel_yield_point`
   forward declaration**. Currently a forward decl sits near
   the other reactor forward decls because the helper's body
   lives ~1000 lines further down. A future runtime reorder
   could put it before its callers and remove the forward
   decl. Not blocking.
