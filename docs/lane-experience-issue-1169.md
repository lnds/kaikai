# Lane experience — issue #1169: enforce refinement-type predicates

## Scope as planned vs shipped

Planned: refinement predicates (`type T = Base where P`) were parsed and
type-checked but never enforced outside the direct-literal call-site check.
Close the hole at the three downcast sites — annotated bindings, refined
parameters, refined returns — rejecting closed values at compile time and
inserting a structured runtime check (requires/ensures panic shape) for
dynamic values, with the check omitted where the predicate is proven.

Shipped exactly that, plus two pieces the plan implied but did not name:

- `try_eval_pred` extended with a Real-literal comparison fallback
  (`try_eval_real`), so `let bad: Percent = 1.5` folds and rejects at
  compile time — previously only Int predicates folded.
- `expr_show_pred` now renders Real literals from their raw source span
  (`0.0`, not `real_to_string`'s `0`), which improved three existing
  requires goldens (updated in this lane).

## Design decisions

- **Where the pass runs.** The brief pointed at the typer (`TyRefineT`
  downcast in the unifier). The实 machinery worth reusing — `classify_arg_vs_pred`,
  `try_eval_pred`, interval/regex entailment, the structured panic shape —
  is all syntactic, pre-typer, in `refinements.kai`. And after
  `expand_ta_decls` every refinement alias is already the inline
  `TyRefine(base, pred)` shape on params, returns, and annotations. So the
  pass (`refine_enforce.kai`) runs right after transparent-alias expansion,
  where the existing call-site literal rejection also lives. No typer
  changes; the unifier's `TyRefineT(b,_) ⊑ b` arms are untouched.
- **One classifier for all three sites.** Each downcast calls
  `classify_arg_vs_pred(pred, "self", value, refs)`: `CSElided` → no
  check, `CSLoose` → compile error (static reporter) and a
  guaranteed-to-fire assert (rewrite), `CSRuntime` → assert. Binding names
  with refined annotations extend the entailment environment as the walk
  descends, so `let y: NonNeg = x` with `x: NonNeg` elides.
- **Two passes instead of one.** The rewrite is pure (recurses via
  `dsg_map_expr_kind`, exhaustive over ExprKind); the static reporter is a
  manual walk threading `Int / Console`. kaikai compiler code has no
  mutable accumulation through a `(Expr) -> Expr` closure, so a single
  errs+rewrite pass would have required hand-walking every ExprKind
  variant — the known sweep trap. The reporter's coverage equals the
  rewrite's; if they ever drift, a refuted site in an unwalked shape still
  panics at runtime (soundness does not depend on the reporter).
- **Return checks append to the body block tail** (binding `__ref_ret`
  only for non-EVar tails) instead of wrapping the whole body. This keeps
  the requires-assert prefix at the front of the block, which
  `extract_requires_asserts` (call-site analysis) depends on; param checks
  are spliced *after* that prefix for the same reason.
- **`__ref_pred_*` fns are skipped.** Match-arm narrowing calls them to
  *test* a predicate; enforcing the (alias-expanded) param refinement
  inside them would turn a false guard into a panic.
- **`var` reassignment is not re-checked** — only the init value. Checked
  reassignment needs the typer; noted in the doc sidebar as follow-up.

## Structural surprises

- **The brief's map inverted (as warned).** No check insertion happens in
  the typer; requires/ensures lowering lives in parse (`wrap_with_contracts`)
  and the reusable discharge machinery in `refinements.kai`. The docs'
  "typer inserts a runtime check" describes the semantic model, not the
  implementation site.
- **kaic1 bundle traps cost the only real debugging time.** A binder named
  `args` in the new module was silently resolved to the `args` prelude
  builtin by kaic1 (stage1), so a list-match over call args hit a closure
  tag → `panic: non-exhaustive match` on *every* input, including hello
  world. Build green, binary broken. The memory-documented recipe
  (rename to `args_` / `cargs`, mimic `validate_contract_expr`'s exact
  shape) fixed it. Diagnosed by bisecting with an identity rewrite.
- **A stale kaic2 misled once**: a cosmetic fix looked ineffective because
  the harness had rebuilt kaic2 without the edit; the next harness run
  (which rebuilds) showed it working. FRESHNESS discipline is real.

## Fixtures

Six new fixtures in `examples/refinements/`, wired into `test-violations`
(tier 1 via TEST_LIGHT_TARGETS):

- `refine_binding_literal_violation` (.err.expected — compile reject)
- `refine_return_literal_violation` (.err.expected — compile reject)
- `refine_param_runtime_violation` (.run.err.expected — structured panic)
- `refine_binding_runtime_violation` (.run.err.expected)
- `refine_return_runtime_violation` (.run.err.expected)
- `refine_valid_passes` (.out.expected — happy path, and the emitted C was
  checked to contain no assert for the two statically-proven bindings)

`test-violations` gained a third golden shape: `<name>.err.expected` =
compile-time rejection, matched per line (grep -F), same contract as the
unions harness. Three pre-existing requires goldens updated for the
raw-span Real render (behavioural improvement, pinned intentionally).

Coverage gaps left open: refined element types inside containers
(`[NonNeg]` literals), string-interpolation sub-expressions (parsed after
this pass runs), `var` reassignment.

## Cost vs estimate

Single sitting. Roughly half the time went to the kaic1 `args`-shadow
trap; the enforcement logic itself landed close to first-write (the
existing classify/entailment machinery did all the heavy lifting).

## Follow-ups

- Checked `var` reassignment (typer-side).
- Refined container element types and interp sub-expressions.
- The `0 <= self` vs `0.0 <= self` render class is now span-faithful; if
  literal minting ever rewrites predicate spans, the goldens will say so.
