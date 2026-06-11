# Lane retro — issue #802 (row-polymorphic instantiation / distinct parametric effect cells)

**Scope:** Make a higher-order whose row-polymorphic signature shares one
row variable `e` across two function arguments unify correctly when the
two arguments touch *distinct* instances of one parametric effect. The
canonical trigger is `loop.while`
(`(() -> Bool / e, () -> Unit / e) -> Unit / e`): a condition thunk that
reads `var go` (`State[Bool]`) and a body thunk that reads `var i`
(`State[Int]`) plus `Stdout` must fuse into
`State[Bool] + State[Int] + Stdout + e`, not collapse `Bool ≡ Int`.

## Scope as planned vs as shipped

**Planned (per the issue's diagnosis):** the row var of the signature was
giving *separate open tails* to each occurrence, so the fix was framed as
"one fresh row var per signature row-var, shared across all occurrences."

**As shipped:** the diagnosis was wrong about the *root* (instantiation is
correct — `st_instantiate_report` already mints one fresh tail per bound
row-var and shares it via the temp substitution). The two issue repros
(Form A, Form B) actually hide **three distinct bugs**, found by reducing
the repro to its minimal trigger:

1. **Label partition fused distinct parametric effects.**
   `find_remove_label_best` tier-3 (`find_remove_label`) matched two
   same-eff labels by NAME ALONE, ignoring `ty_args`. `State[Bool]` and
   `State[Int]` got paired and then failed in `unify_label_pairs` on
   `Bool ≢ Int`. Tier-2 (`find_remove_label_tyvar_args`) had the same
   over-match for labels whose args were still TyVars. Fix: gate both
   tiers on `labels_args_unify`, and tier-3 is now an explicit
   `find_remove_label_unifiable`.

2. **`Mutable` masking never ran on lambda bodies.** A `var i` inside a
   thunk lowers to a local `array_make` slot whose `Mutable` effect
   `infer_decl` masks for top-level fns (`mask_local_mutable_demand`,
   #251/#252) but `synth_lambda` never invoked. A `var` inside a
   trailing-lambda thunk leaked `Mutable` and refused to unify against a
   declared `() -> Unit / Stdout` slot. Fix: call
   `mask_local_mutable_demand` in `synth_lambda` too.

3. **Distinct `var` cells were structurally indistinguishable in the
   row.** `var i` and `var go` each desugar to `with State[T] as <name>`,
   but their row labels were both `State[?T]` — the `as <name>` cell
   identity lived only in the codegen dispatch-key (`State@i`), never in
   the `Label`. When two cells reached a partition with their `T`s still
   unresolved (the `while`-in-lambda shape), they fused and cross-bound
   `Int := Bool`, corrupting `yield`'s parameter type to `(Bool) -> ?t`.
   Fix: thread the cell alias into the LABEL's `eff` string as a
   `"State@i"` suffix (typer-only; emitters and signatures keep seeing
   bare names), with the handler discharge made symmetric.

Bugs 1+2 fix every disjoint-effect HOF case (a hand-written `apply2`,
`while_root`); bug 3 is what the issue's own Form A/B repros need.

## Design decisions

### Cell identity in the `eff` string, not a new `Label` field (asu)

Two routes to give a cell identity:

- **Route A (rejected, out of lane):** add an `origin: Option[String]`
  field to `type Label`. Sound, but `Label` is constructed at ~15 sites
  including `emit_c.kai`, `monomorph.kai`, `fnreg.kai` — adding a field
  crosses into the emit/kir lanes, which this typer-pure lane must not
  touch.
- **Route A' (shipped):** encode the cell identity in the existing
  `eff: String` as a `"State@i"` suffix. No type change; the only
  constructor that stamps it is the typer op-call path
  (`synth_op_call_with_scheme_keys`), gated on `lookup_alias` resolving.
  asu confirmed the suffix never escapes the typer: every cell label is
  discharged by its own handler, so it never reaches a signature,
  `main`'s row, or the emitters (verified — `dual_actor_missing_one`
  still prints clean `Actor[Reply]`, and selfhost byte-id holds).

**The taggability decoupling.** The codegen dispatch tag (`@<alias>` on
the dispatch key) is deliberately *suppressed inside a lambda*
(`aliases_disable_tag`, because the `kai_alias_<a>_id` C local is out of
scope there). But row identity must hold *inside* lambdas too — that is
exactly where the `while` thunks live. So the row-label suffix is stamped
whenever `lookup_alias` resolves, **independent of taggability**, while
the dispatch key stays taggability-gated. The two were the same string
before; #802 splits them.

### Discharge binds the cell type (asu): guard-then-bind

Tagging exposed a latent bug the old behaviour had *masked*: the bare-
`State` labels used to cross-unify in the partition (the very fusion #802
removes), which incidentally *converged* the `T`s of two reads of one
cell. With per-cell tagging the cross-fusion is gone, so two reads of one
cell (`State@xs([?t15])`, `State@xs(?t6)`) no longer converge — and the
discharge's shallow `labels_args_unify` (via `ty_eq_shape`) rejects
nested TyVars, so neither read discharged and `State@xs` leaked
(regressing `m7b_5b_var_handler_clause`, the #148 var-in-clause fixture).

The handler is the *authority* on the type of the cell it discharges, so
the discharge now **unifies-and-binds** the label's args against the
handler's, converging all reads. Implemented as asu's two-step:

- **guard (pure):** `label_args_dischargeable` / `ty_dischargeable` — a
  TyVar-permissive-at-any-depth predicate that rejects only a concrete-
  vs-concrete leaf clash (`Int` vs `String`), deciding *whether* to
  discharge without side effects.
- **bind (effect):** only on a positive guard, run the real `unify_list`
  that binds the shared TyVars, threading the resulting substitution to
  the next label.

This keeps the `Reader[Int]`-body-under-`Reader[String]`-handler case
rejecting (the guard's leaf clash fails before any binding), so no
partial binding from a non-discharged label contaminates the substitution.

## Structural surprises

- **The issue's diagnosis named the wrong root.** It blamed row-var
  instantiation ("separate tails per occurrence"); instantiation was
  fine. The actual root is in the row-unification *partition*. Worth
  recording: the downstream symptom (`?e6` vs `?e4` in the diagnostic)
  is a red herring — those are two correct lambda-body tails, not
  scheme occurrences.

- **Three bugs, not one, behind a single repro.** Each surfaced only by
  reducing the repro one ingredient at a time (drop the lambda → bug 2
  shows as `Mutable` leak; same-type vars → all bugs hide; one var →
  hide). A faithful repro reduction was the highest-leverage tool.

- **`taggable=false` inside lambdas almost defeated the fix in its own
  case.** The dispatch tag is suppressed in lambdas; if row identity had
  reused that flag the suffix would have vanished exactly where the
  `while` thunks need it. Decoupling row-identity from dispatch-tag was
  load-bearing.

- **Masking the cross-fusion masked a discharge bug.** The shallow
  discharge never converged cell types on its own; the now-removed
  spurious cross-unification did it as a side effect. Removing the muscle
  exposed the skeleton — the discharge had to start doing its real job
  (unify-and-bind).

## Fixtures added

In `examples/sugars/loop/` (wired into `test-issue-248-loop-sugars`,
which runs with `--path ../stdlib` so `import loop` resolves):

- `issue_802_while_disjoint_state_cells.kai` — the minimal `while` with
  `var i` (Int) in the body and `var go` (Bool) in the condition. Output
  `0/1/2`.
- `issue_802_lambda_yield_var_state.kai` — Form A: the lambda producer
  whose `yield` parameter collapsed to `(Bool) -> ?t`. Output `got 0/1/2`.
- `issue_802_stream_alias_var_state.kai` — Form B: the same through a
  `Stream[t, e]` row-parametric alias with a leading `Stdout`. Output
  `start / got 0/1/2`.

`m7b_5b_var_handler_clause` (the #148 var-in-clause fixture) is the
regression oracle for the discharge unify-and-bind change — it failed at
the cell-identity step and passing it again proves the discharge fix.

**Coverage gap:** a dedicated negative fixture for the discharge
guard's leaf-clash (`Reader[Int]` handler, `Reader[String]` body → still
"effect not handled") was scoped but not added — the existing
`Reader[String]`-under-`Reader[Int]` shapes compile on clean `main` too
(the `main : Int` return absorbs the residual), so a clean negative needs
a fn-level declared row to force the clash. Left for a follow-up; the
guard's leaf-clash branch is exercised indirectly by every multi-cell
discharge in the new fixtures.

## Quality / placement

The new logic (~150 LOC: `label_eff_with_alias`, `strip_label_alias`,
`filter_labels_matching_res`, `label_args_dischargeable`,
`ty_dischargeable`, `find_remove_label_unifiable`) lives in `infer.kai`
rather than a fresh A-grade module. Extraction was considered and
rejected: every helper is bound to `Label` / `Ty` / `Subst` / `unify_list`
/ `apply_ty`, all defined in `infer.kai`; a separate module would need to
`import compiler.infer` while `infer` imports it — a cycle kaikai's
module system rejects under `make selfhost`. The helpers are small
match/recurse functions (file avg cogcom stayed 4.8/fn, under the <5
target). Three now-dead functions (`find_remove_label`,
`find_remove_label_loop`, the old `[Label]`-returning
`filter_labels_matching`) were removed.

## Gates

- selfhost byte-id: **OK** (`kaic2b.c == kaic2c.c`) — the hard
  no-regression gate, held across cell-identity + unify-bind discharge.
- `test-sugars`, `test-effects`, `test-loop`, `test-reader`,
  `test-writer`, `test-issue-248-loop-sugars`: green.
- `dual_actor_*` (#532 parametric `Actor[T]`): positive typecheck OK,
  negative still errors with clean `Actor[Reply]`.
- Both issue repros compile and run with the expected semantics.

## Follow-ups left for next lanes

- The negative discharge fixture noted above (fn-level declared-row clash).
- Route A proper (a `Label.origin` field) is the cleaner long-term home
  for cell identity if the emit/kir lanes ever want the tag visible
  downstream; the string-suffix is the typer-pure equivalent and
  sufficient while the suffix stays confined to the typer.
