# actors

kaikai subsumes actors under the effect system. An actor is a
**fiber with a mailbox**, implemented as an `Actor[Msg]` effect
handler on top of the `Spawn` effect. There is no separate
"actor runtime" — the scheduler, supervision, and cancellation
machinery already specified for fibers in
`docs/structured-concurrency.md` carry actors for free.

(A *fiber* is kaikai's lightweight cooperatively-scheduled
thread — private heap, messages copied across fibers,
one-shot `resume` as tail call. Full spec in
`docs/structured-concurrency.md`.)

This document specifies the `Actor[Msg]` effect: operations,
mailbox policies, supervision, interaction with `Cancel`, and
the out-of-scope list. It refers back to `docs/effects.md` for
the row-and-handler semantics, `docs/effects-stdlib.md` for the
catalog slot reserving `Actor[Msg]`, and
`docs/structured-concurrency.md` for fiber lifetime rules.

Scope of v1: a single effect `Actor[Msg]`, three canonical
mailbox policies, link and monitor supervision primitives, and
clear semantics for `Cancel` mid-receive. Several extensions
are deferred — see §*Out of scope for v1* below.

## Context

The design commits in `docs/design.md` place actors under
effects ("concurrency subsumed under effects. Actors = effect
capability, not a separate primitive"). Doc B lists `Actor[Msg]`
in its out-of-scope section and points here; this doc fills it
in.

Prior art. BEAM actors (Erlang / Elixir) define the surface —
`pid`, `send`, `receive`, per-actor private mailbox — but leave
the mailbox untyped. Pony goes the opposite way: each actor
declares one specific message type, and the compiler rejects
sends that do not match. kaikai takes BEAM's surface and
applies Pony's typing: `Pid[Msg]` is a typed handle, an actor
handles exactly one `Msg` type, and the checker enforces it.
Links and monitors come from Erlang — the semantics is
load-tested and composes with fibers.

## The `Actor[Msg]` effect

### Declaration

```kai
effect Actor[Msg] {
  self()                          : Pid[Msg]
  send(pid: Pid[Msg], msg: Msg)   : Unit / Cancel
  receive()                       : Msg / Cancel
}
```

- `self()` — the pid of the current actor. Always available
  under an `Actor[Msg]` handler.
- `send(pid, msg)` — enqueue `msg` in `pid`'s mailbox. May
  block, drop a message, or complete immediately depending on
  the receiving mailbox's overflow policy (see §*Mailbox
  policies* below). The blocking case (`BlockSender`) is a
  yield point for the sender, so a blocked sender can itself
  receive `Cancel.raise()`.
- `receive()` — remove and return the next message from the
  current actor's mailbox. Blocks the fiber until a message
  arrives. Carries `Cancel` in its row because a blocked
  `receive` is a yield point where the scheduler delivers
  `Cancel.raise()` to a cancelled fiber.

`Msg` is the concrete message type of the actor. One
`Actor[Msg]` instance corresponds to one mailbox for one
message type. Two actors with different message types are two
different effects (`Actor[Request]` vs `Actor[Event]`). A
single actor that needs to mix message shapes uses one sum
type:

```kai
type ServerMsg =
  | Ping(client: Pid[Pong])
  | Stop
  | Tick
```

### `Pid[Msg]` — typed handle

```kai
# Pid is opaque, compiler-synthesised.
# Construction: returned by `spawn_actor` / `self`.
# Elimination: `Spawn.send(pid, msg)` and supervision ops.
```

`Pid[Msg]` is a region-branded handle (same mechanism as
`Fiber[T]` from `docs/structured-concurrency.md` §*Type
system*). It cannot escape the nursery that created the actor
— the region check prevents storing a `Pid[Msg]` in a data
structure that outlives the nursery.

Two pids are equal if and only if they refer to the same
actor instance. Equality is intentional: supervision,
monitoring, and message routing need it.

