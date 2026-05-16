# Lane experience — issue #645 (row subsumption for pure callbacks)

Closes #645. Unblocks ahu Layer 1 (`stream.kai`), kohau pipelines,
henua predicates, manutara filters.

## Scope as planned vs. shipped

**Planned** (from `/tmp/wt-645-brief.md`): permit a pure named function
to flow into a slot expecting `(args) -> ret / e` without forcing the
caller to write `[e] / e` boilerplate. Path: elaboration in the
parser/resolver/typer signature pass — *not* the unifier. Preserve the
asymmetry that effectful → pure stays rejected.

**Shipped**: exactly the elaboration path. ~30 LOC of typer change
(plus a 4-line companion in `infer_decl` to keep the row-fresh counter
in sync, and a ~10 LOC pretty-print adjustment in
`row_to_string_suffix`), no unifier modification, no constraint
propagation, no breaking changes to row syntax.

The unifier is unchanged. `docs/effects.md` §*Subsumption: none* still
holds verbatim — kaikai still has no subsumption rule. What the lane
adds is a generalisation rule: a fn signature that omits the `/` row
clause gains a fresh phantom row variable in its scheme. The
operational effect from the caller's perspective looks like
`ε ⊑ e`, but the mechanism is generalisation, not subsumption.

## Design decision

Two routes were on the table:

