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
an active nursery. `nursery` itself is a stdlib helper that
installs the `Spawn` handler — also not a built-in: the language
core has no scheduling concepts, only the effect system.

## Syntax

```kai
nursery { n ->
  let a = n.spawn { task_a() }
  let b = n.spawn { task_b() }
  combine(n.await(a), n.await(b))
}
```

- `nursery { n -> ... }` opens a scope. `n` is the `Spawn`
  capability binding (the trailing-lambda + `as`-style cap form
  pinned in `docs/syntax-sugars.md`).
- `n.spawn(f: () -> T / e) : Fiber[T] / Spawn + Cancel`
  creates a child fiber.
- `n.await(f: Fiber[T]) : T / Spawn + Cancel` suspends until
  the child finishes, returns its value.
- `n.select(fs: [Fiber[T]]) : T / Spawn + Cancel` returns when
  **any** listed child finishes; the others are cancelled.
- `n.cancel(f: Fiber[T]) : Unit / Spawn` requests cancellation
  of one specific child. The child receives `Cancel.raise()`
  at its next yield point.
- `n.cancel_all() : Unit / Spawn` requests cancellation of every
  child of the nursery. Same delivery mechanism as `cancel`,
  applied to all live children.
- Leaving the nursery block **waits for all pending children** —
  no explicit `await` is required.
- If a child raises `Cancel` on its own (a crash that is not a
  requested cancellation), the nursery cancels the remaining
  children eagerly — at the failing child's termination, not when
  the scope's join reaches it — drains them, and re-raises out of
  the scope. A child
  cancelled on request (`n.cancel`) is an expected outcome and does
  not propagate. (`panic` is a process-terminating escape, not a
  scope-recoverable failure.)

`spawn`, `await`, and `select` carry `Cancel` because each is a
yield point: a fiber blocked in any of them can be cancelled by
the scheduler.

## Type system

- `Fiber[T]` and `Pid[Msg]` are scope-bound handles, not
  first-class movable values. The typer rejects any non-stdlib
  function that mentions either handle in its return type — at
  any depth in the TypeExpr (`Result[_, Fiber[Int]]`,
  `Option[Pid[Msg]]`, `[Fiber[T]]`, …) **and** transitively
  through user-defined sum-type constructor payloads
  (`type Boxed = Wrap(Fiber[Int])`). A narrow allow-list
  (`fiber_spawn`, `mailbox_alloc`, `spawn_actor`, …) admits the
  legitimate stdlib producers; everything else is rejected with
  a diagnostic that names the wrapping constructor + sum type
  when the breach goes through a payload. Coverage:
  `examples/effects/m8x_6_fiber_in_result.kai`,
  `m8x_6_pid_escapes.kai` (direct / parametric),
  `m8x_7_fiber_in_sum.kai`, `m8x_7_pid_in_sum.kai`
  (sum-payload), `m8x_7_recursive_sum_no_breach.kai` (recursive
  sums traverse + terminate without false positive).
- `spawn(f: () -> T / e)` makes the **effect set `e`** part of the
  nursery's own row. A nursery that spawns only pure tasks has no
  additional effect; a nursery that spawns `Io` tasks has `Io`.
- Cancellation is an effect, `Cancel`. A task can `handle` it to
  release resources. Unhandled `Cancel` unwinds the fiber cleanly.

The cap-binding form `nursery { n -> ... }` is the syntactic
introduction site for the per-nursery brand. The kaikai stage 2
compiler runs a pre-typecheck rewrite that strips `n` from the
lambda parameter list, rewrites every `n.<spawn|await|select|...>`
in the body to the corresponding `Spawn.<op>` call, and tags each
rewritten call with a fresh BrandId (one per nursery site). The
brand-mismatch checker walks the rewritten AST with a brand-
attribution environment (binding name → brand) and rejects
sibling-nursery breaches at consume sites (`n.await(f)` /
`n.cancel(f)` / `n.select(fs)`) where the fiber's brand differs
from the surrounding scope's. Brand attribution propagates
through `let p = expr` chains: a binding inherits the brand of
its rhs when the rhs is a known-spawn call or another branded
binding. Coverage:
`examples/effects/m8x_8_nursery_cap.kai` (positive — basic
cap-binding spawn/await),
`m8x_8_brand_through_match.kai` (positive — same-nursery
let-chain),
`m8x_8_sibling_nursery_mismatch.kai` (negative — direct
cross-nursery use),
`m8x_8_brand_propagation_let_chain.kai` (negative — chain
through three lets).

