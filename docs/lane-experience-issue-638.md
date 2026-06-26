# Lane experience — issue #638: `Actor.receive_timeout`

Goal: add `Actor.receive_timeout(d: Duration) : Option[Msg]` — receive
from the current actor's mailbox but give up after a deadline. Path 1
(owner-chosen): combine the two mechanisms the runtime already has — the
mailbox recv-waiter park and the reactor timer-wheel park — into one
dual-park that wakes on whichever fires first. Unblocks ahu's
`ask_timeout`, the kohau pool `acquire(timeout)`, and henua saga
deadlines.

Outcome: shipped, on BOTH backends (native default + C oracle), with a
real dual-wake (not a poll loop, not a fake race). The implementation was
straightforward as designed; the surprise was a latent native-dispatch
defect the feature surfaced — see below.

## The dual-park mechanism

A fiber blocked in `receive()` parks on the mailbox's `recv_waiter` chain
(linked through `awaiters_next`) and yields; a `send` from any fiber
dequeues the head waiter and unparks it. A fiber blocked in `sleep()`
parks on the reactor timer wheel (linked through `reactor_next`, ordered
by deadline) and the reactor drain unparks it when the deadline passes.

The two chains use *different* intrusive link fields on `KaiFiber`
(`awaiters_next` vs `reactor_next`), so a fiber can sit on BOTH at once.
`kai_mailbox_pop_timeout` does exactly that: enqueue on the recv-waiter
chain, then `kai_reactor_park_timer` (which inserts on the timer wheel and
yields). One `kai_sched_park`, two possible wakers.

The mailbox-non-empty fast path takes a queued message immediately without
arming the timer — `receive_timeout` on a ready mailbox is as cheap as
`receive`.

## The wake discriminant — and the use-after-free trap

On wake the fiber must determine WHICH event fired and, critically,
splice itself out of the OTHER structure. This is the one careful piece:
if it doesn't, a later event touches a fiber that already returned.

The discriminant is recv-waiter membership:

- A **send** unparks us by *dequeuing us from the recv-waiter chain* — we
  remain on the timer wheel. So after wake, `kai_mailbox_waiter_remove`
  finds us absent (returns 0) → message-wake → we call
  `kai_reactor_timer_remove` to disarm the still-armed deadline, then pop
  the message and return it.
- The **timer drain** unparks us by *splicing us off the wheel* — we
  remain on the recv-waiter chain. So `kai_mailbox_waiter_remove` finds us
  present (returns 1) → deadline-wake → we return `None`, and do NOT touch
  the timer (already drained).

The UAF this guards: without the recv-waiter removal on the deadline path,
a `send` arriving *after* the timeout would dequeue a fiber that has
already returned `None` and unparked-elsewhere — corrupting the freed
park. The `race_deadline` fixture exercises exactly this: a worker that
replies 50 ms after a 5 ms deadline, with a drain sleep so the late send
lands on the now-idle mailbox. ASAN + UBSan clean confirms the cleanup is
correct.

Accounting subtlety: `kai_reactor_park_timer` bumps
`kai_reactor_parked_count`; the deadline path decrements it in the drain,
the message path decrements it in `kai_reactor_timer_remove`. Both paths
balance the counter exactly once.

## Row composition — Clock stays out of the row (asu-validated)

The op's row is `/ Actor[Msg]`, NOT `/ Actor[Msg] + Clock`. An effect in a
row is an obligation on the caller: "someone above must install a
handler." `receive_timeout` imposes no such obligation — the deadline is
computed by the reactor's internal monotonic clock, which is part of the
fiber runtime, always present, not installable, not interceptable.
Declaring `Clock` would promise an interception point that does not exist.
This is the Erlang `receive ... after` shape: the timeout is part of the
primitive, served by the runtime, not a capability. (A future
mock-the-clock testing capability would be a separate, deliberate `Clock`
op — not a side effect of this one.)

## Where the Duration lives — op takes nanos, wrapper takes Duration

The `effect Actor[Msg]` is in `stdlib/effects/concurrent.kai`, which is
CORE (auto-loaded). `Duration` is in `stdlib/time.kai`, which is NOT core
(opt-in via `import time`), so `Duration` is invisible inside
`concurrent.kai`. Two options:

- (A) move `time.kai` into the core module set so the op can take
  `Duration` directly — inflates the core (Clock + all time arithmetic
  load unconditionally on every compile), violating "fast compilation."