## Spawning actors
<!-- coverage: skip --> spawn_actor primitive deferred to Fibers Tier 2 (Monitor lane); m8x_4_recv_blocking covers the Pid handoff path indirectly

An actor is spawned through `Spawn` just like any other fiber,
with an `Actor[Msg]` handler wrapped around the body so
`send` / `receive` / `self` are in scope. The stdlib exposes
the capability to the body as an `as`-bound argument — the
idiomatic name is `m` (for "mailbox"), the same way
`nursery { n -> ... }` binds the `Spawn` capability as `n`.

### `spawn_actor` — explicit mailbox policy

```kai
pub fn spawn_actor[Msg, R, e](
  n:      Nursery,
  policy: MailboxPolicy,
  body:   (m: ActorCap[Msg]) -> R / Actor[Msg] + e
) : Pid[Msg] / Spawn + e
```

The body receives the mailbox capability `m` as its argument
and uses `m.receive()`, `m.send(pid, msg)`, `m.self()` to
interact with the mailbox. `Actor.receive()` / `Actor.send(...)`
— the default-capability form — remains legal but secondary;
binding the cap as `m` is shorter and matches `nursery`'s
shape.

### `spawn_actor_default` — bounded / block-sender default

```kai
pub fn spawn_actor_default[Msg, R, e](
  n:    Nursery,
  body: (m: ActorCap[Msg]) -> R / Actor[Msg] + e
) : Pid[Msg] / Spawn + e
```

Calls `spawn_actor` with `policy = Bounded(1024, BlockSender)` —
the 90th-percentile case. Requiring explicit policy on
`spawn_actor` keeps unbounded-by-accident out of the language
(see §*Open questions*); `spawn_actor_default` covers the
common case without that ceremony.

### `with_mailbox` — mailbox for the current fiber

```kai
pub fn with_mailbox[Msg, R, e](
  policy: MailboxPolicy,
  body:   (m: ActorCap[Msg]) -> R / Actor[Msg] + e
) : R / e
```

Installs an `Actor[Msg]` handler in the current fiber without
spawning a new one. Useful when `main` (or any existing fiber)
needs to receive replies — `with_mailbox` gives it its own
mailbox:

```kai
with_mailbox { m ->
  m.send(worker, Query(m.self()))
  let answer = m.receive()
  ...
}
```

A `with_mailbox` variant with the default policy
(`Bounded(1024, BlockSender)`) may be added if the explicit
form proves repetitive — deferred until usage data justifies
it.

### Longhand

`spawn_actor` is a thin wrapper over `Spawn.spawn` plus a
`handle { ... } with Actor[Msg] ... as m { ... }` installation:

```kai
pub fn spawn_actor[Msg, R, e](
  n: Nursery, policy: MailboxPolicy,
  body: (m: ActorCap[Msg]) -> R / Actor[Msg] + e
) : Pid[Msg] / Spawn + e {
  let (pid, mailbox) = runtime_alloc_mailbox(policy)
  n.spawn(() => {
    handle { body(cap) } with Actor[Msg](mailbox, pid) as cap {
      self(resume)       -> resume(pid)
      send(p, v, resume) -> { runtime_mailbox_push(p, v); resume(()) }
      receive(resume)    -> resume(runtime_mailbox_pop(mailbox))
      return(x)          -> x
    }
  })
  pid
}
```

(Inside the handler, `cap` is a local name for the capability;
the body receives it as the user-facing `m`.) The
`runtime_*` helpers are `Ffi` primitives specified in Doc C.
The runtime layout of mailboxes is a Doc C concern — this
document pins only that every mailbox is per-actor,
heap-allocated, and drained when the actor returns or is
cancelled.

## Mailbox policies
<!-- coverage: skip --> covered by m8_8_mailbox_drop_oldest / m8_8_mailbox_drop_newest / m8x_4_block_sender (filename keying does not catch "policies" but the fixtures exercise every documented policy)

Every mailbox has a policy that decides what `send` does when
the mailbox is full. v1 ships **two policies** — one of them
parameterised by a three-way overflow rule:

