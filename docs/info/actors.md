# actors

Message-passing concurrency built on fibers — `Actor[Msg]` effect
with `send` / `receive` / `self` ops.

## Description

Actors are fibers with a private mailbox parameterised by message
type. `Actor[Msg]` is an effect; its ops are reached via the plain
`Actor.` prefix (the `[Msg]` parameter lives in the handler binding,
not in the op call site).

Each actor has a private heap (BEAM-style isolation). Messages are
COPIED across the boundary; references do not alias.

Mailbox is unbounded by default; an explicit `MailboxPolicy`
(`Unbounded` or `Bounded(cap, DropOldest | DropNewest | BlockSender)`)
can be set per actor — `BlockSender` parks the sender on a full
mailbox (backpressure). `receive()` is selective: future
`receive_match { }` selects on pattern (deferred to Orongo).

## Example

```kaikai
import actor

type Msg = Inc | Dec | Stop

fn loop_body() : Unit / Actor[Msg] + Stdout = {
  let m = Actor.receive()
  match m {
    Inc  -> { Stdout.print("inc");  loop_body() }
    Dec  -> { Stdout.print("dec");  loop_body() }
    Stop -> Stdout.print("stop")
  }
}

fn main() : Int / Spawn + Stdout = with_mailbox {
  Actor.send(Actor.self(), Inc)
  Actor.send(Actor.self(), Dec)
  Actor.send(Actor.self(), Stop)
  loop_body()
  0
}
```

`with_mailbox` is the stdlib helper that installs an `Actor[Msg]`
handler over the current fiber (see `stdlib/actor.kai`). The full
spawn/install surface:

- `with_mailbox(body)` / `with_mailbox_policy(policy, body)` —
  mailbox for the *current* fiber.
- `spawn_actor(body)` / `spawn_actor_policy(policy, body)` — spawn
  a fresh actor fiber (returns its `Pid[Msg]`); the `_policy` form
  sets an explicit `MailboxPolicy` on the spawned mailbox, e.g.
  `spawn_actor_policy(Bounded(1, BlockSender), () => consumer())`
  for sender-side backpressure.

## Monitoring

`Monitor` lets one actor observe another's lifecycle. See
`docs/actors.md` for the full surface.

## Parallelism

Actors run in parallel across OS threads with `KAI_THREADS=N`
(default 1; no code changes — see `kai info fibers` §*Parallelism*).
The actor guarantee holds at any N: one mailbox, processed serially;
messages that cross a thread boundary are copied, so no actor ever
observes shared mutation.

## NOT IN KAIKAI

- Erlang `unlink`. The `Link` effect exists with a `Link.link(peer)`
  op, but bidirectional crash propagation is m8.x scope and there is
  no `unlink` op. For supervision today, use Monitor + handler.
- Process registry / named actors as primitive. Use a stdlib
  helper that wraps `Actor[Msg]` handles.
- Distributed actors. Single-node only.
- `receive_match { ... }` selective receive. Deferred to Orongo.

## See also

`kai info fibers`, `kai info effects`, `docs/actors.md`
