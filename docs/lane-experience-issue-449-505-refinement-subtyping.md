# Lane retro — issue-449-505-refinement-subtyping

## Scope as planned vs. as shipped

**Planned:** close #449 and #505. The brief, drawing on Eric's
2026-05-12 audit, treated the two issues as one root cause —
"refinement-type aliases are nominal where they should be transparent
sugar for the inline `Int where p` form". Hypothesis: `type X = Int
where p` registers as a fresh `TyCon("X", [])` and the alias-expansion
pass skips it, so every use site stays nominal and unifies against
neither the base type nor the inline-refined form. Fix shape A from
the brief: desugar the alias at expansion time so the existing inline
machinery handles the rest.

**Shipped:** exactly what the brief proposed, in 11 lines of
compiler diff plus four positive fixtures and one comment update
mirrored higher in the file. No type-system reshape, no unifier
change, no parser change. The existing `TyRefineT(b, _) ⊑ b` arms
in `unify_env` already handle every direction the issues asked for;
removing one early-return from `transparent_alias_of_decl` is enough
to feed them.

## Design decisions and alternatives considered

- **Shape A (desugar at expansion).** Chosen. Single point of
  change — `transparent_alias_of_decl` drops the `TyRefine(_, _) ->
  None` arm and falls through to the generic structural expansion.
  Every `TyName("Pos", [])` in a signature, binding, or constructor
  call rewrites to the alias body `TyRefine(Int, p)`, which then
  resolves to `TyRefineT(TyInt, p)` via the unchanged `resolve_ty`
  path. The unifier already does the subtyping work.

- **Shape B (subtyping in `unify_env`).** Rejected. Already done.
  `unify_env` has `TyRefineT(abase, _) -> unify_env(env, abase, b, s)`
  and the symmetric arm, both bidirectional today. Adding a "nominal
  refinement alias subsumes its base" arm would only have masked the
  real bug — that the alias was never expanding in the first place.

- **Shape C (keep alias name + add unifier subtyping for it).**
  Rejected. Would require teaching the unifier to look up an alias
  table — which `transparent_alias_of_decl` already produces, just
  for a different consumer. Two paths, same intent.

The reason the alias was excluded from expansion to begin with was a
conservative move tied to `synthesize_refine_pred_fns` and
`lower_pattern_narrow_decls` — both of which key off the alias name
in match-arm patterns. Confirmed reading the pipeline order in
`compiler.kai` lines 57895–57960: those two passes run on `merged_raw`
*before* `expand_ta_decls` fires. Once they have produced
`__ref_pred_X` and rewritten every `PNarrow` against `X`, the alias
name has no further obligations and the carrier can be expanded
freely.

## Structural surprises

1. **The exclusion was overcautious, not load-bearing.** The
   precondition (synth + pattern lowering run first) was already
   satisfied; the comment had ossified into a rule. Reading the
   pipeline ordering directly was faster than relitigating the
   design — the fix turned out to be a one-arm deletion.

2. **An adjacent bug surfaced during fixture writing.** A
   refinement-typed parameter (alias *or* inline) forwarded through
   an intermediate function to a base-typed callee returns the
   argument value instead of the callee result. Reproduces in
   `inline-form` so it predates this lane. Routed out as **#563**;
   the lane stayed scoped to alias subtyping.

3. **No selfhost effect.** `stage2/compiler.kai` does not use a
   single refinement-type alias internally, so the expansion change
   touches zero code in its own compilation. Selfhost is byte-
   identical and the fixed-point closes cleanly.

## Fixtures shipped

Wired into `test-violations` (which iterates every
`examples/refinements/*.kai`):

- `alias_arithmetic.kai` / `.out.expected` — `NonNeg + NonNeg`
  returns an `Int` that flows back into `NonNeg`. Drives #449
  arithmetic and #505 `Add` lookup at once.
- `alias_function_call.kai` / `.out.expected` — `Probabilidad`
  passed to `real_to_string(r: Real)` succeeds. Drives the #449
  function-call shape.
- `alias_literal_assignment.kai` / `.out.expected` — `identity(16)`
  with `n: NonNeg` succeeds. Drives the #505 literal-assignment
  shape (the entailment that admits `Int` literals into a refined
  parameter when the literal proves the predicate).
- `alias_passed_to_base.kai` / `.out.expected` — local binding
  `p: Probabilidad = 0.25` flows through `pair: Probabilidad ->
  Real` and arithmetic composes. Exercises both subtyping
  directions in one program.

No negative fixture added: the existing
`requires_violation_diagnostic.kai` family already covers predicate
violation at construction time, and the lane did not touch the
violation diagnostic path.

## Coverage gaps left

- `alias_passed_to_base.kai` mentions a refinement-typed parameter
  passed through one intermediate function. The intermediate
  function's return type is `Real` (the base), not the refined
  alias — that combination triggers **#563** for *any* refined
  parameter type, not just aliased ones. Once #563 lands, a sixth
  fixture testing `Probabilidad -> Probabilidad` round-trip becomes
  meaningful; today it can't be written without hitting #563.

## Real cost vs. estimate

- **Estimate:** 1–3 days.
- **Actual:** ~3 hours including the #563 detour and lane retro.

The brief's hypothesis matched the code on the first read. The
slowest part was confirming that `synthesize_refine_pred_fns` and
`lower_pattern_narrow_decls` did not depend on the alias name
surviving past the expansion pass.

## Follow-ups left for next lanes

- **#563** (opened this lane) — refinement-typed parameter forwarded
  through an intermediate fn returns the argument instead of the
  callee result. Independent of alias resolution. Needs a separate
  investigation in monomorph / call-site lowering.
- Once #563 closes, add `alias_round_trip.kai` exercising
  `Probabilidad -> Probabilidad` through an intermediate fn.
- `docs/m12.6x-refine-prop-design.md` v1-status sidebar can drop the
  "aliases like `type Port = Int where ...` followed by `p : Port`
  are not resolved at parse time yet — that needs an alias-resolver
  hook" caveat. Deferred to a doc sweep lane.