```kai
type MailboxPolicy =
  | Unbounded
  | Bounded(capacity: Int, on_full: Overflow)

type Overflow =
  | DropOldest
  | DropNewest
  | BlockSender
```

### `Unbounded`

No capacity limit; `send` always succeeds and never blocks.
Suitable for actors where message bursts are rare and memory
growth is not a concern (supervisors, short-lived coordinators).

The default recommendation for v1 is **not** unbounded; an
unbounded mailbox is a leaked-memory bug waiting to happen. It
exists in the catalog because some patterns genuinely need it —
bootstrap messaging where the producer cannot apply backpressure
(init hand-off from launcher to supervisor), or small queues
seeded once at creation with a fixed set of messages that will
be drained without further writes. `Bounded` is the default
people should reach for in every other case.

### `Bounded(capacity, on_full)`

Fixed capacity; `on_full` decides the eviction rule when a
`send` arrives at a full mailbox:

- **`DropOldest`** — the oldest unread message is evicted, and
  the new message is enqueued. Suitable for state snapshots
  where only the latest value matters (telemetry, tick events).
- **`DropNewest`** — the new message is rejected; the mailbox
  keeps its current contents. Suitable for "first-come
  wins" protocols (leadership election, lock acquisition).
- **`BlockSender`** — the sending fiber is parked until space
  frees up. Suitable for backpressure propagation through a
  pipeline. A `BlockSender` send is a yield point, so a blocked
  sender can receive `Cancel.raise()` and unwind.

Under `DropOldest` and `DropNewest`, `send` returns `Unit`
without signalling that a drop occurred — the sender does not
learn whether its message was delivered or evicted. If the
caller needs to know, wire an acknowledgement into the protocol
on the payload side, or switch to `BlockSender` so that
delivery is guaranteed (at the cost of backpressure).

### No multi-instance policies in v1

Per-priority queues, fair-share schedulers, and pluggable
policies are deferred. The three overflow rules plus
`Unbounded` are expected to cover the cases stdlib actors will
need; revisiting the set is an intentional, audited change once
real usage data exists. Both enums (`MailboxPolicy` and
`Overflow`) are closed in v1 — adding a variant to either is a
deliberate decision, not an incremental extension.

## Selective receive and future extension
<!-- coverage: skip --> design discussion of post-MVP extension; v1 receive is FIFO only

v1 `receive()` is **first-in-first-out**. Callers that need
"receive only messages matching predicate P" write it
explicitly:

```kai
fn receive_matching[Msg](m: ActorCap[Msg], p: (Msg) -> Bool) : Msg / Actor[Msg] {
  let msg = m.receive()
  if p(msg) { msg }
  else {
    # re-enqueue at the back and try again.
    m.send(m.self(), msg)
    receive_matching(m, p)
  }
}
```

This is O(n²) if many non-matching messages accumulate. v1
accepts that cost. A real selective-receive primitive
(preserving original order of non-matching messages) is on the
deferred list; it requires a second internal queue and changes
to the mailbox data structure — designed separately when
demand arrives.

## Supervision: links and monitors

Actors rarely run alone. When one crashes, its peers usually
need to know. kaikai provides two primitives, both implemented
as effects on top of `Spawn` and `Actor`.

### Links — bidirectional

```kai
effect Link {
  link(pid: Pid[_]) : Unit
}
```

`Pid[_]` is an **existential pid** — "a pid whose message type
we do not care about". It is a reserved form for `link` and
`monitor` ops only; user code does not spell `Pid[_]` in its
own signatures. See §*Open questions* #3 for why it is special.

Installing a link between the current actor and `pid` means:
if either actor crashes (unhandled effect or panic), the other
receives `Cancel.raise()`. Links are bidirectional by
construction; the scheduler maintains a set of linked peers
per actor and delivers cancellation on fault.

Use when two actors depend on each other symmetrically — a
worker and its job queue, two sides of a handshake. Do **not**
use link when the relationship is "observer watches observee"
— use a monitor instead.

