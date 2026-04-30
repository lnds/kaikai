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

**Scope decision** (2026-04-29): see `docs/fibers-honesty-targets.md`
for which items below belong to Show-HN-honest, Production-honest
1.0, and post-MVP tiers. This file is the operational inventory; the
honesty-target file is the strategic scope.

## Status (R2 lane)

R2 (`fiber-scheduler` branch) lands the m8.x runtime in five
phases. Items 1-5 below are now ✅ landed; item 6 ships v1 (shallow
check) with the full machinery deferred; item 7 is independent
typer work. See `docs/fibers-impl.md` §*Phasing* for the
phase-by-phase landing table.

## Deferred items

### 1. Real cooperative scheduler — ✅ Phase 2

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

### 2. Cooperative `Cancel` delivery at yield points — ✅ Phase 3

Once the scheduler can suspend, every yield point (every effect-op
call site, plus `Spawn.yield()`) checks the current fiber's
`cancel_requested` flag. If set, the dispatcher injects
`Cancel.raise()` *instead of* running the op. The KaiFiber
`cancel_delivered` flag prevents repeated injection.

Lives in `stage0/runtime.h` op-dispatch prologue (or equivalently
in compiler-emitted code at every `kai_evidence_lookup_node` site).

Spec: Doc B §`Cancel`/Delivery points; `docs/structured-concurrency.md`
§Cancellation.

### 3. `Bounded(_, BlockSender)` mailbox policy — ✅ Phase 4

`KAI_OVERFLOW_BLOCK_SENDER` (3) currently errors at allocation in
`kai_mailbox_alloc_bounded`. The full semantics: a `send` to a full
bounded mailbox parks the calling fiber on a per-mailbox waiter
queue; receivers wake the head of the queue when they pop a slot.
A waiting sender is itself a yield point, so `Cancel.raise()` can
unwind it (per actors.md §`Bounded(capacity, on_full)`).

Depends on item 1.

### 4. Blocking `Actor.receive()` on empty mailbox — ✅ Phase 4

Today `kai_mailbox_pop` errors when the mailbox is empty (v1 inline-
eager has no way to suspend). m8.x makes it park the calling fiber
on the mailbox's waiter list; senders wake the head when they push.
Same machinery as item 3 in reverse.

Spec: Doc actors.md §`Actor[Msg]` *receive()*.

### 5. Cross-fiber `Link` / `Monitor` runtime registry — ⚠ partial Phase 5

**Link**: ✅ landed. KaiMailbox.owner_fiber, KaiFiber.linked_head
(intrusive list), `kai_link_add_bidirectional` /
`kai_link_propagate_terminate` helpers, EvLink + `kai_default_link_link`,
`default_link_setup` wired into compiler.kai's main installation.
Trampoline DONE/CANCELLED branches walk the linked chain and set
`cancel_requested` on each peer.

**Monitor**: ⏳ deferred to Phase 5.5+. Pid handoff for a clean
two-fiber `Monitor.monitor` demo needs `spawn_actor` (m8.x #6
follow-on) or message types carrying Pid; the v1 actor surface
(with_mailbox only) supports neither cleanly. The runtime registry
shape (per-fiber monitor-set, MonitorDown push at termination)
will mirror Link's; the work is small once the demo path exists.

**Trap-exit semantics**: ✅ landed 2026-04-29 (Fibers Tier 2 lane).
`Spawn.set_trap_exit(Bool)` toggles the current fiber's
`trap_exit` flag (KaiFiber struct, stage0/runtime.h). The Link
propagation walk now reads `peer.trap_exit`: when set AND the
peer has a current mailbox (per `kai_mailbox_alloc[_bounded]`
back-pointer), `kai_link_propagate_terminate` pushes a String
into the peer's mailbox — `"Normal"` if the dying fiber's state
is `KAI_FIBER_DONE`, `"Crashed"` if `KAI_FIBER_CANCELLED` —
instead of setting `cancel_requested`. With trap_exit=0 the
behaviour is unchanged. Coverage:
`examples/effects/m8_trap_exit.kai` (end-to-end through the
Spawn effect) + `stage2/tests/link_runtime_test.c` (C-level
unit test exercising both reasons + the no-mailbox fallback).

