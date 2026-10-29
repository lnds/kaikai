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

kaikai takes it further by making the scope **a bracketed region of
the `Spawn` effect**. `spawn`/`await`/`select` are not built-in
primitives; they are operations you can only invoke inside an active
nursery. `nursery` itself is a stdlib helper — also not a built-in:
the language core has no scheduling concepts, only the effect system.

`nursery` does not handle `Spawn`; it brackets a scheduler-owned
scope, and `Spawn` stays in the row on both sides of its signature.
Fiber lifetime is owned by the runtime: `Spawn` is a fiber-local
effect whose evidence the scheduler installs per fiber, so it cannot
travel in a frame slot and no user handler can own its ops. See
`docs/effects-stdlib.md` §*Three classes of builtin effect*.

## The spawn boundary

A thunk handed to `spawn` must arrive with no unhandled user effect.
A child fiber starts with an *empty* evidence stack — it does not
inherit the parent's — because a parent handler's evidence lives on a
stack frame the parent overwrites once it returns. A handler installed
around the `nursery` is therefore not visible from inside the child.

Two kinds of effect still cross, and they are the only two: one whose
`default { }` block bridges to a runtime supplier (the scheduler
installs it per fiber, so `Stdout`, `Clock`, `Log`, `Env` … resolve
inside a child), and a fiber-local effect (`Cancel`, `Link`,
`Monitor`, `Spawn`, `Actor`), which binds the child's own disposition
by construction.

The thunk's row `e` is part of the spawner's own row (`spawn`,
`Spawn.spawn`, `spawn_actor` and `spawn_actor_policy` all return
`/ Spawn + e`), so a child that may perform `Cancel` or `Clock` makes
its spawner declare it, up to `main`, where the runtime installs the
default for each fiber.

The rule holds through generic code. A function whose parameter is
read inside a spawned thunk, or is passed on to a parameter that is,
spawns that argument: the effects of the argument handed in at each
call are held to the same rule, naming the call site. `pmap(xs, f)`
with `f : (A) -> B / Fail` is therefore rejected inside a
`handle { ... } with Fail`, even though `pmap` is polymorphic in `f`'s
row. The set of such parameters is part of a module's interface.

Anything else is rejected at compile time:

```text
error: effect not handled in spawned fiber: Fail
  = note: a fiber does not inherit the parent's handlers
  = help: handle the effect inside the spawned body: `handle { ... } with Fail { ... }`
```

Put the handler inside the spawned body:

```kaikai
import spawn

effect Fail {
  fail(msg: String) : Unit
}

fn work() : Int / Fail = { Fail.fail("boom"); 1 }

fn main() : Int / Spawn = {
  nursery { n ->
    let f = n.spawn(() => handle { work() } with Fail { fail(m, resume) -> 0 })
    n.await(f)
  }
}
```

The rule covers every surface that reaches a fiber: `n.spawn`, the
free `spawn`, `Spawn.spawn`, and `spawn_actor` / `spawn_actor_policy`.

## Syntax