### Monitors — unidirectional

```kai
effect Monitor {
  monitor(pid: Pid[_]) : MonitorRef
  demonitor(ref: MonitorRef) : Unit
}
```

`monitor(pid)` registers the current actor as an observer of
`pid`. When `pid` terminates (normal return, crash, or
cancellation), the observer receives a `MonitorDown` message
**on its own mailbox**:

```kai
type MonitorDown = MonitorDown(ref: MonitorRef, cause: TerminationCause)

type TerminationCause =
  | Normal
  | Crashed(msg: String)
  | Cancelled
```

### How MonitorDown is delivered

v1 uses the **embedded** model: `MonitorDown` travels through
the observer's normal mailbox, so the observer's message type
must include `MonitorDown` as one of its variants. A typical
supervising actor declares something like:

```kai
type SupervisorMsg =
  | Tick
  | Stop
  | Down(event: MonitorDown)            # supervision channel

fn supervisor(m: ActorCap[SupervisorMsg]) : Unit / Actor[SupervisorMsg] + Monitor + Cancel {
  forever {
    match m.receive() {
      Tick       -> ...
      Stop       -> break
      Down(ev)   -> handle_down(ev)
    }
  }
}
```

The coupling — every supervising message type must reserve a
variant for `MonitorDown` — is accepted in v1 because it is
the least intrusive option under the existing typed-mailbox
rule. A future revision may add a dual-channel model (system
messages on a separate queue, read by a dedicated op) once
usage data shows the embedded form is too tight; §*Out of
scope for v1* tracks that extension.

### `demonitor`

`demonitor(ref)` withdraws the subscription; no further
`MonitorDown` will arrive for that ref even if the target
terminates. `MonitorDown` events already deposited into the
observer's mailbox before `demonitor` are **not** flushed —
the observer receives them on its own cadence. A future
`demonitor_flush(ref)` helper may be added if the need
surfaces; for v1, callers drain pending `Down(...)` events
explicitly if it matters.

### Fault propagation

Monitors do **not** propagate faults. The observer learns
about the target's termination and decides what to do — retry,
restart, cascade — without being killed itself.

### Trap-exit semantics

By default a Link propagates termination as cancellation: when
either peer terminates (DONE or CANCELLED), the other receives
`Cancel.raise()` at its next yield point. That symmetry is the
right default for two halves of a handshake, but it makes a
supervisor pattern impossible — the moment the first child
crashes, the supervisor that should restart it is itself
cancelled.

`Spawn.set_trap_exit(on)` opts the current fiber out of that
default. With `trap_exit=true`, a linked peer's termination
delivers a `String` message into the fiber's mailbox instead of
setting `cancel_requested`:

| Peer terminated via | Message pushed |
|---|---|
| normal return (`KAI_FIBER_DONE`)         | `"Normal"`  |
| `Cancel.raise()` or `Spawn.cancel(...)` (`KAI_FIBER_CANCELLED`) | `"Crashed"` |

The fiber drains those messages with `Actor.receive()` exactly
like any other mailbox traffic. Two requirements for delivery:

1. The fiber must be inside a `with_mailbox { ... }` (or
   `with_mailbox_policy`) scope when its peer terminates — the
   runtime locates the mailbox through the fiber's most-recently-
   allocated mailbox slot. Without one, the propagation falls
   back to the default `cancel_requested` behaviour.
2. The fiber's message type must be `String` (or accept
   `String`) so the runtime's pre-built `"Normal"` /
   `"Crashed"` payloads round-trip through the typed mailbox.
   v1 ships String specifically — a richer
   `type ExitReason = Normal | Crashed(...)` payload waits on
   user-defined sum types crossing the runtime/library
   boundary.

Toggle scope: `set_trap_exit` affects propagations that happen
*after* the call returns. Links established before the call
still respect the flag — the read happens at termination time,
not at link time.

