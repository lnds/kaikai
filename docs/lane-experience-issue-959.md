# Lane experience — issue #959: structured nursery (auto-join + cancel-on-fail)

## Scope as planned vs as shipped

**Planned.** Make `nursery` honour structured concurrency: join all
children spawned in its scope before returning (no explicit `await`
needed), and on a child failure cancel the surviving siblings and
re-propagate the cause. `nursery` was a typed pass-through
(`pub fn nursery[T,e](body) = body()`): children were enqueued but
never joined, so a nursery whose body only `spawn`-ed (no `await`)
returned before any child ran.

**Shipped.** Exactly that, both halves, on the default (native)
backend and on C, with no phasing. The two pieces:

1. **Auto-join.** A per-fiber stack of `KaiNursery` scopes. `Spawn`
   gains two internal ops — `scope_enter` (push a scope) and
   `scope_exit` (join + close). `Spawn.spawn` registers each child on
   the active scope via an intrusive `scope_sibling_next` link.
   `scope_exit` walks the children FIFO and parks on each until DONE
   or CANCELLED before the nursery returns.
2. **Cancel-on-fail.** When `scope_exit` finds a child that
   terminated CANCELLED *without* anyone requesting its cancellation,
   it cancels the surviving siblings, finishes the drain (waits their
   unwind), and re-raises out of the scope via the running fiber's
   `cancel_pad`. The root fiber has no pad, so a child crash at the
   program root is a banner + non-zero exit.

`stdlib/spawn.kai`'s `nursery` brackets `body()` with the two ops; its
row gains `Spawn`. No parser/desugar change — the cap-binding rewrite
(`n.spawn` → `Spawn.spawn`) already produces the thunk shape the new
signature accepts.

## Design decisions and alternatives considered

- **Scope tracking: per-fiber stack vs global stack vs generation
  snapshot.** A global "active nursery" stack breaks under nesting:
  when a spawned child later opens its own nursery, the global top has
  already moved on. A generation-counter snapshot (`birth_seq >= seq0
  && parent == me`) still needs a child list that does not exist
  today, and shares a fault-class with nurseries on the same parent
  fiber. The per-fiber `nursery_top` stack is the Trio model: it
  composes under arbitrary nesting (verified — a nursery whose body
  spawns fibers that each open their own nursery joins correctly) and
  reuses the op path both backends already know.

- **One combined `nursery(thunk)` op vs two `scope_enter`/`scope_exit`
  ops.** A single op would run `body()` *inside* the op handler, i.e.
  inside effect dispatch (`in_dispatch_node` re-entrancy). Two ops
  keep the body running outside dispatch, which is simpler and matches
  how the cap-binding rewrite already wants to thread the thunk.

- **What counts as a "child crash".** `panic` is `exit(1)` — not a
  scope-recoverable failure, by design. The recoverable failure is a
  child that raised `Cancel` on its own. The discriminant is the
  existing `cancel_requested` flag: a spontaneous `Cancel.raise()`
  leaves it 0 (failure → propagate); a requested cancel (`n.cancel`,
  or the cancel-on-fail walk) sets it 1 (expected → no propagation).
  Catching a modelled `Fail` at the nursery boundary is a separate
  follow-up — it reshapes the body's inferred row, so it is its own
  type-design lane.

## Structural surprises the brief did not anticipate

- **`signal_concurrent` regression caught the cancel-on-fail
  over-trigger.** The first cut treated *any* CANCELLED child as a
  failure. `demos/signal_concurrent` cancels a child deliberately
  (`Spawn.cancel(target)`) as its normal flow; that child terminates
  CANCELLED, and the naive rule aborted the nursery with "no
  survivors". The `cancel_requested` discriminant is what makes
  requested-cancel and spontaneous-crash distinguishable. tier0's
  demo gate is what surfaced it — without that demo the bug would
  have shipped.

- **Native needs three touch points, not one.** Beyond the static
  `kai_default_spawn_scope_*` handlers in `runtime.h` (both copies),
  the native backend resolves ops to `kaix_*` symbols, so
  `stage0/runtime_llvm.c` needs forwarders. The LLVM emitter already
  emits the `kaix_` reference by op name; only the C forwarder was
  missing (linker undefined-symbol until added).

## Fixtures added and coverage

- `examples/effects/m8x_9_nursery_autojoin.kai` (+ `.out.expected`) —
  two children, no explicit await, golden pins both children's
  effects before the nursery returns. Auto-join order is
  deterministic and identical on both backends.
- `examples/effects/m8x_10_nursery_cancel_on_fail.kai` — one child
  raises `Cancel`; target asserts the sibling never prints, "after
  nursery" never prints, and the banner lands on stderr.

Both wired into `test-effects`. Verified on native (default) and C,
plus ASAN-clean on auto-join / join / nested. The `kaikai-book`
`ejemplos/cap13/*.kai` all run correct on native; `02_nursery.kai`'s
prose ("the block waits for all children; if one fails the rest are
cancelled") is now true rather than aspirational.

Coverage gap: no fixture for cancel-on-fail re-raise *captured by an
enclosing nursery* (the nested-fail case was checked by hand and
propagates correctly, but the only golden'd failure case exits at the
root). A nested-capture fixture would need a `Cancel` handler around
the outer nursery to observe the re-raise without exiting — left for a
follow-up since the root case already proves the propagation path.

## Follow-ups

- `Fail`-at-nursery-level recovery (separate type-design lane; tracked
  in `docs/fibers-honesty-targets.md` Residual §2).
- `Spawn.cancel_all()` is in the cap-binding op list and the docs but
  has no `Spawn` op / handler yet — pre-existing, untouched here.
