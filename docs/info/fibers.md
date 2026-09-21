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
  Stdout.print("got #{results.length()}")
  0
}
```

- On normal exit, the nursery scope is the structured boundary;
  fibers cannot escape its lexical extent.
- Children are joined automatically: the nursery does not return
  until every child spawned inside it has finished, even with no
  explicit `await`.
- If a child raises `Cancel` on its own, the surviving siblings are
  cancelled and the failure re-raises out of the scope. A child
  cancelled on request (`n.cancel`) is an expected outcome and does
  not propagate.
- That re-raise walks the handler stack like any `Cancel.raise()`, so
  a `with Cancel` around the nursery catches it and execution
  continues past the handle. With no handler in scope it is terminal:
  the fiber unwinds cancelled, and at the program root the runtime
  prints `no survivors` and exits non-zero.

```kai
handle {
  nursery { n ->
    let r = n.spawn(() => risky())
    n.await(r)
  }
} with Cancel {
  raise(resume) -> println("a child failed; cleaning up")
}
```

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

A `Cancel` handler only fires at the scope where it is installed, so
one placed further out does not see an inner scope's resource. To
release a resource regardless of where the cancel is handled, give the
owning scope's handler a `finally { }` clause: it runs on every
unwinding exit, cancellation included, and `initially { }` acquires
into the handler state so both halves live in one construct
(`kai info effects` §*Cleanup*). The same applies to any clause that
abandons the continuation over an inner scope. Perceus frees memory on
all these paths regardless.

Nested handles resolve innermost-first, exactly as a synchronous
`Cancel.raise()` would: only the innermost `with Cancel` in scope
runs, and an outer one wrapping it does not.

The exception is a fiber that is `Link.link`'d to a peer with
`Spawn.set_trap_exit(true)`: in that case the runtime bypasses the
fiber's own Cancel handlers so the supervisor observes the child's
termination through its mailbox (see `kai info actors` §*Trap-exit
semantics*).

That exception is a deliberate hole in lexical handler scoping: under
trap-exit the bypass crosses *every* `with Cancel` in the chain, so a
`handle` you can see in the source provably does not run. It is the
price of OTP-style layered supervision composing — without it, any
intermediate cleanup handler between a supervisor's spawn and its
worker would keep the supervisor's `receive` from ever waking. Cancel
handlers in a fiber holding no trap-exit'd link are unaffected.

## Parallelism — `KAI_THREADS`

Fibers and actors run in parallel across N OS threads with a
work-stealing scheduler. On by default, no code changes — set
`KAI_THREADS` only to override:

```sh
./my_program              # N = host CPU count (capped at 32)
KAI_THREADS=8 ./my_program
KAI_THREADS=1 ./my_program
```

- `KAI_THREADS=1` is byte-identical to the cooperative single-thread
  scheduler — the escape hatch when you want one.
- Output that depends on the *order* independent fibers interleave is
  not stable above one thread; only causally ordered output is.
- Semantics are unchanged at any N: messages crossing a thread
  boundary are physically copied (same-thread sends still transfer
  ownership), each actor's mailbox is processed serially, and
  per-fiber RC stays non-atomic — parallelism costs nothing on the
  object hot path.
- A blocking FFI call stalls one worker thread, not the program.
- The I/O reactor is shared: heavily mixed I/O+CPU workloads may
  serialize on it.
- `main` is pinned to the OS thread the process entered on and never
  migrates, so a thread-affine C library (AppKit/GLFW/SDL/GTK) can be
  driven straight from `main` at any N — see `kai info ffi`. Spawned
  fibers migrate freely; only the entry fiber carries the pin.

## Mutable state and fibers

Each fiber owns a private heap. A value a spawned thunk captures is
copied into the child, so a mutation the child performs lands on the
copy: the parent never observes it, and neither does any sibling.
Where the boundary falls:

- **Inside one fiber**, `Mutable` is the ordinary way to hold state.
  `Ref` and `Array` behave exactly as they read — no races are
  possible, because nothing else reaches that heap.
- **Between fibers**, message passing, and only message passing. One
  fiber owns the state; the others ask it to act. See
  `kai info actors`.
- **`nursery` is control flow, not sharing.** It runs independent work
  in parallel and joins it. Results come back through `await`; they do
  not come back through a cell both sides hold.

A thunk that captures a value carrying mutation — a `Ref`, an `Array`,
or a record holding either, at any depth — is rejected where it is
spawned, because the copy would make the program quietly wrong rather
than loudly broken:

```kaikai-neg
import spawn

fn bump(r: Ref[Int]) : Unit / Mutable =
  Mutable.ref_set(r, Mutable.ref_get(r) + 1)

fn main() : Int / Spawn + Mutable = {
  let counter = Mutable.ref_make(0)
  nursery { n ->                        # error: a spawned fiber cannot
    let f = n.spawn(() => bump(counter)) #   capture `Ref[Int]`
    n.await(f)
  }
  0
}
```

Two shapes that look similar and are fine: a carrier CONSTRUCTED
inside the thunk belongs to the child and is never shared, and a value
READ before the spawn is captured as the value, not the cell.

```kaikai
import spawn

fn fill(buf: Array[Int]) : Int / Mutable = {
  let _ = array_set(buf, 0, 1)
  array_get(buf, 0)
}

fn compute(x: Int) : Int = x * 2

fn main() : Int / Spawn + Stdout + Mutable = {
  let counter = Mutable.ref_make(21)
  let snapshot = Mutable.ref_get(counter)   # an Int, not the cell
  nursery { n ->
    let a = n.spawn(() => { let buf = array_make(8, 0)  fill(buf) })
    let b = n.spawn(() => compute(snapshot))
    Stdout.print("#{n.await(a)} #{n.await(b)}")
  }
  0
}
```

This is the same rule the row check applies to effects, read on the
value side: a child fiber starts with an empty evidence stack AND a
heap of its own, so neither a handler nor a cell reaches it from the
parent.

## NOT IN KAIKAI

- `async` / `await` keywords. Concurrency is an effect, not syntax.
- Goroutines / unstructured spawn. Every spawn lives in a nursery.
- Channels as a primitive. Use Actors.
- Shared mutable state between fibers. See Mutable state and fibers.
- OS-thread parking under blocking syscalls. Reactor parks fibers.
- Multi-shot resume (which would mean a fiber's continuation
  runs twice). One-shot only.

## See also

`kai info effects`, `kai info actors`,
`docs/structured-concurrency.md`, `docs/fibers-honesty-targets.md`