Spec: Doc actors.md §*Supervision: links and monitors*.

### 6. Full region-brand machinery for `Fiber[T]` / `Pid[Msg]` — ⚠ v1 shallow

**Phase 6 v1** ships the existing shallow check
(`ty_expr_contains_fiber` recursive walk through TyName / TyList /
TyFn / TyDim / TyRefine looking for `Fiber`) plus a generalised
allow-list (`fiber_producer_helpers` in `stage2/compiler.kai`).
Regression test `examples/effects/m8x_6_fiber_in_result.kai`
covers the parametric-sum-type case (Fiber[T] embedded in
`Result[String, Fiber[Int]]`).

**Deferred to post-MVP**: the full machinery from
`docs/structured-concurrency.md` §*Type system* — extend `Ty`
with `TyBranded(Ty, BrandId)`, mint brands at handler-installation
sites, propagate the brand through every binding form (let, match,
list literal, record field, etc.), check escape at fn return /
fn arg / record field. Closer to lifetime checking than to HM
unification.

**Known shallow-check gaps** (not caught by v1):
- Custom sum types with Fiber in a constructor payload but no
  `Fiber` in the type's name or args (e.g. `type Boxed = Wrapper(Fiber[Int])`
  — return type `Boxed` hides Fiber from the walker).
- `Pid[Msg]` escape checks (Pid would need the same brand
  machinery; v1 only checks Fiber).
- Brand mismatch between sibling nurseries (only the lexical-
  walking version detects nursery scope, not the shallow check).

### 7. Per-op type generics on the Spawn ops — ⚠ partial (Tier 2 lane)

Out of m8's stated scope but materially blocks the cleanup of
m8 #3's type-erased Spawn ops. With m7b #2a in place (per-op TYPE
generics), the four Fiber-shaped ops now carry `[T]`:

```
effect Spawn {
  spawn[T] (thunk: Nothing)        : Fiber[T]
  await[T] (fiber: Fiber[T])       : T
  select[T](fibers: [Fiber[T]])    : T
  cancel[T](fiber: Fiber[T])       : Unit
  ...
}
```

`stdlib/spawn.kai`'s `fiber_await` / `fiber_select` / `fiber_cancel`
are now one-line aliases; the canonical `Spawn.await(f) : T` shape
is reachable directly. Coverage:
`examples/effects/m8_spawn_per_op_generics.kai`. Landed in the
Fibers Tier 2 lane (2026-04-30).

**Still pending — per-op ROW generics.** The full Doc B shape is

```
effect Spawn {
  spawn[T, e](f: () -> T / e) : Fiber[T]
  ...
}
```

— `e` is a per-op *row* variable propagating the spawned thunk's
row into the caller's row. `add_effect_op_sigs_loop` only allocates
`mk_tpbinds_from` (TYPE binds) for `op_tparams`; there is no
op-level `mk_rvbinds`. Until that lands, the spawn op keeps
`thunk: Nothing` (TyAny) so the wrapper
`fiber_spawn[T, e](f: () -> T / e) = Spawn.spawn(f)` can absorb the
open row through TyAny. After per-op row generics
land, the wrappers reduce to one-line aliases (or are removed) and
`Spawn.spawn[T, e]` becomes the canonical entry point. Doc C
§*Per-op type generics* §*Implementation plan (m7b)* has the work
plan; the additional row-bind plumbing is the small step that
remains.

## Bugs surfaced in m8 (also m8.x scope)

Tracked in `~/.claude/projects/.../memory/project_stage0_bugs.md`.
The ones that bit m8 demos and need closing before m8.x can ship
proper structured-concurrency demos:

