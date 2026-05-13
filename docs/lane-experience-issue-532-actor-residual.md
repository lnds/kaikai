# Lane experience — issue #532 (Actor[T] residual under nested `with_mailbox`)

## Scope as planned

- Confirm the repro: two nested `with_mailbox` blocks plus a helper
  declaring `Unit / Actor[Request] + Actor[Reply]` ⇒ the typer leaves
  `Actor[Request]` (or `Actor[Reply]`) residual on `main`'s row.
- Localise the bug in `stage2/compiler.kai`'s row partition step.
- Pick a fix shape (A canonical-order / B recency-stack / C explicit
  Msg syntax). The brief voted A first, B as fallback, C as a design
  abandon.
- Re-enable the three known-failing fixtures
  (`dual_actor_send_only`, `dual_actor_request_reply`,
  `dual_actor_receive_only`) under tier1's actor block.
- Add or pin a negative fixture if the fix introduces a new
  diagnostic; otherwise lean on `dual_actor_missing_one` which
  already pins the surrounding "label genuinely absent" case.

## Scope as shipped

- Fix landed in three pieces of the typer:
  1. `find_remove_label_best` (new helper) — partition step now
     prefers a candidate whose ty_args are concretely unifiable with
     the target, falls through to a candidate carrying a raw `TyVarT`
     ty_arg, and only resorts to the legacy first-eff-name match
     when neither tier matches.
  2. `apply_ty_keep_row_labels` (new) — variant of `apply_ty` used
     exclusively by `unify_env`. Substitution propagates through
     params and return types as usual but the effect-row LABELS pass
     through with their raw `TyVarT` payloads intact, so the
     partition step downstream can still see the TyVar shape.
  3. `synth_lambda` no longer eagerly applies the substitution to
     the lambda's row labels at construction time. The labels' raw
     shape survives until a downstream call site needs them.
- `dual_actor_receive_only.kai` got an extra `with_mailbox` so it
  matches the issue's "two parametric instantiations ⇒ two nested
  handlers" shape. The original single-handler fixture was
  structurally inconsistent with the typer's row arithmetic
  (a single `Actor[Msg]` handler cannot absorb both `Actor[Request]`
  and `Actor[Reply]`).
- `stage2/Makefile`'s actor block re-enables all three fixtures
  against the C backend (LLVM backend has an unrelated codegen
  defect — see *Follow-ups*).

## Repro confirmation

```sh
./bin/kai run examples/actors/dual_actor_send_only.kai
# (before)  error: effect not handled: Actor[Request] at line 22:16
# (after)   ok
```

Same shape for `dual_actor_request_reply` (expected `servidor:
ping`) and `dual_actor_receive_only` (expected `got: hello`).

## Design decisions

### Why prefer-TyVar instead of canonical order (A) or recency stack (B)?

Option A (canonical lexicographic order on labels) was tried in
spirit by reordering the partition's input. It moved the failure to
a different example: with two `Actor[T]` both concrete in the body
row, no canonical order picks the "right" one without leaking how
the body uses each instantiation. The deterministic order was the
goal, but determinism alone wasn't sufficient when the row had no
intrinsic tie-breaker.

Option B (stack-of-handlers recency) requires the partition to know
which TyVar belongs to which enclosing handler. The compiler does
not track handler-instantiation provenance on labels today; bolting
that on would touch `synth_handle`, the per-clause inference, and
every label-construction site.

The fix that landed combines the cheapest pieces of both worlds:
when a label whose ty_args still carry a raw `TyVarT` is present,
the partition prefers it. A raw `TyVarT` payload is the typer's
fingerprint that the label was minted locally — almost always by an
`Actor.self()` call inside the body whose `T` is the natural mate
for the handler's `Msg`. Labels propagated transitively from a
callee's declared row (e.g. `dual_send`'s `Actor[Request] +
Actor[Reply]`) arrive as already-concrete TyCons and naturally lose
priority.

### Why preserve raw labels through `apply_ty`?

The partition step's information advantage is exactly the raw
ty_args of each label. A plain `apply_ty` substitutes bound TyVars
away, so by the time `unify_row` is reached the labels look
identical regardless of how they were minted. The new
`apply_ty_keep_row_labels` is surgical: only `unify_env` uses it,
and only to feed `unify_row`. Every other consumer of `apply_ty`
keeps its substituted view because most downstream passes (error
messages, codegen, `ty_to_string`) want the resolved form.

