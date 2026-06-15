# Lane experience — issue #842: multi-param generic effect op payload not bound to all row ty_args

## Scope as planned vs as shipped

Planned (from the issue): a parametric effect with 2+ type params whose
op references more than one of them — `effect Repo[a, i] { find(key: i)
: Option[a] }` — wrongly rejects field access on the `find` payload with
"type annotation needed". The issue framed it as a regression of #743
("payload never bound to all row ty_args"), suggested bisecting the
`decl_row` binding from commit `779932c3`.

Shipped: the fix, but the issue's attribution was wrong on two counts.
First, `779932c3` (the `decl_row` positional-unify-at-instantiation
attempt) was **reverted** (`c7e31630`, same day) for breaking
`m7b_11`; the #743 fix that actually shipped is the deferred-field
worklist, not `decl_row`. So there was no "first ty_arg only" bug to
bisect. Second, the trigger is not "op references 2+ params" — `find`
alone with a 2-param effect works. The trigger is **two ops of one
parametric effect raising the same row-label name, one concrete and one
with a free payload**, and it is **order-dependent**.

## Real root cause: a row-dedup that drops a binding

`Repo.save(1, User{...})` raises the concrete label `Repo[User, Int]`;
`Repo.find(1)` raises `Repo[?a, Int]` (the return payload `?a` is free).
`st_add_label`'s dedup (`label_present_unifiable`, a *shallow, pure*
predicate where a TyVar matches anything) sees the two as the same label
and **drops** `find`'s without recording `?a := User`. The surviving row
holds only `Repo[User, Int]`; `check_body_row` unifies that against the
declared `Repo[User, Int]` — no free var to bind — and `?a` stays free
forever, so the deferred-field drain reports "annotation needed" on
`u.name`. When `find` is walked *first*, its label survives, `save`'s is
the one dropped, and `check_body_row` pins `?a` from the surviving
label — hence the order dependence.

## Three dead ends (each broke a different fixture)

1. **Merge-bind at `st_add_label` via the diagnostic `unify_label_args_into`** —
   needs `/ Console + File` threaded through 4 callers, and over-binds.
2. **Full-unify dedup** (treat two labels as dup iff `unify_list`
   succeeds, keep its bindings) — fixes #842, breaks #472
   `dual_actor_request_reply`: `Actor.self()` raises `Actor[?fresh]`
   (a payload-FREE op), and the merge bound `?fresh` to the first
   same-effect sibling in a multi-instance row (`Actor[Reply]` vs the
   needed `Actor[Request]`), the exact "guess by row position" the typer
   must not do.
3. **Keep the binding-bearing label as a separate row entry** — fixes
   #842 and #472, breaks `m8_7_actor_self_send`: the kept `Actor[?fresh]`
   never unifies to `String` and leaks as `effect not handled:
   Actor[String]`.

The discriminator between #842's `find` payload (must be pinned from the
row) and #472/m8_7's `self`/`receive` payload (must stay free, pinned by
its own usage later in the body) is **timing**, which `st_add_label`
cannot observe — identical to the m7b_11-vs-#842 tension that sank the
reverted `decl_row` attempt.

## The fix: declared-row pinning at the deferred-field drain (asu's call)

Pin at the drain, which runs **after** `check_body_row` and after all
usage-based inference settles:

- `st_add_label` is left exactly as on `main` (drop the duplicate). The
  eager merge is wrong at that point in time.
- Each parametric op call stashes `OPV(eff, ids)` — the effect name and
  the **live tyvar ids** of its row ty_args (`eff_fresh`) — in a new
  `InferState.op_payload_labels`, captured *before* the dedup can drop
  the label.
- At drain, for a receiver still free, find the stashed op whose payload
  shares the receiver's **final representative** (`ids_share_rep`), then
  unify that op's label — resolved under the final sub — against the
  declared row via the existing `find_matching_declared` /
  `unify_label_args_into`.

## The trap that produced a silent false-green

The first cut stashed the resolved `Label` (frozen `Ty`s). That failed
silently: the deferred-field receiver resolves to `?t13` (introduced by
the `match` binder), the stashed label carried `?t5` (the op's payload),
and the two are distinct ids. **`row_args` is a snapshot of `apply_ty`
taken at op-call time — before the `match` binder unifies the receiver
with the return payload — so it freezes a dead representative.** The fix
is to stash the live *ids* and resolve them under the *final* sub at
drain (asu's option B′): then the receiver and the payload share a
representative iff the body actually connected them, which the `match`
binder did. Comparing raw stored ids never works; comparing final reps
does. Verify any future change here with two `apply_ty` traces, not by
reading the typed dump alone.

## Soundness for the discarded-payload-free op

The co-class precondition (`ids_share_rep`) is what makes this sound
where attempt 2 was not. A discarded payload-free op
(`let _ = Box.peek()` in `/ Box[Int] + Box[String]`) never joins its
payload to any receiver class, so no deferred field shares its rep and
the pin never fires — no arbitrary binding by row position. This is the
5th fixture (kept as a note below); it is the case that distinguishes
B′ from the unsound shallow-guard variants.

## Why it cannot reintroduce m7b_11

The drain runs after the row-subset check and only unifies inferred op
labels against the declared row's concrete ty_args; it never feeds
row-subset checking, so the `Reader.ask() + 1` body that *determines*
its payload (and must surface a row-mismatch) is untouched — at drain
its receiver is already concrete, no deferred field, no-op.

## Fixtures

- `examples/effects/issue_842_multi_param_op_payload_field.kai`
  (+ `.out.expected` = `alice 30`) — positive: declared-row form,
  `save` before `find` (the failing order), runs on C + LLVM.
- `examples/effects/issue_842_multi_param_op_payload_bad_field.kai`
  (+ `.err.expected` = `no field 'zzz' on User`) — the Tier 1 escape
  canary: a bad field is rejected on the now-concrete `User`.
- `stage2/Makefile` `test-issue-842` (wired into `test` +
  `TEST_LIGHT_TARGETS` + `.PHONY`), mirroring `test-issue-743`.
- Kept as notes, verified by hand (typer machinery, not this bug shape):
  #472 `dual_actor_request_reply` + `m8_7_actor_self_send` stay green
  (the two regressions the dead ends introduced are gone); the
  discarded-payload-free op in a multi-instance row compiles without a
  position-binding.

## Follow-up left for a next lane

The **handler-form** of the same shape — `handle { worker() } with
Repo[User, Int] { ... }` where the concrete instantiation lives in the
handler clause's ty_args rather than a declared row — still reports
"annotation needed". This **pre-dates this fix** (verified on a pristine
tree) and is out of scope for #842, whose repro is the declared-row
form. Closing it means teaching the drain to pin from an enclosing
handler's clause ty_args, not only the function's declared row.

## Cost vs estimate

The fix is ~120 lines in `infer.kai` (one new `InferState` field with
the ~20-constructor flat-record tax #743 documented, one new type, the
stash setter, and the drain pin pass reusing `find_matching_declared`).
The expensive part was the diagnosis: three dead ends and a silent
false-green before the capture-as-value root surfaced under a tyvar-id
trace. selfhost byte-id held throughout (the bug shape is not in the
selfhost corpus — byte-id is false-green here; the four behavioural
fixtures are the real gate).
