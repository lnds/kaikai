# m8.x — concurrency runtime follow-up

Tracks every item m8 (`stage2-design.md` §m8) deferred to a
follow-up milestone. The common thread is **real fiber suspension**:
the m8 v1 runtime is *inline-eager* (every spawned thunk runs to
completion synchronously inside `Spawn.spawn`), so any semantics
that needs a fiber to be suspended mid-execution requires either
ucontext-style stack switching or full CPS reification — neither
of which lands in m8.

The user-facing **type surface** of every Spawn / Cancel / Actor /
Link / Monitor op already matches what m8.x will deliver. The
runtime swap should be invisible to user code.

## Deferred items

### 1. Real cooperative scheduler

Replace the inline-eager defaults of `Spawn.spawn` / `Spawn.await` /
`Spawn.select` (`stage0/runtime.h` §m8 #2/#3) with a scheduler that
allocates a private stack per fiber and uses `swapcontext` /
`makecontext` (POSIX) — or a full CPS reification of the rest of
the caller's body — to suspend the current fiber at every effect-
op call site.

Acceptance criteria:
- `n.spawn { f() }` returns a `Fiber[T]` whose body has *not yet
  run* when control returns to the parent.
- `n.await(fib)` blocks the calling fiber until `fib`'s thunk
  completes, then resumes with the result.
- A timer fiber + a worker fiber actually interleave under
  `n.select([...])`.

Substrate decision pinned at start of m8.x: ucontext on POSIX
(macOS + Linux x86_64 + aarch64), or full CPS reification through
the existing `KaiCont` infrastructure. The CPS path requires
extending `kai_cont_init_identity` to capture the *rest* of the
caller's body as a heap closure (see Doc C §*The CPS transform*
§*One-shot case*); right now every op-call site uses the identity
continuation and resume is effectively a tail return.

### 2. Cooperative `Cancel` delivery at yield points

Once the scheduler can suspend, every yield point (every effect-op
call site, plus `Spawn.yield()`) checks the current fiber's
`cancel_requested` flag. If set, the dispatcher injects
`Cancel.raise()` *instead of* running the op. The KaiFiber
`cancel_delivered` flag prevents repeated injection.

Lives in `stage0/runtime.h` op-dispatch prologue (or equivalently
in compiler-emitted code at every `kai_evidence_lookup_node` site).

Spec: Doc B §`Cancel`/Delivery points; `docs/structured-concurrency.md`
§Cancellation.

### 3. `Bounded(_, BlockSender)` mailbox policy

`KAI_OVERFLOW_BLOCK_SENDER` (3) currently errors at allocation in
`kai_mailbox_alloc_bounded`. The full semantics: a `send` to a full
bounded mailbox parks the calling fiber on a per-mailbox waiter
queue; receivers wake the head of the queue when they pop a slot.
A waiting sender is itself a yield point, so `Cancel.raise()` can
unwind it (per actors.md §`Bounded(capacity, on_full)`).

Depends on item 1.

### 4. Blocking `Actor.receive()` on empty mailbox

Today `kai_mailbox_pop` errors when the mailbox is empty (v1 inline-
eager has no way to suspend). m8.x makes it park the calling fiber
on the mailbox's waiter list; senders wake the head when they push.
Same machinery as item 3 in reverse.

Spec: Doc actors.md §`Actor[Msg]` *receive()*.

### 5. Cross-fiber `Link` / `Monitor` runtime registry

Today `Link` and `Monitor` are typer-side declarations only
(`stage2/compiler.kai` §m8 #9 builtin_*_decl). m8.x adds:

- Per-fiber linked-set and monitor-set, populated by the op clauses.
- On fiber termination (normal / crash / cancel), walk the linked
  set → set `cancel_requested` on each peer; walk the monitor set
  → push `MonitorDown(ref, cause)` to each observer's mailbox.
- A user-installable default handler for `Link` and `Monitor`
  (vs the type-only stubs today).

Spec: Doc actors.md §*Supervision: links and monitors*.

### 6. Full region-brand machinery for `Fiber[T]` / `Pid[Msg]`

m8 #6 ships a *syntactic* approximation: any user fn (not in
`is_fiber_producer_helper`) declaring `Fiber[T]` in its return
type is rejected. The full Doc B / structured-concurrency.md
§*Type system* design tags every Fiber / Pid value with the brand
id of its enclosing nursery scope and tracks the brand through
let-bindings, pattern-matches, list literals, etc., rejecting any
value whose brand has expired against the current scope.