```kai
fn supervisor() : Unit / Actor[String] + Spawn + Console + Link + Cancel = {
  fiber_set_trap_exit(true)
  let me = Actor.self()
  let _ = fiber_spawn(() => with_mailbox { worker(me) })
  match Actor.receive() {
    "Normal"  -> Stdout.print("worker finished cleanly")
    "Crashed" -> { Stdout.print("worker crashed; restarting"); restart(me) }
    _         -> Stdout.print("unexpected message")
  }
}
```

Trap-exit is a per-fiber switch, not a per-link one: every link
the fiber holds at the moment of a peer's termination respects
the current `trap_exit` value. To revoke, call
`fiber_set_trap_exit(false)` and links revert to the default
cancel-cascade behaviour.

Trap-exit converts at the runtime boundary, before any user-level
`Cancel` handler can intercept. When a fiber is linked to a peer
with `trap_exit=true`, its `Cancel.raise()` bypasses any
`with Cancel { raise(_) -> ... }` handler that lives in the call
chain between the spawn point and the receive point — including
handlers that the child inherited from the parent's evidence chain
at spawn time. The fiber unwinds straight to the trampoline's
cancel pad, the link-propagation walk fires, and the trap-exit'd
peer learns of the termination through its mailbox. Without this
rule, OTP-style layered supervision would not compose: the moment
any intermediate frame between the supervisor's spawn and the
worker's `Cancel.raise()` installed an outer `Cancel` handler (for
cleanup, escalation, etc.), the supervisor's `Actor.receive()`
would never wake. Plain `Cancel.raise()` inside a fiber that holds
no trap-exit'd link still dispatches through user handlers as
before, preserving the cleanup-on-cancel idiom for non-supervised
work. Spec: `stage0/runtime.h` §`kai_check_trap_exit_cancel_bypass`,
issue #103.

### Supervision trees

A supervisor is an ordinary actor whose message type includes
`MonitorDown`; its `receive` loop handles child termination
explicitly:

```kai
type SupervisorMsg =
  | Down(event: MonitorDown)
  | Event(payload: DomainEvent)

fn supervisor(
  m:        ActorCap[SupervisorMsg],
  children: [ChildSpec]
) : Unit / Actor[SupervisorMsg] + Spawn + Monitor + Console + Cancel {
  let pids = children | (spec) => start_child(spec)
  pids |> each((p) => monitor(p))
  forever {
    match m.receive() {
      Down(MonitorDown(ref, Crashed(msg))) -> {
        Console.eprint("child crashed: #{msg}")
        let fresh = restart_child(ref_to_spec(ref))
        monitor(fresh)
      }
      Down(MonitorDown(ref, Normal))    -> remove_child(ref)
      Down(MonitorDown(ref, Cancelled)) -> remove_child(ref)
      Event(payload)                    -> handle_event(payload)
    }
  }
}
```

kaikai does not ship a prebuilt "supervisor" abstraction in
v1 — the pattern above is short enough to write by hand, and
the canonical Erlang OTP-style DSL (`one_for_one`,
`rest_for_one`, etc.) requires a larger design than v1 can
absorb. Once patterns stabilise, a stdlib module
`stdlib/supervisor.kai` can land as a layer on top of
`Monitor` + `Spawn`.

## Interaction with `Cancel`
<!-- coverage: skip --> covered by m8x_3_cancel_at_yield (cooperative Cancel delivery exercised end-to-end with a spawned worker)

This section names `Actor`'s ops by their canonical effect-level
form (`Actor.receive`, `Actor.send`, `Actor.self`). At call
sites with a binding `as m`, the same ops appear as
`m.receive()`, `m.send(...)`, `m.self()` — the underlying
semantics is identical.

`Actor.receive()` is a yield point. A fiber waiting on
`receive` can be cancelled by:

- `Spawn.cancel(pid)` from a peer.
- Its parent nursery closing with outstanding children.
- A linked actor crashing.