The pragmatic v1 of this lane (issue #71 option (b)) lives as a
side-table walker keyed on call-site `(line, col)`; the brief's
spec'd long-term shape — `TyBranded(Ty, BrandId)` woven into the
unifier so brands flow through generic helper instantiations — is
documented in `docs/fibers-honesty-targets.md` §*Residual m8.x
items* item 1 as the next refinement (helper-passthrough cases
where a generic `fn id[T](x: T) : T = x` round-trip strips the
brand). Direct-binding propagation (`let p = q`) and the
spawn-site-to-consume-site loop are covered today.

### Debugging brand inference

`kaic2 --dump-brands <file>` emits the brand registry collected
by the cap-binding rewrite. Output is deterministic (one record
per line, fixed column order) and grep-friendly:

```text
# kaic2 --dump-brands
# brand allocations (one per `nursery { n -> ... }` site):
brand 0 cap=outer at=examples/effects/m8x_8_brand_dump.kai:12:23
brand 1 cap=inner at=examples/effects/m8x_8_brand_dump.kai:14:31
# call attributions (one per rewritten `n.<op>` site):
call op=spawn brand=0 at=examples/effects/m8x_8_brand_dump.kai:13:24
call op=spawn brand=1 at=examples/effects/m8x_8_brand_dump.kai:15:26
call op=await brand=1 at=examples/effects/m8x_8_brand_dump.kai:16:18
call op=await brand=0 at=examples/effects/m8x_8_brand_dump.kai:18:16
```

The flag does not run inference, so it is cheap to consult on a
file whose typer would otherwise reject the program. Pinned by
the `m8x_8_brand_dump.brands.expected` golden under tier 1.

```kai
# Polymorphic over whatever effect the worker carries.
pub fn pmap[A, B, e](xs: [A], f: (A) -> B / e) : [B] / e + Spawn + Cancel {
  nursery { n ->
    xs | (x) => n.spawn { f(x) } | n.await
  }
}
```

If the caller is in a pure context, `e` resolves to the empty set
and the row collapses to `/ Spawn + Cancel`. If the caller is
already under `Console`, `e = Console` and the signature becomes
`/ Console + Spawn + Cancel`.

## Cancellation

`Cancel` is delivered when:

- A sibling fiber in the same nursery crashes.
- The nursery is cancelled wholesale via `n.cancel_all()` (cancels
  every child) or a child is cancelled individually via
  `Spawn.cancel(fiber)`.
- The surrounding fiber is itself cancelled (propagation).

A fiber that wants to clean up on cancellation wraps the affected
region in a `Cancel` handler. The handler runs the cleanup and
does not call `resume`; the fiber unwinds out of the wrapped
block:

```kai
fn worker(m: ActorCap[Job]) : Unit / Actor[Job] + Console + Mutable + Cancel {
  let counter = Mutable.ref_make(0)
  handle {
    forever {
      process(counter, m.receive())
    }
  } with Cancel {
    raise(resume) -> {
      let final = Mutable.ref_get(counter)
      Console.eprint("worker: cancelled after #{final} jobs")
      # resume is intentionally not called — the fiber unwinds
    }
  }
}
```

If a fiber has no `Cancel` handler, the runtime delivers
`Cancel.raise()` at the next yield point and the fiber unwinds
cleanly. There are no silent survivors. Doc B §`Cancel`
*Handling for cleanup* and *Unwind through nested handlers*
pin the wider semantics.

One exception applies in the actor model: when a fiber is linked
to a peer with `trap_exit=true` (set via `fiber_set_trap_exit`),
its `Cancel.raise()` bypasses any `with Cancel { raise(_) -> ... }`
handler in the call chain — including handlers inherited from the
parent fiber's evidence chain at spawn time — and unwinds straight
to the trampoline cancel pad so the link-propagation walk can push
a `"Crashed"` string into the supervisor's mailbox. See
docs/actors.md §*Trap-exit semantics* for the BEAM-faithful
contract this enforces (issue #103).

## Patterns

### Race (first result wins)

```kai
pub fn race[T, e](options: [() -> T / e]) : T / e + Spawn + Cancel {
  nursery { n ->
    let fibers = options | (f) => n.spawn(f)
    n.select(fibers)
  }
}
```

Polymorphic in `e` so the candidate functions can carry any
effect set (`Console`, `File`, `Io`, etc.).

### Timeout

```kai
type Outcome[T] =
  | Value(value: T)
  | Timeout

pub fn with_timeout[T, e](
  ms:   Int,
  task: () -> T / e
) : Option[T] / e + Spawn + Cancel + Time {
  nursery { n ->
    let winner = n.spawn { Value(task()) }
    let timer  = n.spawn { Time.sleep(ms); Timeout }
    match n.select([winner, timer]) {
      Value(v) -> Some(v)
      Timeout  -> None
    }
  }
}
```

`Time.sleep(ms)` is an op of a `Time` effect (deferred to a
later doc; for v1, treat `Time` as a placeholder for whatever
clock capability the runtime exposes — likely `Spawn` or a
dedicated `Time` effect).

### Actor supervision

```kai
pub fn run_pipeline(
  s: Pid[Source], w: [Pid[Work]], k: Pid[Collected]
) : Unit / Spawn + Cancel {
  nursery { n ->
    let source = n.spawn { producer(s) }
    each([1..4]) { _ -> n.spawn { worker(s, w) } }
    let sink   = n.spawn { collector(w, k) }
    n.await(sink)
  }
}
```

If any worker crashes, the producer and sink are cancelled
before `run_pipeline` returns. For the actor surface (mailbox
policies, link/monitor supervision, `spawn_actor`,
`with_mailbox`), see `docs/actors.md`.

## Execution and parallelism
<!-- coverage: skip -->

Fibers are scheduled M:N over OS threads with work-stealing.
`KAI_THREADS=N` selects the thread count at process start; unset, it
is the host CPU count. `KAI_THREADS=1` is byte-identical to the
cooperative single-thread scheduler.
None of the semantics in this document change with N: nurseries,
cancellation and awaits behave identically; a message or spawned
thunk that crosses a thread boundary is physically copied, which is
what keeps per-fiber RC free of atomics. See
`docs/mn-scheduler-design.md` for the scheduler design and
`kai info fibers` for the user-facing summary.

## Root nursery

`main` runs inside an implicit root nursery, so `spawn` is
usable at the top level without ceremony. Any `main` whose
effect row contains `Spawn` triggers the root nursery
installation (Doc B §`main` and the runtime *Installation
order*):

```kai
fn main() : Unit / Console + Spawn + Cancel {
  let processed = pmap([1..100], heavy)
  Console.print("processed #{processed.length} items")
}
# In this example pmap closes its internal nursery before
# returning, so there is nothing left to wait on. The implicit
# root nursery matters when main spawns fibers directly at the
# top level (without an inner nursery) — those fibers are
# joined before the program exits with 0.
```

## Non-goals

- **Unbounded concurrency**. There is no `detach` / "fire and
  forget". Every fiber is owned by exactly one nursery.
- **Pre-emptive cancellation**. A fiber stuck in a tight CPU
  loop with no effect ops is not interrupted until it yields
  (e.g. via `Spawn.yield()`). The runtime does not insert
  async safety points or signal-driven preemption —
  cancellation is cooperative by design. See §Cancellation for
  delivery rules and `Spawn.yield()` placement.
- **Priority schedulers, fairness guarantees**. Post-MVP.
- **Distributed supervision**. Phase 5+ at the earliest, when
  distribution lands.

## Implementation notes

Stage 2 work, milestone **m8** — the scheduler and `Spawn` /
`Cancel` runtime support land after m7a (effects mechanics) and
m7b (sugars). Doc B §*Next steps* tracks the m7a/m7b split;
this doc's deliverables sit on top of both.

Requires:

- The effects + handlers machinery from m7a (row unification,
  CPS transform, handler-stack runtime).
- `Fiber[T]` as a region-branded handle; the region check is
  a small extension to the existing type checker, sharing the
  brand machinery with `Pid[Msg]` from `docs/actors.md`.
- A scheduler in the runtime: per-fiber stacks (segmented or
  heap-allocated), a cooperative scheduler loop, and a
  cancellation flag every yield-point op checks.
- `nursery` as a stdlib helper installed as an effect handler;
  the trailing-lambda form `nursery { n -> ... }` desugars (per
  `docs/syntax-sugars.md` §1) to a call passing the body to
  the handler.

## References

- Nathaniel Smith, *Notes on structured concurrency, or: Go
  statement considered harmful* (2018).
- BEAM / Erlang, the original "isolated fibers with private
  heap, messages copied" model that kaikai inherits at the
  runtime level.
- Trio, Python structured-concurrency library.
- Kotlin `coroutineScope`; Swift `async let` / `TaskGroup`.
- OCaml 5 Eio.
