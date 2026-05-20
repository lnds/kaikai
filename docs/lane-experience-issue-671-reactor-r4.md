# Lane retro — issue #671 reactor R4 (Signal trap fiber-aware)

Date: 2026-05-20. HEAD pre-lane: v0.79.0. Edition: hanga-roa.

## Scope as planned vs scope as shipped

Issue body proposed `signalfd(2)` on Linux + `kqueue +
EVFILT_SIGNAL` on macOS — two platform-specific paths integrated
into the scheduler's `poll()` set. The lane shipped a **single
portable path**: an async-signal-safe `sa_handler` that writes the
`signo` byte to a self-pipe, drained by the existing `poll()` set.
Universal across Linux + macOS + every other POSIX target; no
platform split; reuses the exact infrastructure pattern that
SIGCHLD self-pipe (R1, #611) already uses in this same file.

Acceptance gate from the issue, all green:

- `Signal.await()` parks the fiber, not the OS thread. ✅
- Fixture with two concurrent fibers (busy + signal-await) shows
  both progressing until SIGTERM. ✅
  (`examples/effects/m8x_signal_await_parks.kai`)
- `lnds/ahu`'s `run_app` can now be rewritten as
  spawn+multiplex+cancel. ✅ (unblocks; ahu lane follows
  separately).
- `tier0` byte-identical selfhost holds. ✅
- `demos/signal_concurrent` shows `Spawn.cancel` reaching a
  fiber parked in `Signal.await()`. ✅ (bonus over the acceptance
  gate; serves as the visible regression guard alongside the
  existing R1/R2/R3 demos.)

## Design decisions and alternatives considered

### Why a self-pipe instead of `signalfd` / `kqueue`

The issue body suggested platform-specific signal mechanisms.
Three reasons the self-pipe won on review:

1. **Async-signal-safety budget is the same.** Both paths need
   the `sa_handler` to do something async-safe. `signalfd` reads
   are not in a handler at all (the handler runs in the kernel,
   we just read the fd from user space); `write(2)` to a pipe is
   on POSIX's async-signal-safe list. The self-pipe pays one
   extra syscall (write) but removes the platform split.

2. **Reuses the SIGCHLD path's exact shape.** `kai_reactor_sigchld_pipe`
   and its handler have been in this runtime since R1 and survived
   the bug-bash week. Cloning that pattern into
   `kai_reactor_signal_pipe` is one new variable + one new
   handler + one new drain helper, all symmetric. The reviewer
   for this lane can read the diff side-by-side with R1's pipe
   logic.

3. **No new platform dependencies.** `signalfd` is Linux-only;
   `kqueue + EVFILT_SIGNAL` is macOS/BSD-only. The runtime already
   has a `poll()` loop. Adding either would have meant a
   `#ifdef __linux__ / #else` block plus two new code paths to
   keep in sync. The self-pipe needs zero `#ifdef`.

### Single waiter slot (vs per-fiber waiter list)

The reactor pattern in this file uses two shapes: per-fd waiter
lists (sockets) and singleton slots (stdin). Signal is closer to
stdin: the wake source (a process-wide signal) has no fiber
identity attached. If two fibers were both parked in
`Signal.await()`, the runtime would have to pick one to wake, but
the byte in the pipe carries no information about which. v1
makes this explicit: `kai_reactor_signal_waiter` is a singleton;
a second concurrent `Signal.await()` panics with the same shape
as R3's stdin-multiplex panic. The diagnostic message names the
workaround ("serialize via an actor or supervisor").

A multi-waiter design is possible (each `await` would have to
register a signal mask and the drain would walk every waiter
checking masks) but unnecessary for v1 — every Signal use case
the issue body listed (`run_app`, `todo-demo`) has one waiter.

### `reactor_wait_status` reused for signo

The KaiFiber struct already had `reactor_wait_status` for
`Process.wait` exit codes. Signal arrival reuses the same slot
to pass the signo from drain back to the await handler. Saves
adding a new field to a struct that is per-fiber allocated and
already chunky. Documented inline at the park primitive.

### SA_RESTART on the new handler

R1's SIGCHLD handler deliberately does NOT set `SA_RESTART`
because the scheduler thread's `poll()` must return on EINTR so
the wake path is observed even when the self-pipe write races
the signal delivery. R4's signal handler **does** set
`SA_RESTART`. Two reasons:

1. The signal IS the payload (not a separate wake). If
   `SA_RESTART` is off, every blocking syscall in the program
   could surface EINTR to user code when the signal arrives,
   even if no one cares. With `SA_RESTART` set, only `poll()`
   wakes (and `poll()` already returns on every signal regardless
   of `SA_RESTART` per POSIX).
2. The wake-byte arrives via the self-pipe drain regardless of
   whether the syscall was restarted; we never need EINTR as the
   wake signal.

The two handlers (SIGCHLD and our signal handler) coexist with
different flags by design. Documented in the inline comment.

### Defensive SIGINT subscription

v1's behaviour was: empty subscribed set → `await` waits on
`{SIGINT}` so Ctrl-C still wakes the caller. R4 preserves that
behaviour and **also** installs the sa_handler for SIGINT in that
case. Otherwise a user who calls `Signal.await()` without
`Signal.on(SigInt)` would park forever (no handler installed, no
byte ever written to the self-pipe). The defensive subscription
keeps the v1 user-facing contract intact.

## Structural surprises

### No surprises in the runtime code

The integration is mechanical:

- 1 new global pair (`kai_reactor_signal_pipe`)
- 1 new global slot (`kai_reactor_signal_waiter`)
- 1 new handler (`kai_reactor_signal_handler`)
- 1 new drain (`kai_reactor_signal_drain`)
- 1 new park (`kai_reactor_park_signal`)
- 3 edits to existing fns (`kai_reactor_init`,
  `kai_default_signal_on`, `kai_default_signal_await`).

Everything follows the SIGCHLD/stdin pattern. No new abstractions
needed.

### The fixture uses the existing `sigharness.c`

`tests/sigharness.c` from issue #107 forks the child, waits for
"ready", sends SIGTERM (default), checks exit code. The R4
fixture (`m8x_signal_await_parks.kai`) writes "ready" first thing
in `await_loop`, runs `compute_loop` in parallel, and exits
cleanly when the harness delivers SIGTERM. The golden pins the
interleaving: `ready` first, three compute steps, `got SigTerm`,
`ok`. Compute steps appear **before** `got SigTerm` because the
harness sleeps ~50 ms before sending the signal, which is enough
for the three yield rounds.

### Demo is self-terminating

`demos/signal_concurrent/` is the visible regression guard. The
`make demos verify` harness runs each binary stand-alone (no
signal harness), so the demo can't depend on an external SIGTERM.
The compute fiber instead cancels the signal fiber via
`Spawn.cancel(target)` after three yields. This doubles as proof
that `Spawn.cancel` reaches a fiber parked in `Signal.await()` —
which is exactly the runtime requirement for ahu's `run_app`.

## Fixtures added

- `examples/effects/m8x_signal_await_parks.kai` (+ .out.expected) —
  harness-driven, regression guard for the parking property.
- `demos/signal_concurrent/main.kai` (+ .out.expected) — visible
  demo at the same level as `sleep_concurrent`, `stdin_concurrent`,
  `tcp_concurrent`. Self-terminates via Spawn.cancel.
- `demos/baseline.txt` bumped 33 → 34.

## Docs touched

- `docs/effects-stdlib.md` §Signal — replaced "blocks the OS
  thread" with the R4 reactor description; added a v1-status
  sidebar; flipped the Limitations bullet from "v1 blocks" to
  "closed by R4".
- `docs/fibers-honesty-targets.md` — added the R4 row to the
  Tier 2 table; added R4 to the §"Where we are today" listing
  alongside R1/R2/R3.

## Real cost vs estimate

Issue estimated 1-2 days. Real cost: ~2 hours (single agent
session). The estimate was conservative because the issue body
assumed a platform split; the self-pipe alternative collapsed
that into one path.

## Follow-ups for future lanes

1. `on_cancel(sig)` shape (issue #107 BEAM-style): still waits on
   the unrelated lane that lets Cancel honour user-installed
   handlers on runtime-triggered cancellation. Documented in
   `fibers-honesty-targets.md` §Residual m8.x items.
2. Multi-waiter signal slot (per-fiber masks). Not on any
   roadmap; v1 panic + serialize-via-actor is the documented
   answer.
3. `lnds/ahu` `run_app` upgrade — separate lane in the ahu repo,
   now unblocked by R4.

## What this lane closes

- Acceptance gate from #671: ✅ all three points.
- `lnds/ahu` blocker: ✅ unblocked (separate lane to write).
- Hanga Roa reactor surface: complete. R1+R2+R3+R4 cover every
  blocking I/O op the user can name.

`Signal.await()` no longer freezes the program. The path to
`ahu.run_app` Tongariki is open.