When cancelled during a `receive`, the scheduler delivers
`Cancel.raise()` at the receive site. The mailbox is dropped
(all undelivered messages are garbage-collected by Perceus);
if the actor has a `with Cancel { raise(_) -> cleanup }`
handler around its receive loop, the cleanup runs before the
fiber unwinds. Otherwise the fiber exits cleanly.

`Actor.send(pid, msg)` with a `BlockSender` mailbox is also a
yield point for the sender, not the receiver. A sender blocked
on a full bounded mailbox can be cancelled; on cancellation
the sender's `send` raises `Cancel` and unwinds, leaving the
message un-delivered.

`Actor.self()` is not a yield point (it is a pure lookup).

## Patterns

### Request / reply

The common synchronous-over-async pattern: send a request with
a reply-to pid, then wait for the response.

```kai
type Request = Query(question: String, reply_to: Pid[Reply])
type Reply   = Answer(String)

fn ask(m: ActorCap[Reply], server: Pid[Request], q: String) : String / Actor[Reply] + Cancel {
  m.send(server, Query(q, m.self()))
  match m.receive() {
    Answer(a) -> a
  }
}
```

`ask` takes the caller's mailbox capability explicitly — the
caller must already be inside an `Actor[Reply]` scope, typically
by being an actor itself, or by wrapping the call in
`with_mailbox { m -> ask(m, server, q) }` to open a short-lived
mailbox just for the interaction.

### Event bus

An actor whose mailbox collects events from many producers and
broadcasts them to subscribers:

```kai
type BusMsg =
  | Publish(topic: String, payload: String)
  | Subscribe(topic: String, subscriber: Pid[Event])
  | Unsubscribe(topic: String, subscriber: Pid[Event])

type Sub = Sub(topic: String, pid: Pid[Event])

fn event_bus(m: ActorCap[BusMsg]) : Unit / Actor[BusMsg] + Actor[Event] + Cancel {
  var subscribers: [Sub] = []
  forever {
    match m.receive() {
      Publish(topic, payload)  -> broadcast(m, @subscribers, topic, payload)
      Subscribe(topic, p)      -> subscribers := @subscribers ++ [Sub(topic, p)]
      Unsubscribe(topic, p)    -> subscribers := @subscribers |> filter((s) => !(s.topic == topic && s.pid == p))
    }
  }
}
```

(`var subscribers` + `@` / `:=` are m7b sugars; see
`docs/syntax-sugars.md`.)

### Actor pool

A supervisor that spins up N identical workers and
round-robins requests:

```kai
fn pool(
  nur:  Nursery,
  size: Int,
  work: (Task) -> Result / Cancel
) : Pid[Task] / Spawn + Monitor + Cancel {
  let workers = [1..size] | (_) => start_worker(nur, work)
  spawn_actor_default(nur) { m -> dispatcher_loop(m, workers) }
}
```

`pool` takes the caller's nursery, spawns `size` worker actors
inside it, and starts a dispatcher actor that round-robins
`Task` messages across them. The returned `Pid[Task]` is
region-branded to `nur`: it stays valid while the caller's
nursery is open and cannot outlive it.

`dispatcher_loop` is an ordinary actor body that receives
`Task`, picks the next worker, forwards, and monitors each
worker so a crash is observable via `MonitorDown`.

## Out of scope for v1

- **Distributed actors.** Pids are local; sending to a remote
  pid is not defined. A future `docs/distributed-actors.md`
  (phase 5+) adds node addressing, serialisation, and failure
  semantics.
- **Code upgrades ("hot code reload").** Erlang's killer
  feature; orthogonal to language design and requires a runtime
  with versioned module loading. Post-MVP.
- **Selective receive with preserved ordering.** v1's
  receive-and-requeue loop is O(n²); a real `receive_match(p)`
  primitive with a secondary save-queue is deferred.
- **Priority mailboxes.** No per-message priority. Callers
  that need it implement it with two mailboxes and their own
  arbitration actor.
- **Typed monitors.** `monitor` gives you `MonitorDown`; it
  does not parameterise the message type beyond that. Richer
  reason payloads wait on `Error[E]` from Doc B open questions.
