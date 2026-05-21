ACTORS(7)                       kaikai                       ACTORS(7)

NAME
  actors — message-passing concurrency built on fibers

SYNOPSIS
  Actor.send(pid, msg)                        # send a message
  Actor.receive()                             # park fiber until a message
  Actor.self()                                # this actor's pid
  spawn_actor[Msg](body)                      # spawn + return Pid[Msg]

DESCRIPTION
  Actors are fibers with a private mailbox parameterised by message
  type. `Actor[Msg]` is an effect; `send` / `receive` / `self` are
  its ops. The capability is reached via the plain `Actor.` prefix
  (the `[Msg]` parameter lives in the handler binding, not in the op
  call site).

  Each actor has a private heap (BEAM-style isolation). Messages are
  COPIED across the boundary; references do not alias.

  Mailbox is unbounded by default. `receive()` is selective: future
  `receive_match { }` selects on pattern (deferred to Orongo).

EXAMPLE
  See `demos/vs/elixir/main.kai` and `demos/vs/rust/main.kai` for a
  full working actor demo. The shape is:

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
      Actor.send(Actor.self(), Stop)
      loop_body()
      0
    }

  `with_mailbox` is the stdlib helper that installs an `Actor[Msg]`
  handler over the current fiber (see `stdlib/actor.kai`).

MONITORING
  `Monitor` lets one actor observe another's lifecycle (shipped
  2026-04-30, see `docs/actors.md`).

NOT IN KAIKAI
  - Erlang link/unlink semantics. Use Monitor + a supervisor pattern.
  - Process registry / named actors as primitive. Use a stdlib
    helper that wraps Actor[Msg] handles.
  - Distributed actors. Single-node only.
  - `receive_match { ... }` selective receive. Deferred to Orongo.
  - Mailbox bounds / backpressure as primitive. Implement in the
    actor's `receive` loop.

SEE ALSO
  kai info fibers, kai info effects, docs/actors.md