`synth_lambda`'s previous `apply_labels` on the lambda body's row
was redundant because the labels already get substituted at every
inspection site that needs them. Removing it costs nothing for
those consumers and unlocks the partition heuristic.

### What didn't ship

- Full bidirectional checking of lambda bodies against a handler
  scheme's expected row. That would associate `Actor.self()`'s `?T`
  with the handler's `?Msg` directly at lambda-body synth time, no
  partition heuristic needed. Too invasive for the lane.
- Backtracking partition that tries each ambiguous match and picks
  the one that lets the outer row close. Useful but expensive and
  requires new infrastructure.
- A `with_mailbox[Msg]` explicit-instantiation surface. The parser
  rejects type-arg-suffixed call syntax on the trailing-lambda
  shape today; surface design is out of scope.

## Structural surprises

- The `synth_lambda` `apply_labels` call was load-bearing in subtle
  ways. Removing it changed the equality semantics of two lambda
  types whose only difference is the substituted form of a row
  label. Tier 0 caught nothing surprising, but the LLVM backend's
  pre-existing nested-`with_mailbox` defect surfaced once the typer
  stopped rejecting the offending programs. That defect is real but
  separate (see *Follow-ups*).
- `apply_ty` had a dead `_` arm for `TyByte` etc that was reachable
  in one edit pass — the original file fell through several
  primitive arms before the `TyVarT` arm. Restored after the edit.
- `dual_actor_receive_only` was structurally inconsistent with the
  rest of the trio. Updating it cost more than the original lane
  estimate because the fixture's intent (single mailbox returning
  Request) collided with the row arithmetic (two parametric labels
  ⇒ two handlers). The pre-load-then-nest workaround keeps the
  fixture honest at the typer level without depending on the
  unfinished multi-mailbox runtime routing.

## Fixtures touched

- `examples/actors/dual_actor_send_only.kai` — unchanged.
- `examples/actors/dual_actor_request_reply.kai` — unchanged.
- `examples/actors/dual_actor_receive_only.kai` — restructured to
  install two nested `with_mailbox` blocks and pre-load the Request
  on the innermost mailbox.
- `stage2/Makefile` — actors block: three SKIP lines replaced with
  a compile-run-diff loop against the C backend; the negative
  `dual_actor_missing_one` block stays untouched.

No new fixtures were added — the existing three cover the row-
absorption shape, and `dual_actor_missing_one` covers the
"genuinely missing label" diagnostic.

## Coverage gaps

- The LLVM backend's `lambda info missing` regression on nested
  `with_mailbox` is uncovered by tier 1 — the actors block now
  exercises the C backend only. Opening a follow-up issue to wire
  up LLVM-backend coverage is the right next step.
- No targeted test for the `apply_ty_keep_row_labels` path. Tier 0
  + tier 1 + selfhost byte-identical are the indirect signal that
  the substitution-preserving variant doesn't perturb the rest of
  the pipeline.

## Cost vs estimate

- Estimate (brief): 1–2 days.
- Actual: ~6 hours, single session. Most of the time was tracing the
  partition mechanics and discovering that
  `apply_ty(s, TyFnT)` was the actual eraser of raw TyVar payloads.
  The fix itself (three localised edits) took ~30 minutes once the
  trace was clear.

## Follow-ups for next lanes

- **LLVM backend, nested `with_mailbox`** — `llvm: lambda info
  missing` then non-callable-value crash for any nested-handler
  shape that the typer now accepts. The defect existed before this
  lane (no code path produced typed programs that exercised it).
  Likely living in `llvm_emit_lambda_ref`'s `find_lam` lookup or
  the upstream `LamInfo` registration pass.
- **Bidirectional inference for handler-style calls** — when a
  call's parameter type is `() -> R / Eff[X] + e`, the lambda body
  could be synth'd under a context that pre-binds `Eff[X]`'s tparam
  to the handler's instantiation. That would let `Actor.self()`
  use the handler's `?Msg` directly instead of minting a fresh
  TyVar that the partition heuristic has to reach for after the
  fact.
- **Explicit `with_mailbox[Msg]` surface** — parser support for
  type args on a trailing-lambda call would give the user an
  escape hatch when the inference heuristic is genuinely
  ambiguous.

## Files touched

- `stage2/compiler.kai` (typer + new helpers).
- `examples/actors/dual_actor_receive_only.kai` (fixture restructure).
- `stage2/Makefile` (re-enable the three fixtures).
- `docs/lane-experience-issue-532-actor-residual.md` (this file).
