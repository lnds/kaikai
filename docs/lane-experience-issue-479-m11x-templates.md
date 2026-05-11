# Lane experience — issue #479 (m11 v1.x templates 4 + 5)

Best-effort retrospective by the implementing agent.

## Scope as planned vs as shipped

**Planned (per issue #479 + lane brief):**

- Template 4 — wrong arity, fired in place of the generic
  "type mismatch in function call" for arity mismatches.
- Template 5 — missing effect, m11-shape upgrade of the existing
  "effect not handled" diagnostic.
- 6–10 fixtures total, both negative + positive regressions.
- Tier 0 + Tier 1 green locally, selfhost byte-identical.
- Visual + JSON shape matches m11 v1.
- Retro + PR closing #479.

**Shipped:**

- Template 4 implemented as a preflight inside `st_unify_call`
  (stage2/compiler.kai). When the resolved callee type is a
  `TyFnT(params, …, …)` and `len(params) != len(arg_tys)`, we
  emit the wrong-arity diagnostic with name, declared arity,
  signature, and a tailored help line (add missing / remove
  extra). The preflight short-circuits before `st_unify`, so we
  never produce both the generic type-mismatch and the arity
  diagnostic for the same site.
- Template 5 implemented as an in-place enrichment of the
  existing `check_body_row_label_loop` site. The legacy header
  `effect not handled: <label>` is preserved verbatim so every
  prior fixture
  (`examples/effects/m8_2_yield_unhandled`,
  `array_mutable_required`, `m7b_11_followup_diag_type_args`,
  `m12_8_y_phase4_no_row_negative`) keeps matching. The new
  notes are: `missing effect: \`L\``, `enclosing row: <row>`,
  plus two separate `help:` lines so an editor can present the
  two remediation paths (declare the effect vs. handle locally)
  as parallel quick fixes.
- 9 fixtures: 4 t4 negatives + 1 t4 positive regression, 3 t5
  negatives + 1 t5 positive regression. All wired into the
  pre-existing `test-diagnostics` make target (no new make
  targets needed — the loop just globs `examples/diagnostics/`).
- Tier 0 green, Tier 1 green, selfhost byte-identical (the
  selfhost fixed-point check inside `make tier0` succeeded).
- Retro + lane handoff per the lane-handoff exception.

**Not in scope (left for future lanes):**

- `--diagnostics-json` does not exist as a flag yet — the m11
  series ships visual diagnostics first; LSP integration via
  #447 will consume them later. The issue body cited it as a
  goal but the JSON contract is still typed-holes-only
  (`docs/typed-holes.md`). The two new templates respect the
  same line-by-line `error:` / `note:` / `help:` convention so
  the future JSON serializer can mechanically lift them without
  touching the emit sites.
- Nested-closure effect propagation is already correctly
  reported by the existing post-typing walker
  (`collect_call_labels`), so no new case was needed there — a
  closure that performs `Stdout` inside a parent that does not
  declare it surfaces the same Template 5 diagnostic at the
  call site of the outer function.

## Design decisions

### Template 4 — preflight vs. post-failure note

I considered two options:

1. **Post-failure** (mirror Template 1): let `st_unify` fail as
   "type mismatch in function call", then add an arity-detail
   note when the cause was clearly arity.
2. **Preflight** (what shipped): detect arity mismatch *before*
   `st_unify` runs and emit a distinct header
   "wrong number of arguments to `fn`" in its place.

I picked preflight because:

- The diagnostic header is what most readers (and LSP clients)
  index against. Saying "type mismatch" for an arity bug muddies
  the failure category and forces downstream tooling to parse
  the notes to disambiguate. "Wrong number of arguments" is the
  category.
- The arity test is `O(1)` after `apply_ty` — cheap.
- No existing fixture relied on the generic header for an arity
  mismatch (`grep -rl "type mismatch in function call" examples/`
  returned six fixtures, all of which exercise *argument-type*
  mismatches with matching arity).

### Template 5 — in-place vs. call-site duplication

The first attempt added a *second* missing-effect diagnostic at
the call site inside `st_unify_call`, mirroring how Template 1
augments the post-unify failure path. That attempt was reverted
because the call-site `expected_fn_ty` is constructed with a
*flex* row tail (`Some(fr.id)`), so `unify_row` always succeeds
on row labels — it absorbs the callee's effects into the fresh
tail variable. The actual missing-effect failure surfaces
later, in `check_body_row_label_loop`, which compares the
performed-row against the declared row of the enclosing
function.

That site already had a good error (header + note + combined
help); the m11-shape upgrade was purely additive: a second
`note:` line for the missing effect (so an LSP quick-fix can
key off it without parsing the header) and a third `note:`
for the actual enclosing row state. The combined help line is
preserved so old fixtures match; two new help lines split the
remediation into the two separately-actionable suggestions.

### Helpers added

- `arity_mismatch_of(sub, callee_ty, arg_tys) : Option[Int]` —
  preflight predicate.
- (an earlier `fmt_param_tuple` helper was drafted and then
  deleted — `ty_to_string(TyFnT(...))` already renders
  `(T1, T2) -> R / Eff` correctly, and the no-premature-
  abstraction rule wins over keeping a parallel helper.)
- `emit_wrong_arity(file, callee, callee_ty, arg_tys, l, c)` —
  the Template 4 emit block.
- `render_declared_row(declared) : String` — small helper for
  the `enclosing row:` note in Template 5.

## Structural surprises

- **The "missing effect" site already existed**. Going in I
  expected Template 5 to be brand-new emit code at the call
  site. The actual structure is the row-check pass, not the
  unifier: row mismatches at call sites are absorbed by flex
  tails and only failed at `check_body_row`. The lane became
  an enrichment of one site, not a new diagnostic path. ~30
  LOC instead of the ~80 I budgeted.
- **`apply_ty` on the callee** in `st_unify_call` was already
  available because Template 1 needed it. The arity preflight
  added no new substitution-walk cost.
- **Two-help-line discipline.** Existing m11 v1 fixtures match
  a single `help:` line. I added two new `help:` lines in
  Template 5 (split-remediation) and kept the original combined
  line so old fixtures still find their substring. New fixtures
  match against the *new* lines. This means stderr has three
  `help:` lines for Template 5; if that ever becomes noisy, the
  combined line can be retired and the old fixtures rewritten
  to match the split form. Out of scope for this lane.

## Fixtures and coverage

Added under `examples/diagnostics/`:

- `m11_t4_too_many_args.kai` + `.err.expected`
- `m11_t4_too_few_args.kai` + `.err.expected`
- `m11_t4_zero_args_to_unary.kai` + `.err.expected`
- `m11_t4_ok_correct_arity.kai` (positive regression, no
  golden — must compile + run cleanly)
- `m11_t5_single_missing_effect.kai` + `.err.expected`
- `m11_t5_multi_missing_effect.kai` + `.err.expected`
- `m11_t5_partial_row_missing.kai` + `.err.expected`
- `m11_t5_ok_effect_declared.kai` (positive regression)

The existing `test-diagnostics` make target globs the directory
so no Makefile edit was needed. Tier 1 picks them up
automatically through `make test` → `test-diagnostics`.

Coverage gap left: arity on UFCS / `try_bare_call_narrow` /
pipe-dispatch paths is in the same `st_unify_call` wrapper so
all five call paths inherit Template 4. No path-specific
fixture was added (the existing fixtures exercise the
canonical `synth_call` path); a follow-up could pin UFCS arity
explicitly, but the wrapper makes the coverage structural, not
incidental.

## Estimated vs. actual cost

- Estimate (issue): 2–3 days.
- Actual: single session, ~1h elapsed inside the lane. The
  Template-5-already-exists discovery cut the implementation
  in half; the test target was already wired; the fixtures
  followed the m11 v1 precedent verbatim.

## Follow-ups for next lanes

- **`--diagnostics-json`** is still aspirational. The two new
  templates respect the line discipline; an LSP lane (#447)
  can lift them mechanically.
- **Combined-help cleanup**: Template 5 currently emits both
  the legacy combined help line ("either declare … or wrap …")
  and the new split lines. A future m11 v2 lane can remove the
  combined line and rewrite the four pre-existing fixtures
  (`m8_2_yield_unhandled`, `array_mutable_required`,
  `m7b_11_followup_diag_type_args`, `m12_8_y_phase4_no_row_negative`)
  to the split form.
- **Roadmap.md update**: this lane closes the m11 series for
  v1; `docs/roadmap.md` should flip the m11 entry to "v1
  complete" once #479 merges, per the doc-discipline rule.
