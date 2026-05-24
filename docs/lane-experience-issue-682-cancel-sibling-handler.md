# Lane retro — issue #682: Spawn.cancel from sibling runs user Cancel handler

**Date**: 2026-05-24
**Branch**: `fix-issue-682`
**Scope-as-planned**: investigate, then fix the runtime so a user-installed
`with Cancel { raise(_) -> cleanup }` handler in the target fiber fires when
cancellation arrives from a sibling fiber via `Spawn.cancel(target)`. Fall
back to a doc-only fix if the runtime work proved out of scope.

**Scope-as-shipped**: runtime fix (chosen path), one fixture, doc update,
this retro. Three files changed: `stage0/runtime.h`,
`examples/effects/issue_682_cancel_sibling_handler.kai{.out.expected}`,
`stage2/Makefile`, `docs/info/fibers.md`, plus the retro itself.

## Symptom + path divergence

The repro from the issue body produced:

```
worker: parking
canceller: cancelling
main: done
```

missing the `worker: cancelled cleanly` that a synchronous `Cancel.raise()`
inside the same `handle { } with Cancel { ... }` would have produced. Two
cancel-delivery paths had quietly diverged:

- **Synchronous path** — user code calls `Cancel.raise()` directly inside a
  handler scope. Lowering goes through `emit_named_call`'s `Eff.op(args)`
  branch (stage2/main.kai:11405-11468): `kai_evidence_lookup_node("Cancel")`
  walks the evidence stack, finds the user `with Cancel { ... }` node,
  invokes the clause with a fresh `KaiCont`, and when the clause discards
  `resume` (forced — `raise(): Nothing` has no Nothing value to feed back)
  longjmps to the handle's `handle_jmp` landing pad. The clause's return
  value lands in the handle's `_discard` slot, becomes the handle
  expression's value, and `_body_result` flows into the return clause.

