# Lane experience — issue #1174: the Module theory, Currency, and Money

## Scope as planned vs as shipped

**Planned (issue + spike verdict + folded #1176):** land the spike's
formation guard (`theory Module`, `utable_module_violation` at the
three formation sites), add the cross-kind generic-instantiation
kind-check, declare `kind Currency : Module with currency` with
habitants, and rebuild `money.kai` on `Decimal<c>` with scalar
`Mul`/`Div` via protocols. Close the spike's two holes (aliases
post-expansion, library entry points).

**Shipped:** all of the above, plus four pieces the brief did not
pre-map because they only became visible on contact:

1. **Unit discipline through protocol binops.** The `__proto_<op>`
   dispatchers are typed `(?t0, ?t1) -> ?t0` — two *independent*
   tvars (deliberately, for heterogeneous impls), so
   `Decimal<USD> + Decimal<EUR>` type-checked and silently swallowed
   the EUR unit; `*` erased units entirely. Without fixing this, the
   whole Money story is fiction: the compile-time guarantees exist on
   `Real` but not on the carrier money actually uses. `synth_binop`
   now enforces the primitive dim rules on same-head dimensioned
   operands before dispatch: `+`/`-` unify the full types (unit
   equality), `*`/`/` combine the units, run the Module guard on the
   combined unit, and stamp it back on the impl's result.
2. **Unit-kind tparams on type aliases.**
   `type Money[c: Currency] = Decimal<c>` did not exist: alias
   expansion substituted type positions only, and the alias body's
   `c` failed unit validation. Shipped: tparam-aware substitution in
   unit position, alias-decl tparams in the validator's scope, and a
   post-expansion re-resolve + re-validate pass in the driver (gated
   on the first validation being clean) so `Money[USD]` re-qualifies
   its habitant and `Money[Zorro]` diagnoses at the use site.
3. **`__attach_unit` / `__detach_unit` intrinsics.** Constants mint
   via the existing `Decimal<u>` literal lowering, but runtime values
   (`money.parse`, `money.convert`) need a tag/untag door. Both are
   identity at runtime (same emit shape as `__strip_unit`), typed
   kind-agnostic (`(t) -> t<?u>` / `(t<?u>) -> t`).
4. **`core/kinds.kai` was never auto-loaded by kaic2.** `bin/kai`
   globs `stdlib/core/*.kai`; the compiler's internal core list did
   not include the catalog, so the catalog's decls (including
   `Currency`) were invisible to harness-driven raw `kaic2` runs.
   Added to `core_module_files()`. The parser side is covered by
   hardcoding `currency`/`Currency` in `kind_reg_base()` — the same
   catalog-mirrors-builtin pattern `unit`/`Measure` already uses,
   necessary because the parser's kind registry is a per-file
   pre-scan and money.kai's habitants live in a different file from
   the kind declaration.

## Design decisions

- **No `unify_module`.** Confirmed the spike's verdict end to end:
  `unify_abelian` untouched. The Module semantics are the formation
  guard (three sites) plus the pre-existing abelian isolation.
- **Cross-kind check at the abelian pivot.** `TyScheme` gained a
  per-uvar kind list (parallel to `bound_uvars`; `""` =
  unconstrained, used by kind-agnostic intrinsics); instantiation
  seeds `Subst.unit_var_kinds`; `unify_abelian_diff`'s pivot bind
  consults it — a habitant of another kind refuses the bind (surfacing
  as the ordinary unification mismatch), and a var-to-var bind
  propagates the constraint. This kills the spike's "generic
  instantiation back door" (`sq[u: Measure](3.0<USD>)` minting
  `USD^2`) and benefits every kind, not just Module: a Metric
  habitant no longer binds a Measure tparam either.
- **Failure UX is the ordinary type mismatch.** The pivot gate
  returns `None`; the existing call-site diagnostics render
  `expected (Real<?u1>) / found (Real<USD>)`. No new diagnostic
  channel inside unify.
