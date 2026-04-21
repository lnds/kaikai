# structured concurrency

Adopted for stage 2. Makes the "fibers + effects" model concretely
composable: every fiber lives inside a scope (`nursery`) that waits
for its children and propagates cancellation. No fiber can outlive
the block that spawned it.

## Motivation

Without structure, fibers are like raw `go` statements or threads:
they leak, they survive scope exits, they need manual join/cancel
plumbing. The *"Go statement considered harmful"* argument
(Nathaniel Smith, 2018) showed that fiber lifetimes are easier to
reason about when tied to lexical scopes. Trio, Kotlin coroutines,
Swift structured concurrency, and OCaml 5's Eio all converged on the
same answer.

kaikai takes it further by making the scope **the handler for a
`Spawn` effect capability**. `spawn`/`await`/`select` are not
built-in primitives; they are operations you can only invoke inside
an active nursery.

## Syntax

```kai
nursery (n) => {
  let a = n.spawn(() => task_a())
  let b = n.spawn(() => task_b())
  combine(n.await(a), n.await(b))
}
```

- `nursery (n) => { ... }` opens a scope. `n` is the capability
  (like `handle ... with ...` binds a resume).
- `n.spawn(f: () -> T / e) : Fiber[T] / Spawn` creates a child.
- `n.await(f: Fiber[T]) : T / Spawn` suspends until the child
  finishes, returns its value.
- `n.select([fibs...]) : T / Spawn` returns when **any** listed
  child finishes; the others are cancelled.
- Leaving the nursery block **waits for all pending children**.
- If any child crashes (unhandled effect or panic), the nursery
  cancels the remaining children, drains them, and re-raises the
  original cause.

## Type system

- `Fiber[T]` is a capability, not a value. It **cannot** escape the
  nursery that produced it: the scope type tags `Fiber` with a
  region brand and the return type checker rejects carrying it out.
  Same mechanism as Rust lifetimes, applied only to this capability.
- `spawn(f: () -> T / e)` makes the **effect set `e`** part of the
  nursery's own row. A nursery that spawns only pure tasks has no
  additional effect; a nursery that spawns `Io` tasks has `Io`.
- Cancellation is an effect, `Cancel`. A task can `handle` it to
  release resources. Unhandled `Cancel` unwinds the fiber cleanly.

```kai
# Polymorphic over whatever effect the worker carries.
pub fn pmap[A, B, e](xs: [A], f: (A) -> B / e) : [B] / e + Spawn {
  nursery (n) => {
    xs |> map((x) => n.spawn(() => f(x)))
       |> map(n.await)
  }
}
```

If the caller is in a pure context, `e` resolves to the empty set
and `Spawn` is the only visible effect. If the caller is already
under `Io`, `e = Io` and the signature becomes `/ Io + Spawn`.

## Cancellation

`Cancel` is delivered when:

- A sibling fiber in the same nursery crashes.
- The nursery receives a timeout / explicit `n.cancel()`.
- The surrounding fiber is itself cancelled (propagation).

A fiber receiving `Cancel` may:

```kai
fn worker() : Unit / Io + Cancel {
  let db = connect_db()
  handle {
    forever(() => process(receive()))
  } with {
    Cancel -> { close_db(db); resume() }
  }
}
```

If a fiber ignores `Cancel`, the runtime delivers it at the next
effect operation — there are no silent survivors.

## Patterns

### Race (first result wins)

```kai
pub fn race[T](options: [() -> T / Io]) : T / Io + Spawn {
  nursery (n) => {
    let fibers = options |> map((f) => n.spawn(f))
    n.select(fibers)
  }
}
```

### Timeout

```kai
pub fn with_timeout[T](ms: Int, task: () -> T / Io) : Option[T] / Io + Spawn {
  nursery (n) => {
    let winner = n.spawn(task)
    let timer  = n.spawn(() => { sleep(ms); Timeout })
    match n.select([winner, timer]) {
      Value(v) -> Some(v)
      Timeout  -> None
    }
  }
}
```

### Actor supervision

```kai
pub fn run_pipeline() : Unit / Io + Actor + Spawn {
  nursery (n) => {
    let source  = n.spawn(() => producer())
    let workers = [1..4] |> map((_) => n.spawn(() => worker(source)))
    let sink    = n.spawn(() => collector(workers))
    n.await(sink)
  }
}
```

If any worker crashes, the producer and sink are cancelled before
`run_pipeline` returns.

## Root nursery

`main` runs inside an implicit root nursery, so `spawn` is usable at
the top level without ceremony:

```kai
fn main() {
  let result = pmap([1..100], heavy)
  println("done")
}
# the implicit root nursery waits for any leftover fibers before the
# program exits with 0.
```

## Non-goals

- **Unbounded concurrency**. There is no `detach` / "fire and
  forget". Every fiber is owned by exactly one nursery.
- **Priority schedulers, fairness guarantees**. Post-MVP.
- **Distributed supervision**. Phase 5+ at the earliest, when
  distribution lands.

## Implementation notes

Stage 2 work. Requires:

- The effects + handlers machinery (already planned).
- `Fiber[T]` as a region-branded capability; the region check is a
  small extension to the existing type checker.
- A scheduler in the runtime: per-fiber stacks (segmented or
  heap-allocated), a cooperative scheduler loop, and a cancellation
  flag that every effect-op checks before resuming.
- `nursery` as a regular effect handler in the stdlib; the sugar
  `nursery (n) => { ... }` desugars to
  `handle Spawn with (n) => { ... }`.

## References

- Nathaniel Smith, *Notes on structured concurrency, or: Go
  statement considered harmful* (2018).
- Trio, Python structured-concurrency library.
- Kotlin `coroutineScope`; Swift `async let` / `TaskGroup`.
- OCaml 5 Eio; Lukas Holcik & Kayce Basques' posts on scope-based
  fibers.