- ~~**#9 Closure-capture-through-let in trailing lambdas.**~~
  **Closed 2026-04-29** (`f692a5c`). `stage1/compiler.kai`
  gained scope-aware `fv_stmts_scoped` mirroring stage 2's
  shape; each `SLet(pat, _, rhs)` walks `rhs` under the outer
  scope and extends the scope with the pattern's bound names
  for the next stmt. `EBlock` threads the scoped result's
  `own` through to the trailing tail. Stage 0 had the same
  fix already (`N_LET` walks RHS first then `pat_add_locals`).
- ~~**#7 Closure-capture-through-pattern-bind.**~~ **Closed
  2026-04-29** (`f692a5c`). `stage1/compiler.kai`'s
  `fv_arms` now extends `own` with `pat_bindings(pat, own)`
  before walking the guard and body, matching stage 2's
  behaviour and stage 0's `N_ARM` `ls_push_mark` /
  `pat_add_locals` / `ls_pop_mark`.
- ~~**#12 User-installed `with Spawn` delegating to outer Spawn
  segfaults.**~~ **Closed 2026-04-29** (`4a77d49`). Root cause
  was the runtime's `kai_evidence_lookup_node` always
  returning the innermost `Spawn` handler, including the one
  whose clause body was currently being dispatched —
  `Spawn.spawn(thunk)` inside the clause re-resolved to the
  same handler, infinite-looping until the stack overflowed.
  Fix: a new `int in_dispatch` field on `KaiEvidence`, set to
  1 right before invoking the clause function pointer (after
  args are bound to `_op_arg_<i>` C locals so inner ops still
  see the live node) and cleared on the way out. Both
  `lookup_node` and `lookup_node_by_id` skip flagged nodes,
  giving the user-installed handler an Effekt-style "outer
  handler" view from inside its own clause. The LLVM op
  dispatch path does not yet honour the flag (it never built
  the discard/longjmp contract); pinned as a Wave A follow-up
  whenever LLVM grows the discard-resume shape.
- ~~**Lambda-row inference with "concrete + open row var".**~~
  **Closed 2026-04-29** (`9de0449`). The bug was already
  fixed by the cumulative typer work landed between m8 #10
  and 2026-04 (closure-capture handler-id sentinel +
  nested-lambda free-var propagation in particular).
  `with_mailbox` regained its strict
  `body: () -> R / Actor[Msg] + e` signature; selfhost stays
  byte-identical; m7b #14 follow-up no longer needs a
  separate fix.
- **Body-vs-declared row check missing Spawn through stdlib.**
  `fn main() : Int / Console` was accepted for a body calling
  `fiber_spawn` (which has `/Spawn` in its row); runtime crashed
  on missing Spawn evidence. Workaround in m8_9 fixture: declare
  `/ Console + Spawn` explicitly.

  **Root cause located 2026-04-29.** The synth_call ordinary-fn
  path constructs `expected = TyFnT(args, ret, Row{labels:[],
  tail:fresh})` and unifies callee against expected. The fresh-
  tail absorbs the callee's labels through the substitution but
  the caller's `st.row` is **never updated**. `st_add_label` is
  invoked only by `try_op_call` (explicit `Eff.op(...)`); plain
  fn calls leak the row. The dead helper `st_add_labels` (with
  the apt comment "Used by `synth_call` to propagate every
  label the callee declared into the caller's row") was the
  intended call site that never got wired.

  **Three-line fix** at the end of `synth_call`'s ordinary path:
  apply the substitution to the callee type, pull the resolved
  row's labels, and fold them through `st_add_labels`. Selfhost
  break is real but contained — 57 stage 2 helpers (parser
  diagnostics, dump_*, p_error, …) currently sub-declare their
  rows and rely on this leak. Each needs `/ Console` (or
  `/ Console + File` for the disk-touching subset) appended to
  its signature, then the cascade re-runs against transitive
  callers. Estimated ~1 day of careful editing across the
  codebase, plus selfhost validation between every batch.

  Tracked separately because the deployment is a lane, not a
  bug fix; the row tracking gap, however, **is** a real bug
  and a Tier 1 violation (effects in types must match what the
  body performs).

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