- **Scalar mult stays in protocols.** `Money * Decimal` routes
  through `impl Mul for Decimal` with the unit combined outside the
  impl; nothing kind-specific in the dispatch.
- **`Money[m]` (wrong-kind alias argument) is accepted** and expands
  to `Decimal<m>` with Measure semantics. The alias's tparam kind is
  erased at expansion; enforcing it would need kind-context threading
  through the whole `expand_ta_*` family for a shape the value-level
  isolation already contains. Documented, not enforced.

## Structural surprises

- The pre-existing `Decimal<u>` UoM literal lowering
  (`try_lower_decimal_uom_lit`) emitted the record's `raw` as a bare
  `EInt` where the carrier field is `Int128` — every *executed* use
  trapped at runtime with `kai: type mismatch in *`. No fixture ran
  it. Fixed with an `EFixed`-typed raw (the shape `mk_decimal_lit`
  already used).
- String-interpolation fragments (`#{...}`) are re-parsed fresh at
  typing time and never pass through `resolve_kinds_decls`, so a
  unit literal of a *user* kind inside an interpolation stays bare
  and — since kinds v1 — cannot unify with its qualified form.
  Pre-existing (#1107-era) gap; the cross-kind gate makes one more
  shape visible (`print("#{sq(3.0<X>)}")`). Follow-up material, not
  fixed here: needs the kind context threaded to the fragment parse.
- `let`/`var` annotations were never unit-validated (only the RHS
  expression); `let a: Real<zzz>` sailed through to a unify error.
  Now validated (needed so `Money[Zorro]` reports "unknown unit").
- The sugars harness greps `.err.expected` lines as *regex* — an
  `operator `*`` substring silently never matches. Golden lines
  avoid metacharacters.

## Fixtures

- `examples/sugars/kinds_module_guard_err` — annotation-side guard
  (`Real<USD^2>`).
- `examples/sugars/kinds_module_ops_err` — operator-side guard
  (`*`, `^`, `/` → `USD^2`, `1/USD`).
- `examples/sugars/kinds_module_scalar` — additive ops, scalar
  action, cancellation ratio stay green.
- `examples/sugars/kinds_cross_kind_bind_err` — Module habitant into
  a Measure tparam rejected (the folded #1176 fixture).
- `examples/sugars/kinds_unit_tparam_same_kind` — same-kind binds
  stay green (Measure and user AbelianGroup).
- `examples/sugars/kinds_alias_unit_param` (+
  `kinds_alias_unknown_arg_err`) — unit-kind alias tparams.
- `examples/stdlib/money_basic`, `money_convert` — same-currency
  arithmetic, scalar action, ratio, parse/convert round-trip.
- `examples/stdlib/money_cross_err`, `money_squared_err` — USD+EUR
  and USD·USD as compile errors on the Decimal carrier.

## Coverage gaps / follow-ups

- Interpolation-fragment kind qualification (above).
- `Money[m]` wrong-kind alias argument (above).
- LSP/library entry points now derive Module kinds from their decl
  stream (`infer_program_with_protos`), but that path does not run
  habitant qualification; bare habitants there mean the guard keys
  on nothing. Consistent with that path's general kind support.
- `money.convert`'s rate is a bare `Decimal`; a typed
  `Rate[from, to]` is inexpressible as a unit (`USD/EUR` is exactly
  the product Module forbids) and would need its own nominal type.

- The serial backend-parity sweep surfaced one failing fixture
  (`demos/vs/python`, a C-oracle build failure in a map-pipe lambda's
  deferred field). It reproduces identically with a fresh kaic2 on
  main — pre-existing, filed as #1186, not introduced here.

## Cost vs estimate

The spike's "the lane is direct" held for blocks 1 and 3; the two
unplanned compiler gaps (protocol-binop unit erasure, alias unit
tparams) roughly doubled the compiler-side surface. The money.kai
module itself was the cheapest block, exactly as predicted.