- **Sibling-initiated path** — sibling fiber calls `Spawn.cancel(target)`.
  `kai_default_spawn_cancel` sets `target->cancel_requested = 1`, detaches
  the target from any reactor waiter list (#679 fix), and unparks it. When
  the target resumes (via `kai_sched_park`'s post-resume hook or the
  next `kai_evidence_lookup*` call), `kai_check_cancel_yield_point` fired
  and longjmp'd straight to the trampoline's `cancel_pad` — marking the
  fiber `CANCELLED` without visiting any user handler.

The two paths agreed by design on the "no user handler in scope" case
(both wind up at the trampoline's tail and propagate awaiter wakeups +
link chain), but disagreed silently on the "user handler in scope" case
that is the documented graceful-shutdown idiom in `kai info fibers`.

## Fix shape

`kai_check_cancel_yield_point` now walks the evidence stack before
longjmp'ing. If it finds an innermost `Cancel` node with a live
`handle_jmp` (a user-installed handle — the default Cancel handler does
not allocate a jmp_buf because its clause never longjmps), it mirrors
the synchronous op-call shape: bind identity continuation, mark the node
as in-dispatch across the call to preserve the m8 bug #12 recursion-
into-outer-frame invariant, invoke the clause, then if `status ==
UNRESUMED && handle_jmp != NULL` store the discarded value and longjmp
to the handle's landing pad. If no user handler is in scope, it falls
back to the `cancel_pad` path verbatim.

The walk skips `f->in_dispatch_node` for the same reason
`kai_evidence_lookup_node` does (m8 bug #12, stage2/main.kai:11432-11445):
a Cancel handler that is itself the active dispatch must not re-enter
itself if the sibling cancel hits while its clause body is running. The
walk continues past it to an outer `with Cancel { ... }` if one exists,
or to the cancel_pad fallback otherwise.

`cancel_delivered = 1` is flipped *before* the clause invocation. Without
the early flip, any op call inside the clause body (e.g. `Stdout.print`)
would re-enter `kai_check_cancel_yield_point`, see `cancel_requested`
still set with `cancel_delivered` still clear, and try to dispatch the
clause a second time. The flip mirrors the post-#103 contract where
`cancel_delivered` is the one-shot guard against re-entry.

## EvCancel ABI dependency

The hook needs to invoke the user clause via its function pointer, which
means it has to know the offset of `raise` inside `EvCancel`. The compiler
emits `struct EvCancel { KaiHandlerId handler_id; void *env; KaiValue
*state; KaiValue *(*raise)(EvCancel *self, KaiCont *k); }` in every
translation unit that imports the Cancel effect (see the emitted C in the
repro: `struct EvCancel` at line 119). The fix mirrors that shape as
`struct KaiRtEvCancel` in `runtime.h` next to the hook.

The Ev-struct prefix (`handler_id`, `env`, `state`, then op fn pointers)
is the convention every effect's lowering follows; it is documented in
`docs/effects-impl.md` §*Evidence layout*. Since `Cancel` has exactly one
op (`raise`) and is a built-in effect (the user cannot extend it with
extra ops), the mirror is stable across compiler versions within an
edition. If the prefix were ever to change, the audit would catch it
because the synthetic dispatch would jump through a NULL pointer; the
compiler change would have to land alongside a runtime mirror update.

## What we did not do

- **Did not** make `Cancel.raise()` walk back to the cancel_pad on a
  non-trap-exit'd fiber. That would have unified the paths the other way
  (kill the cancel_pad delivery and route everything through user
  handlers), but it would have broken the `Spawn.cancel` contract for
  fibers with no user handler — they would no longer terminate, just
  hit `kai_default_cancel_raise`'s `exit(0)` banner, which is not what
  the v1 default contract documents.

- **Did not** touch the lowering. The compiler emits the same code as
  before; only the runtime hook diverges. This keeps the fix to one C
  change in `stage0/runtime.h` and avoids re-running the selfhost dance
  for any stage2 source edits.

- **Did not** unify with `kai_check_trap_exit_cancel_bypass`. The
  trap-exit bypass is the inverse of this fix: it deliberately routes
  *around* user Cancel handlers so a linked supervisor with
  `set_trap_exit(true)` observes the termination through its mailbox
  (issue #103). The two checks coexist in the new code: the trap-exit
  bypass runs at evidence-lookup time for `Cancel` and shortcuts to
  cancel_pad; the yield-point hook runs at *every* op lookup and dispatches
  to the user handler when the sibling-initiated cancel arrives. Since
  the trap-exit case sets the handler-id-skip flag via `cancel_delivered`
  inside its own longjmp, and the yield-point hook sets the same flag
  before dispatch, the two paths cannot fire twice for the same cancel.

## Fixture

`examples/effects/issue_682_cancel_sibling_handler.kai` is the exact
repro from the issue body, wired to `test-issue-682-cancel-sibling-handler`
in `stage2/Makefile` and added to both `test` and `test-fast`. Builds
on top of #679's reactor-detach fix (without it the worker would still
be parked on the timer when the test timed out).

## Cost vs estimate

- Investigation + diagnosis: ~30 min (reading runtime.h cancel paths,
  comparing against #103 and #679, finding the missing handler dispatch
  in `kai_check_cancel_yield_point`).
- Implementation: ~15 min (one function rewrite + one mirror struct).
- Fixture wiring + doc update: ~15 min.
- Selfhost + tier1 verification: ~5 min wall, mostly the selfhost run.

Total: ~1h end-to-end. Close to estimate; the load-bearing work was
locating the two divergent paths and convincing oneself the EvCancel
ABI assumption is sound, not writing the code.

## Follow-ups

- `kai info fibers` now documents both paths as equivalent under the
  "user handler in scope" condition and points at `kai info actors` for
  the trap-exit bypass exception. The aspirational sidebar that said
  user Cancel handlers do not run on runtime-triggered cancel
  (`runtime.h` comment at the old `kai_check_cancel_yield_point`)
  has been removed; the residual-items list in
  `docs/fibers-honesty-targets.md` references the old gap and should
  flip to "shipped" in a follow-up touch.
- The Ev-struct prefix convention is undocumented as a stability
  contract within an edition. `docs/effects-impl.md` should grow a
  §*Runtime ABI invariants* paragraph naming the prefix as load-bearing
  for any runtime helper that synthesizes op dispatch from C. Left for
  a future doc lane; this lane's change is local enough that the new
  `KaiRtEvCancel` comment carries the contract.

## Surprises

None major. The cleanest signal that the fix was on the right track:
the comment block already in `kai_check_cancel_yield_point` (pre-fix)
called out the gap explicitly — "v1 does not run user-installed
`with Cancel { raise(_) -> cleanup }` handlers on runtime-triggered
cancel; that interaction is queued as a follow-up." The hook had a
known TODO; this lane closed it.