- (B) the op takes `nanos: Int` (the natural runtime contract — the prim
  `mailbox_recv_timeout` already takes nanos), and the `Duration`-typed
  `receive_timeout(d)` wrapper lives in `stdlib/actor.kai` (which imports
  and may `import time`).

Chose (B). The decisive argument: a caller needs `Duration` constructors
(`millis(5)`, `seconds(10)`) from `time.kai` regardless, so they
`import time` anyway — moving `Duration` to core would not save the
import. (B) keeps the core graph untouched. The "two names" are not a
surface redundancy: the user only ever calls `actor::receive_timeout(d:
Duration)`; the nanos op is the internal mechanism, like the runtime prim.

## The blocker the brief did not anticipate: native dispatch assumes a uniform op-thunk layout per effect

The native backend dispatches an effect op to its clause through an Ev
blob field index. `nemit_perform` resolves the index by
`nfx_find_handler_by_eff(handlers, eff)` — the FIRST `KHandlerDecl` whose
`eff == "Actor"` — and the op's position in that handler's `op_thunks`
list. This silently assumes every handler of one effect has the SAME set
of ops in the same order.

That held before this lane: every `Actor` handler in `stdlib/actor.kai`
had exactly `{self, send, receive}`. When I added the `receive_timeout`
clause to `with_mailbox` / `with_mailbox_policy` but NOT to `spawn_actor`
/ `spawn_actor_policy` (the typer accepts a 3-clause handler because those
bodies never call `receive_timeout` — coverage is by demand), the two
handler shapes diverged: `with_mailbox` put `send` at field index 5
(4-op layout), but a perform resolving against the first Actor handler in
the global list could read index 4 (3-op layout). Result: `send`
dispatched to the wrong clause — "mailbox_send: argument is not a Pid" —
and a segfault on the timeout path. **C was unaffected** (it dispatches by
name, not by a per-handler field index), so this was the textbook
"works on C, breaks on native" trap.

Fix: add the `receive_timeout` clause to ALL FOUR Actor handlers, making
the layout uniform again. This is correct on its own merits — a spawned
actor can want `receive_timeout` too.

The underlying native-dispatch fragility is broader than this lane: ANY
user-written partial `Actor` handler (3 clauses, legal by coverage) would
reactivate the bug. That is a pre-existing latent defect in
`nemit_perform` / `nfx_find_handler_by_eff`, out of scope here, and worth
a separate issue (native op dispatch should key the field index off the
op's position in the EFFECT DECLARATION, not off whichever handler
`by_eff` happens to find first).

## How the race was tested both orderings

Four fixtures in `examples/actors/`, all wired into the `test-effects`
harness with `.out.expected` goldens, all passing native + C, all
ASAN/UBSan clean:

- `receive_timeout_none` — empty mailbox, 5 ms deadline → `None`. Single
  fiber: the reactor's own timer wakes the park. Proves the deadline path
  parks (doesn't spin).
- `receive_timeout_some` — message pre-queued → `Some` via the fast path,
  no timer armed.
- `receive_timeout_race_message` — worker replies 5 ms before a 200 ms
  deadline → message-wake → `Some`. Mirrors ahu's `ask_timeout` shape
  (embed `Actor.self()` in the request, then `receive_timeout`).
- `receive_timeout_race_deadline` — worker replies 50 ms after a 5 ms
  deadline → deadline-wake → `None`, and the late send lands on the idle
  mailbox without corruption (the UAF guard).

## Files touched

- `stdlib/effects/concurrent.kai` — `receive_timeout(nanos: Int)` op on
  `effect Actor[Msg]`.
- `stdlib/actor.kai` — clause on all four handlers; `Duration`-typed
  `receive_timeout` wrapper; `import time`.
- `stage2/runtime.h` + `stage0/runtime.h` (change together) —
  `kai_mailbox_pop_timeout`, `kai_mailbox_waiter_remove`,
  `kai_reactor_timer_remove`, `kai_prelude_mailbox_recv_timeout`.
- `stage0/runtime_llvm.c` — `kaix_prelude_mailbox_recv_timeout` shim.
- `stage2/compiler/{infer,resolve,emit_c}.kai` — register the
  `mailbox_recv_timeout` prim (type, prelude name, EP forwarder).

## Follow-ups

- Native op dispatch keying off effect-declaration order rather than
  first-matching-handler order (the latent defect above).
- ahu adopts `ask_timeout` as a one-line change over `ask` (its job, not
  this lane's).
