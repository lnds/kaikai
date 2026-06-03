# Lane experience — issue #743: generic effect op payload type never bound to row's ty_args

## Scope as planned vs as shipped

Planned (from the issue's suggested fix): close the loop at the
instantiation site in `synth_op_call_with_scheme_keys` — unify the
op's fresh effect tyvars positionally against the enclosing row's
concrete `ty_args`, so the return payload of a generic effect op
(`Repo.find : ∀a,i. i -> Option[a]`) resolves to a concrete type
before a downstream field access.

Shipped: exactly that, but the row the unification reads from is **not**
the one the issue named. The issue said to unify against "the row in
scope (`/ Repo[Box, String]`)" and pointed at `eff_fresh` + the row's
`ty_args`. The trap: `InferState.row` is the *inferred* accumulator,
which is **empty** at the op-call site and only fills as the body is
walked. The declared signature row is never seeded into it (the typer
checks the body row as a *subset* of the declared row at the end, in
`check_body_row` — it does not pre-seed). So reading `st.row` found
nothing and the fix was a no-op. The first build compiled cleanly and
changed zero behaviour, which is the most dangerous kind of green.

The real source of the concrete `ty_args` is the **declared row** of
the enclosing function, which lived only as an AST `RowExpr` argument
to `infer_decl` and was never threaded into `InferState`. Shipping the
fix required adding that channel.

## Structural surprise the brief did not anticipate: a new InferState field

The issue's attribution was precise about the *defect* (fresh-and-free
instantiation, payload tyvar never bound) and the *fix shape* (unify
`eff_fresh` against the row's `ty_args`). What it missed is that the
material it assumed was in hand — "the row in scope" — is not in
`InferState` at the op-call site at all.

`InferState` already carries `ret_ty` (the declared return type,
seeded by `infer_decl` before walking the body, cleared inside
lambdas). The declared *row* is the exact analogue and simply was
never added. So the lane added `decl_row: [Label]` alongside `ret_ty`,
seeded the same way (`st_set_decl_row` in `infer_decl`, resolved under
the decl's tparam/row-var binds via `decl_row_labels_with_binds` so a
row arg that names the fn's own tparam — `fn f[a]() : … / Reader[a]` —
resolves to that bound tyvar rather than dropping). `synth_lambda`'s
state-merge inherits `outer.decl_row` (a generic op called inside a
closure still sees the enclosing fn's declared row — the right call,
matching how the row is the closure's effect context even though
`ret_ty` is cleared for `!`-propagation reasons).

Cost of the new field: ~20 `InferState` constructors had to learn the
slot. A `perl` one-liner over the exact closing pattern
`, diags: st.diags }` covered 15; three special cases (the initial
constructor with literal `[]`, the `[d, ...st.diags]` diag-push
variant, and the multi-line `synth_lambda` merge) were edited by hand.
This is the standing tax on `InferState` being a flat record threaded
by copy — every new piece of decl-scoped context pays it.

## Why the fix is additive (the load-bearing safety argument)

The unification only fires when `row_fixed_ty_args(decl_row, eff, arity)`
returns `Some` — i.e. the declared row names this effect with a
matching ty_arg arity. When it does, the declared row is ground truth
the signature already committed to, so binding `?a = Box` cannot
reject anything that was previously sound. When it returns `None`
(genuinely polymorphic call site, open/absent row, stateless effect
with empty ty_args), nothing happens and the prior free behaviour is
preserved verbatim. Verified against `poly1b` (`fn get[a]() : Option[a]
/ Repo[a, String]` — the row fixes a *tparam*, not a concrete type;
the fresh payload binds to the tparam, which is correct) and `poly2`
(two concrete instantiations of one generic effect in two functions —
each binds independently).

## The Tier 1 flip is the real proof

The noisy half of the bug (Bug B: the bogus
`` `id` is a function in scope, not a field `` UFCS diagnostic) is the
*visible* symptom, but it is secondary. The load-bearing fix is the
silent half: with the receiver type left free, a field access on a
generic-effect payload **bypassed the type checker entirely**. The
negative fixture (`b.zzz`, a nonexistent field) is the canary — before
the fix it compiled with exit 0 and only failed at runtime
(`kai: no such field zzz`); after the fix it is rejected at compile
time with `no field 'zzz' on Box`. That flip from "compiles silently"
to "rejected on the concrete type" is the evidence the Tier 1
(safe-at-compile-time) hole is closed, independent of the diagnostic
improvement. The fixture asserts on `on Box` specifically — the
substring proves the receiver reached its concrete type.

Note on the issue's isolation table: row T4 claimed `b.zzz` "compiles
silently" on `main`. On this checkout (0.86.1) it compiled and *ran* to
a runtime field panic. Same root cause, same Tier 1 escape (the typer
accepted it); the issue's wording predates a runtime field-lookup that
now panics instead of returning garbage. The flip the fixture pins is
the compile-time rejection, which is the actual contract.

## Diagnostic relationship to proposed-extensions #12

`docs/proposed-extensions.md` §12 (`f.field` is always field access;
UFCS needs `f.method()`) is the complementary *language-surface*
proposal that would remove the ambiguity class producing Bug B's
misleading message by construction. It is an edition-boundary change
and is **out of scope here** — the owner's comment on #743 is explicit:
"Land the typer fix regardless; the proposal lands separately when an
edition window opens." This lane closes the Tier 1 escape (T4/T5),
which exists even with no name collision; the proposal closes the
diagnostic ambiguity. No edition bump, no resolver change, no
`proposed-extensions.md` edit was made — the proposal stands as
written.

## Fixtures added

- `examples/effects/issue_743_generic_effect_payload_field.kai`
  (+ `.out.expected` = `found`) — positive: the full triple condition
  (generic effect + field `id` + same-named `id` binding in scope),
  type-checks and runs on both backends.
- `examples/effects/issue_743_generic_effect_payload_bad_field.kai`
  (+ `.err.expected` = `no field 'zzz' on Box`) — negative: the Tier 1
  escape canary; must be rejected on the concrete type.
- `stage2/Makefile` target `test-issue-743` (wired into `test` and
  `.PHONY`): positive C + LLVM run-diff, negative reject + diagnostic
  grep.

Coverage gap: no fixture exercises a generic effect op called in a
genuinely polymorphic context (the `None` branch of the fix). The
`poly1b`/`poly2` cases were validated by hand during the lane but not
pinned as fixtures — they belong to the typer's general row machinery,
not this bug shape. Left as a note rather than a fixture to keep the
lane scoped to the reported defect.

## Load-bearing sites touched

- `stage2/compiler/infer.kai` — `synth_op_call_with_scheme_keys`
  (the unification step, reading `decl_row`); new helpers
  `row_fixed_ty_args` / `row_fixed_ty_args_loop` (find the fixing
  label), `unify_eff_fresh_with_row` (positional unify),
  `decl_row_labels_with_binds` / `decl_row_labels_loop` (resolve the
  declared `RowExpr` under tparam binds), `st_set_decl_row` (setter);
  `InferState.decl_row` field + ~20 constructor updates;
  `infer_decl` seeds `decl_row` next to `ret_ty`.

## Cost vs estimate

Estimate-by-gate (per project discipline, no time units): bounded by
(1) selfhost determinism green, (2) the isolation table from the issue
reproduced as the verification gate, (3) full `make test`. The real
surprise cost was the dead first build — the issue's "row in scope"
phrasing sent the fix at `st.row`, which is empty there. Recognising
that `decl_row` had to exist (and was already prefigured by `ret_ty`)
was the actual work; the unification itself is ~15 lines.

## Follow-ups left for next lanes

- proposed-extensions #12 remains open as the edition-boundary change
  that retires Bug B's ambiguity class. Nothing in this lane advances
  or blocks it.
- The `InferState` flat-record constructor tax recurs on every
  decl-scoped field. If a third such field lands (`ret_ty`, now
  `decl_row`, …), consider a `DeclCtx` sub-record threaded as one slot
  rather than N parallel fields. Not worth it for two.
