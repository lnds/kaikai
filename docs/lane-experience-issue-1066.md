# Lane experience — issue #1066: `NetTcp.recv_timeout`

## Scope

Add a `recv_timeout` op to the `NetTcp` effect — a socket recv with a
deadline, the dual of the existing `Actor.receive_timeout`. Motivation:
`rongo`'s HTTPS server had no read timeout, so a slow/idle client could
park a fiber in `NetTcp.recv` forever (slow-loris DoS). `Spawn.select`
is a Phase-2 stub that parks on the head of the fiber array, so racing
`recv` against a timer waits on `recv` forever — the issue's whole point
is that "first-of-two" needs a single cancellable op, not a fiber race.

Scope as planned == scope as shipped: one op, its default handler, the
runtime dual-park in both runtime copies, one fixture, one retro.

## Result shape — the decision the issue left open

`recv` is `Result[String, [Int]]` (Err-first: `Ok([Int])` bytes,
`Err(String)`). The issue left the timeout shape to the lane. Three
outcomes must stay distinguishable: data (incl. clean EOF), transport
error, and timeout.

Chosen: **`Option[Result[String, [Int]]]`**.

- `None` — the deadline elapsed (dual of `mailbox_recv_timeout`, which
  returns `Option`).
- `Some(Ok([Int]))` — data, including `Some(Ok([]))` for a clean EOF,
  which stays distinct from timeout.
- `Some(Err(String))` — a transport error.

Why not a bespoke `type RecvResult = Received | Timeout | RecvError`:
the runtime constructs variants by numeric `variant_tag`, and a
user-declared sum's tags are assigned by the compiler's emit — the
runtime C cannot know them. `Some`/`None`/`Ok`/`Err` are the ONLY
constructors with canonical, runtime-known tags (`emit_shared.kai`
pins Some=0, None=1, Ok=2, Err=3). `Option[Result[...]]` reuses those
four and needs no new type, while still being three-way distinguishable.

## The runtime dual-park — reused, but adapted

`Actor.receive_timeout` dual-parks on a mailbox recv-waiter chain
(`KaiMboxNode`, its OWN node type) plus the timer wheel, so the fiber
sits in two structures at once and the win-side splices it out of the
other (the UAF the mailbox code guards).

A socket waiter cannot do that: every reactor list (timer wheel,
socket-read/write, pid, filepool, stdin, signal) threads through the
SINGLE intrusive `KaiFiber.reactor_next` slot — the struct comment
states "a parked fiber lives on exactly one reactor list at a time".
So the fiber can be on the socket-read list OR the timer wheel, never
both.

Design (validated against the alternatives): the fiber lives ONLY on
the socket-read-waiter list and carries its deadline in its own
`reactor_deadline_ns` field. Two changes to `kai_reactor_wait`:

1. `timeout_ms` takes `min(timer-head deadline, min socket-waiter
   deadline)`, so a socket deadline that precedes every timer still
   bounds the `poll()` sleep.
2. A `kai_reactor_socket_read_deadline_drain(now)` pass, unconditional
   after each poll (like the timer drain), wakes every socket-read
   waiter whose deadline elapsed, stamping `reactor_wait_status = 1`.

`kai_reactor_park_socket_read_timeout` reads that stamp on resume: 1
means the deadline won (return `None`), 0 means readiness won (retry
the non-blocking recv).

Two invariants this preserves:

- **No UAF.** The fiber is in ONE list, so whichever wakeup runs, the
  drain that unparks it has already spliced it out — there is no second
  structure to disarm. The single-list design REMOVES the mailbox's
  UAF hazard rather than reproducing it.
- **Data-just-in-time beats a coincident deadline.** The readiness
  drain runs BEFORE the deadline drain, so a socket that turned ready
  in the same poll as its deadline is already off the list and resolves
  as data, not timeout.

## Two runtime copies + the `.bc` forwarder

`stage0/runtime.h` and `stage2/runtime.h` both carry the handler and the
reactor changes (the two-runtimes trap). One extra site the C path does
not need: the native backend calls `kaix_default_nettcp_recv_timeout`, a
thin forwarder in `stage0/runtime_llvm.c` that bridges to the `kai_`
handler. Missing it surfaced as a native-only link error
(`Undefined symbols: _kaix_default_nettcp_recv_timeout`).

`runtime_llvm.c` is NOT a Make prerequisite of `runtime_llvm.bc`, so
`make kaic2` reported "up to date" after the edit — the `.bc` had to be
regenerated with `tools/gen-runtime-bc.sh` by hand (same trap #1067's
lane hit for `runtime.h`).

## Fixture + verification

`examples/effects/net_tcp_recv_timeout.kai` sequences one loopback pair
in a single fiber and exercises all three outcomes: data before the
deadline (`Some(Ok)`), an idle peer past the budget (`None` — the fiber
RETURNS instead of hanging, the DoS fix), and a clean close
(`Some(Ok([]))`, distinct from timeout). 20 ms budget: long enough that
a loopback send always lands first on the readable path, short enough
that the idle path returns promptly.

Verified: native + C byte-identical output; selfhost byte-id green
(a new runtime prim does not change the compiler's emitted C); tier0
green; backend parity green (`examples/effects`, serial).

## Follow-ups

- End-to-end test against rongo's server (the issue author offered).
- The 1 MiB recv ceiling and IPv4-only limits are inherited from `recv`,
  unchanged here.