```kaikai
import spawn

fn task_a() : Int = 1
fn task_b() : Int = 2

fn main() : Int / Spawn = {
  nursery { n ->
    let a = n.spawn { task_a() }
    let b = n.spawn { task_b() }
    n.await(a) + n.await(b) - 3
  }
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
- The re-raise is a `Cancel.raise()` like any other: it walks the
  handler stack of the fiber running the nursery body, so a
  `with Cancel { raise(_) -> ... }` enclosing the nursery — at any
  nesting level, in the root fiber or a spawned one — runs its
  clause and execution continues past the handle. That is the
  supervisor-at-the-boundary shape: catch the failure where the
  scope closes, clean up, and choose what happens next.

  ```kaikai
  import spawn

  fn risky() : Int / Cancel = Cancel.raise()

  fn main() : Int / Spawn + Stdout = {
    handle {
      nursery { n ->
        let r = n.spawn(() => risky())
        n.await(r)
      }
    } with Cancel {
      raise(resume) -> { Stdout.print("a child failed; cleaning up"); 1 }
    }
  }
  ```

  With no `with Cancel` in scope the re-raise is terminal, as
  before: it unwinds a spawned fiber to its cancel pad (marking it
  CANCELLED, which its own parent scope then sees as a failure), and
  at the program root, where no pad exists, it prints
  `kai: nursery child cancelled; no survivors` and exits non-zero.

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

Brands also survive a round-trip through a helper that returns one
of its parameters unchanged. Each fn is summarised as
*brand-transparent at parameter `i`* or *opaque* on the surface
AST, so `let g = id(f)` keeps `f`'s brand and a later
cross-nursery `n2.await(g)` is still rejected. Coverage:
`m8x_8_brand_through_generic.kai` (negative),
`m8x_8_brand_generic_roundtrip_ok.kai` (positive).

Where a brand cannot be followed — an opaque callee, a fiber
stored in a record, list or closure — the checker emits a
`warning:` naming the originating nursery instead of dropping the
guarantee silently (`m8x_8_brand_opaque_helper_warns.kai`).

Brands are deliberately NOT part of the type representation:
`TyBranded(Ty, BrandId)` cannot unify soundly (a brand is a
scope-instance property, and two brands meeting at one type
variable have no correct answer), and the escape check it would
need is rank-2. The reasoning is recorded in
`docs/fibers-honesty-targets.md` §*Residual m8.x items* item 1.

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

```kaikai
import spawn

# Polymorphic over whatever effect the worker carries.
pub fn pmap[a, b, e](xs: [a], f: (a) -> b / e) : [b] / Spawn + Cancel + e {
  nursery { n ->
    xs | (x) => n.spawn { f(x) } | n.await
  }
}

fn main() : Int / Spawn + Cancel = list.length(pmap([1, 2, 3], (x) => x * 2)) - 3
```

If the caller is in a pure context, `e` resolves to the empty set
and the row collapses to `/ Spawn + Cancel`. If `f` prints,
`e = Stdout` and the row becomes `/ Spawn + Cancel + Stdout`. The
row variable comes last in the row. `e` may only carry effects
that cross a spawn (§*The spawn boundary*).

## Cancellation

`Cancel` is delivered when:

- A sibling fiber in the same nursery crashes.
- A child is cancelled individually via `n.cancel(fiber)` /
  `Spawn.cancel(fiber)`, or loses a `select`.
- The surrounding fiber is itself cancelled (propagation).

A fiber that wants to clean up on cancellation wraps the affected
region in a `Cancel` handler. The handler runs the cleanup and
does not call `resume`; the fiber unwinds out of the wrapped
block:

```kaikai
import actor
import spawn

type Job = Job(Int)

fn serve(counter: Ref[Int]) : Unit / Actor[Job] + Mutable = {
  match Actor.receive() {
    Job(_n) -> Mutable.ref_set(counter, Mutable.ref_get(counter) + 1)
  }
  serve(counter)
}

fn worker() : Unit / Actor[Job] + Stdout + Mutable + Cancel = {
  let counter = Mutable.ref_make(0)
  handle {
    serve(counter)
  } with Cancel {
    raise(resume) -> {
      let final = Mutable.ref_get(counter)
      Stdout.print("worker: cancelled after #{final} jobs")
      # resume is intentionally not called — the fiber unwinds
    }
  }
}

fn main() : Int / Spawn + Stdout + Cancel = {
  nursery { n ->
    let f = n.spawn(() => with_mailbox {
      Actor.send(Actor.self(), Job(1))
      Actor.send(Actor.self(), Job(2))
      worker()
    })
    Spawn.yield()
    n.cancel(f)
  }
  0
}
```

If a fiber has no `Cancel` handler, the runtime delivers
`Cancel.raise()` at the next yield point and the fiber unwinds
cleanly. There are no silent survivors. Doc B §`Cancel`
*Handling for cleanup* and *Unwind through nested handlers*
pin the wider semantics.

One exception applies in the actor model: when a fiber is linked
to a peer with `trap_exit=true` (set via `spawn.set_trap_exit`),
its `Cancel.raise()` bypasses any `with Cancel { raise(_) -> ... }`
handler in the call chain — and unwinds straight
to the trampoline cancel pad so the link-propagation walk can push
a `"Crashed"` string into the supervisor's mailbox. See
docs/actors.md §*Trap-exit semantics* for the BEAM-faithful
contract this enforces.

## Patterns

### Race (first result wins)

```kaikai
import spawn

