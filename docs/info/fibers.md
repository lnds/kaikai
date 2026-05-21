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

- On normal exit, the nursery awaits every spawned child.
- If any child throws, the nursery cancels the rest and re-raises.
- The nursery scope is the structured boundary; fibers cannot escape.

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