This is closer to lifetime checking than to HM unification.
Implementation likely: extend `Ty` with `TyBranded(Ty, BrandId)`,
extend the inferencer to mint brands at handler-installation
sites, add a brand-escape check at fn return + fn arg + record
field positions.

### 7. Per-op type generics (m7b #2)

Out of m8's stated scope but materially blocks the cleanup of
m8 #3's type-erased Spawn ops. With m7b #2 in place, the Spawn
declaration becomes:

```
effect Spawn {
  spawn[T, e](f: () -> T / e) : Fiber[T]
  await[T](f: Fiber[T])       : T
  ...
}
```

— matching Doc B verbatim — and `stdlib/spawn.kai`'s wrappers
collapse to the canonical `n.spawn { ... }` cap-binding form once
m7b #4 (`@cap` / `cap := v`) also lands. Doc C §*Per-op type
generics* §*Implementation plan (m7b)* has the work plan.

## Bugs surfaced in m8 (also m8.x scope)

Tracked in `~/.claude/projects/.../memory/project_stage0_bugs.md`.
The ones that bit m8 demos and need closing before m8.x can ship
proper structured-concurrency demos:

- **#9 Closure-capture-through-let in trailing lambdas.** Workaround
  in m8 fixtures: inline expressions, no inner `let`. Fix: extend
  `collect_free_vars` to subtract `SLet` introductions.
- **#7 Closure-capture-through-pattern-bind.** Same shape but for
  `match` arm bindings. Workaround: hoist match handlers to top
  level. Same fix family as #9.
- **#12 User-installed `with Spawn` delegating to outer Spawn
  segfaults.** Currently sidestepped by making `nursery` a typed
  pass-through. Likely interaction between TyAny-erased Spawn ops
  and the m7a #6 CPS clause-body machinery; investigate.
- **Lambda-row inference with "concrete + open row var".** Both
  `nursery` and `with_mailbox` had to relax their body-arg row to
  pure-open-var because m7b #14 doesn't pick up effects when a
  concrete label sits next to the var. Tracked as a separate
  m7b #14 follow-up; needed by m8.x to restore the proper Doc B
  signatures.
- **Body-vs-declared row check missing Spawn through stdlib.**
  `fn main() : Int / Console` was accepted for a body calling
  `fiber_spawn` (which has `/Spawn` in its row); runtime crashed
  on missing Spawn evidence. Workaround in m8_9 fixture: declare
  `/ Console + Spawn` explicitly. Likely a typer bug in how
  imported-helper rows propagate.

## Sequencing

m8.x is one milestone, but its items split into two waves:

**Wave A — substrate.** Item 1 (real scheduler) is load-bearing for
items 2/3/4. Pick the substrate (ucontext vs CPS) first. ~2-4
weeks of focused work.

**Wave B — semantics on top.** Items 2 (Cancel delivery), 3
(BlockSender), 4 (blocking receive), 5 (Link/Monitor registry) all
land cleanly once Wave A is in. Item 6 (region brand) and item 7
(per-op generics) are typer work and can land in parallel with
either wave.

Bugs (m7b #14 follow-ups, closure-capture fixes) should land
*before* Wave A so the demos exercising the new scheduler can use
the full target syntax without workarounds.

## Related

- `docs/effects-impl.md` §*Interaction with fibers (m8 preview)* —
  the contract Doc C committed to.
- `docs/structured-concurrency.md` §*Implementation notes* — what
  the scheduler is supposed to do.
- `docs/actors.md` §*Out of scope for v1* — overlaps with this
  list at items 5 (selective receive — out for v1, distinct from
  m8.x's unblock-on-arrival receive in item 4).
