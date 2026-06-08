# Lane experience — issue #763: `spawn_actor_policy`

**Lane:** `spawn-actor-policy` · **Issue:** #763 · **Date:** 2026-06-07

## Scope as planned vs. as shipped

Planned (from the v1-simplification note at `stdlib/actor.kai:95-98`
and the issue): a stdlib wrapper `spawn_actor_policy(policy, body)`
mirroring `spawn_actor` but threading an explicit `MailboxPolicy`,
unblocked by downstream demand (ahu's `backpressured_etl` example).

Shipped exactly that, plus one runtime primitive the brief did not
anticipate (see *Structural surprises*): `kai_mailbox_alloc_bounded_unowned`
in both runtimes, its prelude wrapper, the `kaix_` LLVM forwarder,
and the four stage2 registration points. Two fixtures, both backends
verified, catalog docs updated. No scope dropped.

## Design decisions

1. **New runtime primitive over reusing `mailbox_alloc_bounded`.**
   `kai_mailbox_alloc_bounded` stamps `owner_fiber = kai_current_fiber()`
   AND overwrites the parent fiber's `mailbox` slot. `mailbox_assign_owner`
   re-points the mailbox at the spawned fiber but cannot restore the
   parent's slot (it never saw the old value). A parent already inside
   `with_mailbox` would have had its monitor / link / trap-exit lookups
   silently corrupted. The unowned variant is 8 lines and symmetric
   with the existing `kai_mailbox_alloc_unowned`; alternatives
   (save/restore the parent slot in stdlib, or making `assign_owner`
   restore it) would have encoded parent-slot bookkeeping into the
   protocol for no gain.
2. **`alloc_unowned_for_policy` helper mirrors `alloc_for_policy`.**
   Same match shape, same `Pid[Nothing]` return. Keeps the two
   policy-dispatch sites visually parallel and the spawn functions
   single-purpose.
3. **Stdlib shape `spawn_actor_policy(policy, body)`, not the spec's
   `spawn_actor(n, policy, body)`.** `docs/actors.md` specifies a
   nursery parameter + `ActorCap` as-binding for the final surface;
   that is a surface-alignment lane of its own. This lane ships the
   exact follow-up the v1 note promised. `spawn_actor_default` also
   stays open (noted in the issue as out of scope).

## Structural surprises

1. **The fiber-escape allow-list.** First compile failed with
   `Pid[Msg] cannot escape 'alloc_unowned_for_policy''s
   structured-concurrency scope`. The m8 #6 shallow region-brand
   check rejects any user fn returning `Fiber[T]`/`Pid[Msg]` unless
   the fn is in `fiber_producer_helpers()` (`infer.kai` ~15013) — an
   explicit allow-list standing in for the unlanded `TyBranded`
   machinery. `alloc_for_policy` and `spawn_actor` were already
   listed; the two new helpers had to join. Cost: one rebuild cycle.
   Future stdlib lanes adding Pid/Fiber-returning helpers will hit
   the same wall — the allow-list comment does say so, but you only
   find the comment after the diagnostic sends you looking.
2. **Two runtimes to patch, not one.** `stage2/runtime.h` is a
   divergent copy (Koka tagged-Int + reuse-token Perceus) of
   `stage0/runtime.h`, and kaic2-emitted C resolves `#include
   "runtime.h"` to the stage2 copy. The primitive and its prelude
   wrapper had to land in both, with different Int-unboxing idioms
   (`cap->tag == KAI_INT` vs `kai_is_int(cap)`). Grep for an existing
   mailbox symbol across both files before assuming one edit suffices.
3. **Doc drift found in passing.** `docs/actors.md` §Next steps
   claimed `stdlib/actor.kai` exposes `spawn_actor_default` — it never
   has (the spec defines it; the stdlib never shipped it). Fixed the
   sentence to list the real surface and flag the open spec gap
   rather than opening a drift issue for a one-line fix inside the
   lane's own blast radius.

## Fixtures added

- `examples/effects/issue_763_spawn_actor_policy_block_sender.kai` —
  the ahu shape: producer into a capacity-1 `BlockSender` consumer
  mailbox; the output interleaving ("producer: send 3" *after*
  "consumer: got msg1") is the observable proof of sender parking.
- `examples/effects/issue_763_spawn_actor_policy_drop_oldest.kai` —
  `Bounded(2, DropOldest)`, four sends before the consumer runs;
  consumer receives msg3/msg4, proving the policy reached the
  *spawned* actor's mailbox (the unbounded default would deliver
  msg1/msg2).

Both wired into `stage2/Makefile` `test-effects` following the
per-fixture block precedent. Both verified under the C and LLVM
backends with identical output. Coverage gap left open: no fixture
spawns an actor with `DropNewest` (the runtime path is shared with
DropOldest up to the drop choice, and `with_mailbox_policy` coverage
exists; a dedicated fixture felt redundant — flagging it here in case
a later regression proves that judgment wrong).

## Cost vs. estimate

Roughly a half-day lane as briefed; landed in well under that. The
only unplanned cost was the escape-check rebuild cycle (~3 min of
kaic2 rebuild) and the stage2/runtime.h discovery, both absorbed
within the session.

## Follow-ups left

- `spawn_actor_default` (`Bounded(1024, BlockSender)` default) — spec'd
  in `docs/actors.md`, still unshipped; natural next lane once usage
  data justifies it.
- Surface alignment with the spec signature (nursery parameter,
  `ActorCap` as-binding) — large, deliberate lane; see `docs/actors.md`
  §Spawning actors.
- The fiber-escape allow-list keeps growing by hand; the real fix is
  the `TyBranded` region-brand machinery
  (`docs/fibers-honesty-targets.md` §Residual m8.x items).
