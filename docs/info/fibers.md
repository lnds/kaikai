# fibers

Structured concurrency via nursery, spawn, await, cancel — BEAM-style
fibers coordinated by the `Spawn` effect.

## Description

kaikai concurrency is fibers (private heap, cheap, ~64 KiB default
stack) coordinated by structured-concurrency primitives. The `Spawn`
effect provides spawn/await/select/cancel as ops; `nursery` is the
stdlib helper that installs Spawn for a scope and joins all children
at exit.

Fibers are not OS threads. The runtime parks them on I/O via the
reactor (R1 file/sleep/process, R2 TCP, R3 stdin, R4 signal) — no
blocking call freezes the OS thread.

Cancellation is COOPERATIVE: `Spawn.cancel(f)` marks the target;
the scheduler injects `Cancel.raise()` into the target at its next
yield point. There is no preemption.

## Nursery

```kaikai
import spawn

fn fetch_one(url: String) : String / Stdout = {
  Stdout.print("fetching #{url}")
  url
}

fn fetch_all(urls: [String]) : [String] / Spawn + Stdout = {
  nursery { n ->
    let fibers = urls | (url => n.spawn(() => fetch_one(url)))
    fibers | (f => n.await(f))
  }
}

fn main() : Int / Spawn + Stdout = {
  let results = fetch_all(["a", "b", "c"])
  Stdout.print("got #{int_to_string(results.length())}")
  0
}
```

- On normal exit, the nursery scope is the structured boundary;
  fibers cannot escape its lexical extent.

> **v1 status (2026-05-21):** `nursery` is a typed pass-through —
> the body runs and its return value is returned, but the
> structured semantics ("wait for every child on exit; cancel the
> rest and re-raise on child failure") are m8.x scope, tracked in
> `docs/fibers-honesty-targets.md`. Spawned children outlive the
> nursery body under the current cooperative scheduler.

## Actors

For message-passing, see `kai info actors`. Actors are built on top
of fibers + a private mailbox.

## Cancellation

`Cancel` is a separate effect, orthogonal to `Spawn`. Its single op
is `raise() : Nothing`. The scheduler injects `Cancel.raise()` into
a fiber whose `Spawn.cancel(f)` was called, at the next yield point.
Handle it to run cleanup. The clause receives `resume` by convention;
since `raise` returns `Nothing` the handler typically ignores `resume`
and short-circuits.

```kaikai
fn worker() : Int / Cancel + Stdout = {
  handle {
    Stdout.print("working")
    42
  } with Cancel {
    raise(resume) -> { Stdout.print("cleaning up"); 0 }
  }
}

fn main() : Int / Cancel + Stdout = worker()
```

The same handler fires whether the cancel was raised synchronously
inside the body (a direct `Cancel.raise()`) or arrived from a sibling
fiber's `Spawn.cancel(self_fiber)`. Both paths walk the target
fiber's evidence stack for the innermost `with Cancel { ... }` in
scope and dispatch through it; only when no user handler is in
scope does the runtime unwind the fiber to its trampoline directly.
This makes "wrap the body in `handle { ... } with Cancel { raise(_)
-> cleanup }`" the canonical pattern for graceful shutdown — a
SIGINT-driven supervisor that calls `Spawn.cancel(server)` runs the
server's cleanup before the fiber terminates.

The exception is a fiber that is `Link.link`'d to a peer with
`Spawn.set_trap_exit(true)`: in that case the runtime bypasses the
fiber's own Cancel handlers so the supervisor observes the child's
termination through its mailbox (see `kai info actors` §*Trap-exit
semantics*).

## NOT IN KAIKAI

- `async` / `await` keywords. Concurrency is an effect, not syntax.
- Goroutines / unstructured spawn. Every spawn lives in a nursery.
- Channels as a primitive. Use Actors or nursery + shared State[T].
- OS-thread parking under blocking syscalls. Reactor parks fibers.
- Multi-shot resume (which would mean a fiber's continuation
  runs twice). One-shot only.

## See also

`kai info effects`, `kai info actors`,
`docs/structured-concurrency.md`, `docs/fibers-honesty-targets.md`