pub fn race[t, e](options: [() -> t / e]) : t / Spawn + Cancel + e {
  nursery { n ->
    let fibers = options | (f) => n.spawn(f)
    n.select(fibers)
  }
}

fn main() : Int / Spawn + Cancel = race([() => 1, () => 2]) - 1
```

Polymorphic in `e` so the candidate functions can carry any
effect set that crosses a spawn (`Stdout`, `Clock`, …).

Selection is by completion order. A candidate that terminates first
wins regardless of its position in the list; position breaks ties only
when several have already terminated. Losers are cancelled
cooperatively — the flag lands and each unwinds at its next yield
point — so the enclosing nursery can join them at scope exit instead
of waiting out work whose result nobody can observe.

### Timeout

```kaikai
import spawn
import time

type Outcome[t] =
  | Value(t)
  | Timeout

pub fn with_timeout[t, e](
  ms:   Int,
  task: () -> t / e
) : Option[t] / Spawn + Cancel + Clock + e {
  nursery { n ->
    let winner = n.spawn { Value(task()) }
    let timer  = n.spawn { time.sleep(time.millis(ms)); Timeout }
    match n.select([winner, timer]) {
      Value(v) -> Some(v)
      Timeout  -> None
    }
  }
}

fn slow() : Int / Clock = { time.sleep(time.millis(500)); 1 }

fn main() : Int / Spawn + Cancel + Clock + Stdout = {
  match with_timeout(10, slow) {
    Some(v) -> Stdout.print("value #{v}")
    None    -> Stdout.print("timed out")
  }
  0
}
```

`time.sleep` parks the fiber on the reactor timer, so a timer
child cancelled by `select` unwinds rather than sleeping out its
deadline.

### Actor supervision

```kaikai
import spawn

fn produce() : Int / Spawn = { Spawn.yield(); 1 }
fn crunch() : Int / Cancel = Cancel.raise()
fn collect() : Int / Spawn = { Spawn.yield(); 2 }

fn run_pipeline() : Int / Spawn + Cancel = nursery { n ->
  let _source = n.spawn { produce() }
  let _worker = n.spawn { crunch() }
  let sink    = n.spawn { collect() }
  n.await(sink)
}

fn main() : Int / Spawn + Stdout = {
  handle { run_pipeline() } with Cancel {
    raise(resume) -> { Stdout.print("a stage crashed; pipeline cancelled"); 1 }
  }
}
```

If any stage crashes, the others are cancelled before
`run_pipeline` returns, and the failure re-raises to the
`with Cancel` around it. For the actor surface (mailbox
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

```kaikai
fn child() : Unit / Spawn + Stdout = { Spawn.yield(); Stdout.print("child ran") }

fn main() : Int / Spawn + Stdout = {
  let _ = Spawn.spawn(() => child())
  Stdout.print("main returns")
  0
}
```

`main` spawns `child` at the top level, without a nursery of its
own; the root nursery joins it before the program exits.

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
  direct-style op dispatch, handler-stack runtime).
- `Fiber[T]` as a scope-bound handle (§*Type system*): the
  return-type escape check it shares with `Pid[Msg]`, plus the
  per-nursery brand check.
- A scheduler in the runtime: per-fiber stacks (segmented or
  heap-allocated), a cooperative scheduler loop, and a
  cancellation flag every yield-point op checks.
- `nursery` as a stdlib helper bracketing a scheduler-owned scope;
  the trailing-lambda form `nursery { n -> ... }` desugars (per
  `docs/syntax-sugars.md` §1) to a call passing the body as a
  thunk, with `n.<op>` rewritten to `Spawn.<op>`.

## References

- Nathaniel Smith, *Notes on structured concurrency, or: Go
  statement considered harmful* (2018).
- BEAM / Erlang, the original "isolated fibers with private
  heap, messages copied" model that kaikai inherits at the
  runtime level.
- Trio, Python structured-concurrency library.
- Kotlin `coroutineScope`; Swift `async let` / `TaskGroup`.
- OCaml 5 Eio.