1. **Elaboration** (validated by asu in pre-flight consult). In
   `fn_scheme_of_decl`, when `rexpr == REmpty` and the decl is not
   `main`, allocate one extra bound row-variable id (`phantom_id =
   len(rbinds)`), construct the body `TyFnT` with that id in the
   outermost tail, and quantify the scheme over it. The body
   inference still verifies the body's row is empty-closed
   (`check_body_row`'s REmpty branch); only the call site sees the
   phantom. Each call site instantiates the scheme with a fresh free
   row variable, which unifies with whatever the slot demands.

2. **Unifier-level** (fallback, escalation gate). Modify `unify_ty`
   to special-case "actual = closed empty row" against "expected =
   open row" by leaving the expected row free. This would have to be
   recursive into function types, and risks soundness regressions on
   the entire selfhost path. The brief required a STOP-AND-ESCALATE
   gate before touching this.

Route 1 was viable and shipped. The escalation gate was never reached.

### Why route 1 is sound and decidable

- **Soundness, pure → effectful direction**: a body certified empty
  by `check_body_row` cannot perform any effect. Passing such a value
  to a slot expecting `(args) -> ret / Stdout` is safe: the value
  silently ignores the row context. The phantom row var is the
  declarative statement "this callee is agnostic to the row it is
  invoked under," which matches reality.
- **Soundness, effectful → pure direction (preserved)**: signatures
  with `RClosed([Stdout])` do NOT trigger elaboration. Their scheme's
  body type carries the closed `{Stdout}` row, which fails to unify
  with an empty-closed row at the call site. `logger : Unit / Stdout`
  passed where `(String) -> Unit` is expected stays rejected
  (regression-guarded by `examples/negative/subsumption/effectful_to_pure_rejected.kai`).
- **Decidability**: no constraint propagation, no rank-2 types, no
  type-class dispatch. A row var added to a scheme is no different
  from a row var the user wrote `[e]` for explicitly. HM with rows
  remains decidable.
- **Monomorphisation cost** (asu's flagged risk D): `MonoTuple = MT(name,
  [Ty], [UnitExprT])` already excludes row vars from the key. Two
  call sites to `even` under different row contexts produce a single
  monomorphisation, not two. No code-size blow-up.

## Fix site

`stage2/compiler.kai` lines ~27340–27420 (`fn_scheme_of_decl`):

```kai
let phantom_id = list_length(rbinds)
let row = if elaborate_phantom_row(rexpr, fname) {
  Row { labels: [], tail: Some(phantom_id), display_alias: None }
} else {
  row_of_expr_with_binds(rexpr, tpbinds, rbinds)
}
let body = TyFnT(pts, ret, row)
let split = tparam_id_split(effective_tparams)
let rvars = if elaborate_phantom_row(rexpr, fname) {
  list_append(rvbind_ids(rbinds), [phantom_id])
} else {
  rvbind_ids(rbinds)
}

fn elaborate_phantom_row(rexpr: RowExpr, fname: String) : Bool {
  match rexpr {
    REmpty -> fname != "main"
    _      -> false
  }
}
```

Companion edit in `infer_decl` (~36509): advance the row-fresh
counter by one extra when elaboration fires, so future fresh row
vars inside the body cannot alias the phantom id.

```kai
let phantom_extra = if elaborate_phantom_row(row, nm) { 1 } else { 0 }
let st0 = advance_row_fresh(st_init, list_length(rbinds) + phantom_extra)
```

`main` is excluded because `check_body_row`'s REmpty branch already
absorbs builtin effects under main; introducing a phantom row var
there would conflict with that special case (and main has no callers
in user code anyway, so elaboration would have zero benefit).

Companion edit in `row_to_string_suffix` (~25320): when a row has no
labels (regardless of tail), pretty-print as the empty suffix. This
prevents the phantom row variable from leaking into `--type-at`
probes, `--types` dumps, and surface diagnostics as a confusing
`?eN` annotation. Genuine row mismatches still print full labels +
tail because both rows carry concrete labels at the diagnostic
site. The change is symmetric: a fresh row var with zero labels is
"no effect demanded yet" by any reasonable display semantics.

## How the asymmetry is preserved

The elaboration predicate `elaborate_phantom_row(rexpr, fname)` only
fires for `REmpty`. A signature like `fn logger(s: String) : Unit /
Stdout = ...` has `rexpr == RClosed([Stdout])` and follows the
unchanged code path: its scheme carries a body type with a closed
`{Stdout}` row. When such a value flows into a slot expecting `(String)
-> Unit` (i.e. closed-empty), unification of `{Stdout}` against `ε`
fails — and the diagnostic still reads "type mismatch in function
call" (see `examples/negative/subsumption/effectful_to_pure_rejected.kai`).

The narrow direction lifted: `ε ⊑ any open row` (pure callee → slot
demanding effects).
The hard direction remains forbidden: `Stdout ⊆ ε` (effectful callee
→ slot demanding purity).

## Why decidability holds

The new bound row var per REmpty signature is structurally
indistinguishable from a row var the user wrote `[e]` for. The
inference algorithm (HM extended with row inference à la Rémy/Leijen
+ tail-variable binding) handles them identically. No new constraint
type, no propagation, no rank-2 polymorphism, no HKT.

## Fixtures shipped

Positive (under `examples/effects/subsumption/`):

1. `pure_to_effectful_via_higher_order.kai` — `double` (REmpty) into
   `apply_io[e]` slot. Minimal proof the elaboration fires.
2. `pure_to_effectful_named_callback.kai` — `add(a, b)` into
   `apply[e]` slot — the `apply(add, 1, 2)` shape from the brief.
3. `chained_pure_callbacks.kai` — three levels of higher-order
   chaining with three different pure named callbacks (`inc`,
   `double`, `even`).
4. `mixed_pure_and_effectful_in_same_pipeline.kai` — `even` (pure) +
   inline lambda calling `println` (effectful) in the same pipeline.
   The RFC's central dolor reproduced and passing.
5. `inline_lambda_still_works.kai` — regression guard for inline
   lambdas, which already worked pre-fix (their row was inferred
   from context, not from a stored scheme).

Negative (under `examples/negative/subsumption/`):

6. `effectful_to_pure_rejected.kai` (`.diag.expected`) — `logger : Unit
   / Stdout` rejected when slot expects `(String) -> Unit`. Picked up
   by `tools/test-negative.sh`.

Updated:

- `examples/effects/row_pure_to_effectful.kai` — was a negative
  fixture under m7a #2 ("pure cannot become effectful"). Repurposed
  as a positive fixture documenting the new behaviour. Sister
  fixture `row_mismatch_higher_order.kai` (effectful → distinct
  effectful) stays negative, guarding the asymmetry.
- `stage2/Makefile` `test-effects` target — split the negative run
  into two loops; `row_pure_to_effectful.kai` now runs in the
  positive loop.
- `examples/stdlib/byte_int_mismatch.err.expected`,
  `examples/stdlib/m12_5_uom_real_decimal_disjoint.err.expected` —
  trimmed the brittle `found:` line. The pretty-print change suppresses
  the row var in the empty-labels case, which strips the trailing
  `/ ?eN` even from these mismatch diagnostics. Stable lines
  (`expected:`, `type mismatch in function call`) are retained.

## Selfhost byte-identical

`make -C stage2 selfhost` → `self-hosting fixed point: OK`. The
elaboration is structurally additive: signatures that already had a
row clause (the prelude / stdlib use `[e] / e` heavily) are
unaffected. Only previously-empty rows gain a phantom id, which is
quantified out at the scheme boundary and never appears in emitted
code.

## Tier gates

- `make tier0`: OK (selfhost byte-identical, 33 demos baseline).
- `make tier1`: OK (after the three golden-file updates above).
- Tier 1-ASAN: deferred to CI.

## Real cost vs estimate

The brief estimated ~50–150 LOC. Actual: ~25 LOC in
`fn_scheme_of_decl` + 4 LOC in `infer_decl` + 6 fixtures + 3 golden
updates + Makefile split. The fix is small because the existing
infrastructure (`mk_rvbinds`, `row_of_expr_with_binds`, the
rvar-aware instantiation in `st_instantiate_report`) already
supported the abstraction; the lane only had to allocate one extra
bound id per qualifying decl.

## Follow-ups

- **Diagnostic phrasing**: the rejection message for `effectful → pure`
  still reads "type mismatch in function call". Users seeing a pure
  callee accepted but an effectful callee rejected may want a
  positive hint ("the callee declares effects this slot does not
  allow"). Out of scope for this lane.
- **Effect-op signatures**: if an `effect E { op : T }` declares an
  op without a row, the lowering pipeline currently never reaches
  `fn_scheme_of_decl` for that op (effect ops live in `DEffect`,
  not `DFn`). Leaving the elaboration scoped to `DFn` is correct;
  noted here so a future op-shape unification lane does not regress.
- **Public-signature stability**: the lane changes how kaikai *displays*
  the scheme of a pure named callback in diagnostics (an extra `?eN`
  may appear). Editions discipline: this is not a breaking change to
  user code (every program that compiled pre-fix still compiles
  post-fix), but it is a visible change to error messages — within
  edition Tongariki this is fine; we should document the relaxation
  in `docs/effects.md` (as a sidebar paragraph, not editing the
  *Subsumption: none* section).

## Doc updates pending in this PR

`docs/effects.md` — append a short sidebar paragraph after the
*Subsumption: none* section noting that pure named callbacks
(REmpty signatures) now flow into slots demanding any row, by
generalisation; the asymmetry stays.