- **Pre-built supervisor DSLs** (`one_for_one`, `one_for_all`,
  `rest_for_one`). The building blocks are there; the OTP-style
  DSL lands as a stdlib module after patterns stabilise.
- **Pid introspection beyond equality.** No `pid.name`,
  `pid.info`, `pid.mailbox_size`. Observability goes through
  monitors and runtime metrics (Doc C).
- **Dual-channel `MonitorDown` delivery.** v1 embeds
  `MonitorDown` into the observer's own mailbox, forcing the
  observer's message type to reserve a variant for it. A
  future revision may add a separate system-message channel
  (BEAM-style), consumed via a dedicated op, so regular
  message types stay free of supervision concerns.

## Open questions

1. **Mailbox policy default.**
   *Decided:* `spawn_actor` requires an explicit policy. The
   stdlib helper `spawn_actor_default(nur, body)` covers the
   common case using `Bounded(1024, BlockSender)` — bounded
   capacity with backpressure on full. This split keeps
   unbounded-by-accident out of the language while still
   giving callers a one-liner for the 90th-percentile case.

2. **Link vs monitor as the default supervision primitive.**
   *Decided:* monitors are the idiomatic default. Adding a
   monitored peer is observation only — the observer learns
   about termination via `MonitorDown` and decides what to do.
   Links are reserved for relationships that are symmetrically
   coupled (worker + job queue, two sides of a handshake). The
   reasoning: linking propagates faults automatically, which
   amplifies the blast radius of a crash; making the default
   the safer of the two options matches kaikai's "safety beats
   ergonomics" tie-breaker (CLAUDE.md Tier 1). Callers who
   want BEAM-style auto-cancellation reach for `link` once
   they have decided that the coupling is real.

3. **`Pid[_]` in link/monitor ops.**
   *Decided:* `Pid[_]` is a special form, accepted only as the
   parameter type of `link` / `monitor` / `demonitor` (and
   future supervision ops in the same family). User code
   cannot bind a value of type `Pid[_]`, store it in a record,
   or return it from a function. Existential pids elsewhere
   would let them leak past supervision, where the absence of
   a concrete `Msg` removes the typed-mailbox guarantee.

4. **`Actor[Msg]` as a row variable argument.**
   *Decided:* not in v1. A function that wants to run "inside
   any actor" cannot declare its `Actor[Msg]` row position as
   polymorphic over `Msg` — that would require row
   polymorphism at the call site, which Doc A §*Out of scope
   for v1* item 3 keeps out of scope (per-op type generics
   land in m7b, but row-level per-call polymorphism does not).
   Functions that genuinely need actor-agnostic behaviour can
   either avoid `Actor[Msg]` in their row (log via `Console`,
   not via the actor's mailbox) or open their own
   `with_mailbox` scope. Revisit if real use cases appear that
   neither workaround covers.

## Next steps

- **Stdlib module `stdlib/actor.kai`**: canonical
  implementations of `spawn_actor`, `spawn_actor_default`, and
  `with_mailbox` ship in m8 alongside the scheduler. An
  OTP-style supervisor DSL (`one_for_one`, `rest_for_one`,
  `one_for_all`) slips to m9+ — design surface is large and
  the right shape needs usage data from the first few stdlib
  actors, not pre-emptive specification.
- **`docs/distributed-actors.md`** — phase 5+. Node addressing,
  serialisation, failure detectors. Depends on a decision about
  which network runtime kaikai targets (Unix-only vs TCP/mTLS).
- **Doc C updates for mailbox layout**: runtime representation
  of mailboxes, lock-free vs per-actor-mutex, how `DropOldest`
  interacts with Perceus RC on messages in flight.

Implementation milestone is **m8** or later — actors depend on
fibers (`Spawn`) and cancellation (`Cancel`), both of which land
in m7's scheduler. Once the scheduler is in place, `Actor[Msg]`
and supervision primitives are comparatively small additions.
