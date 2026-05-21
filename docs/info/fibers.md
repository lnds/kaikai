FIBERS(7)                       kaikai                       FIBERS(7)

NAME
  fibers — structured concurrency via nursery, spawn, await, cancel

SYNOPSIS
  nursery { n -> ... n.spawn(thunk) ... n.await(fiber) ... }
  Spawn.spawn(thunk)                                # inside Spawn handler
  Spawn.await(fiber)
  Spawn.cancel(fiber)
  Spawn.select([fiber1, fiber2])                    # first to finish wins

DESCRIPTION
  kaikai concurrency is BEAM-style fibers (private heap, cheap, ~64 KiB
  default stack) coordinated by structured-concurrency primitives. The
  `Spawn` effect provides spawn/await/select/cancel as ops; `nursery`
  is the stdlib helper that installs Spawn for a scope and joins all
  children at exit.

  Fibers are not OS threads. The runtime parks them on I/O via the
  reactor (R1 file/sleep/process, R2 TCP, R3 stdin, R4 signal) — no
  blocking call freezes the OS thread.

  Cancellation is COOPERATIVE: `Spawn.cancel(f)` marks the target;
  the scheduler injects `Cancel.raise()` into the target at its next
  yield point (next I/O op or other scheduling boundary). The body
  may `handle { ... } with Cancel { raise(_) -> ... }` to run cleanup.
  There is no preemption.

NURSERY

  fn fetch_all(urls: [String]) : [String] / Spawn + NetTcp + Stdout = {
    nursery { n ->
      let fibers = urls | (url => n.spawn(() => fetch_one(url)))
      fibers | (f => n.await(f))
    }
  }

  - On normal exit, the nursery awaits every spawned child.
  - If any child throws, the nursery cancels the rest and re-raises.
  - The nursery scope is the structured boundary; fibers cannot escape.

ACTORS
  For message-passing, see `kai info actors`. Actors are built on top
  of fibers + a private mailbox.

CANCELLATION
  `Cancel` is a separate effect, orthogonal to `Spawn`. Its single op
  is `raise() : Nothing`. The scheduler injects `Cancel.raise()` into
  a fiber whose `Spawn.cancel(f)` was called, at the next yield point.
  Handle it to run cleanup:

    fn worker() : Unit / Cancel + Stdout = {
      handle {
        long_running_loop()
      } with Cancel {
        raise(_) -> Stdout.print("worker cleaning up")
      }
    }

  See `docs/structured-concurrency.md` for the full semantics.

SELECT

  let winner = Spawn.select([fiber_a, fiber_b])
  # winner is the first fiber that completed; the others are not auto-cancelled.

NOT IN KAIKAI
  - `async` / `await` keywords. Concurrency is an effect, not syntax.
  - Goroutines / unstructured spawn. Every spawn lives in a nursery.
  - Channels as a primitive. Use Actors or nursery + shared State[T].
  - OS-thread parking under blocking syscalls. Reactor parks fibers.
  - Multi-shot resume (which would mean a fiber's continuation
    runs twice). One-shot only.

SEE ALSO
  kai info effects, kai info actors, kai info main
  docs/structured-concurrency.md, docs/fibers-honesty-targets.md
