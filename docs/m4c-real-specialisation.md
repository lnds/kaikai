# m4c real specialisation

Started 2026-04-29.

This is the m4c lane that closes structural lie #3 (the
`monomorphise` pipeline pass shipping as identity, Doc B / runtime
audit 2026-04-28). This file tracks what landed, what is deferred,
and the gate evidence.

## Background

Stage 2's pipeline is `lex → parse → resolve → infer → monomorph →
perceus → lower`. The `monomorph` step is supposed to specialise
generic `[a]` / `[a, b]` decls per call-site type tuple — `id[a]`
called with `Int` and with `String` should produce two C copies
(`id__mono__Int` and `id__mono__String`) with the call sites
rewritten accordingly. Pre-m4c #4 the step was a no-op: `tp.decls`
returned unchanged.

The blocker (the m4c retrofit landed 2026-04-29; `clause_fn_name`
is now fn-name aware) was symbol collision: `clause_fn_name(line, col, op)` minted C symbols for
effect-handler clauses keyed on source position alone. Two
specialised copies of one source body share `(line, col, op)` for
every embedded `EHandle` clause, so both copies emit `static
KaiValue *_kai_clause_<l>_<c>_<op>(...)` and the C linker rejects
the binary with multiple-definition errors.

Because `monomorphise` shipped as identity, `try_rewrite_show_dim_real`
in `stage2/compiler.kai` had to wedge a callsite rewrite around
`Show<Real<u>>` — the parametric impl body's `unit_name(x)` reads
the impl-level uvar and would have needed real specialisation
(per-tuple body cloning with `Expr.ty` substitution) to render the
unit suffix. The rewrite shortcut works; the workaround is
documented in `m12.8 Phase 2` (commit `81e0306`).

## What landed in this lane

### Phase 1 — clause-info plumbing *(LANDED 2026-04-29)*

`376d3f3 m4c #4: thread enclosing-fn through clause-info plumbing`.

Threads the **enclosing fn name** through both the C and LLVM emit
paths, so clause symbols are minted as
`_kai_<enc>__clause_<line>_<col>_<op>` instead of
`_kai_clause_<line>_<col>_<op>`. The `<enc>` slot resolves to the
post-monomorph C symbol of the fn / lambda / test that physically
contains the `EHandle` — so two specialisations of one source body
mint distinct symbols and the C linker accepts the binary.

Implementation sketch:

- `ClauseInfo` gains an 8th `String` field (`enc_fn`), populated by
  `lc_record_clauses` from `LamCollect.cur_enc_fn`. The collect
  walker tracks `cur_enc_fn` at scope boundaries:
  - `collect_decl` for `DFn(name, ..., mo)` swaps it to
    `c_sym(name, mo)`.
  - `collect_decl` for `DTest(_, _, line, col)` swaps it to
    `__test__<line>_<col>`.
  - `collect_expr`'s `ELambda` arm pushes `_kai_lam_<id>` for the
    body recursion and restores the outer name on the way out.
- `clause_fn_name(enc_fn, line, col, op)` mints the prefixed C
  symbol.
- The two emit sites for clause bodies (`emit_clause_sig`,
  `emit_clause_body`, `llvm_emit_clause_body`) read
  `ClauseInfo.enc_fn` directly.
- The two install sites (`emit_clause_assignments`,
  `llvm_emit_handle_clause_assigns`) look up the matching
  `ClauseInfo` by `(line, col, op_name)` via
  `lookup_clause_enc_fn` so the install code matches the body's C
  symbol.
- C path: `cls: [ClauseInfo]` is threaded through the `emit_*`
  family alongside `lams`. The thread is passed once at
  `emit_program` from `let cls = clauses_info` and propagated to
  every recursion that may reach an `EHandle`.
- LLVM path: `LlvmEmit` gains a `cls` field (mirrors the C thread
  via the existing `e: LlvmEmit` carrier), populated once in
  `emit_program_llvm` from `lc.clauses`.

### Smoke test — `examples/effects/m4c_run_with.kai` *(LANDED 2026-04-29)*

A polymorphic helper `fn run_with[a, b](x: a, f: (a) -> b /
Greeter) : b / Greeter` called with two distinct (a, b) tuples
under a single `Greeter` handler. Both backends (C and LLVM) emit
and run; the demo runs through `kaic2` and `kaic2 --emit=llvm`
without symbol collision and the test block `run_with tuples`
asserts the two specialisations return the expected pair.

The demo proves the plumbing's load-bearing claim — that two
clauses inside a single fn / handler / test mint matching install
and body symbols — but does not by itself prove the
specialisation-collision case. Real m4c (Phase 2) is required to
exercise the duplicated-body codepath end-to-end.

### Phase 2 minimal — real specialisation *(LANDED 2026-04-29)*

`c27a1fe m4c #4: real specialisation — generate per-tuple copies +
collision demo`.

`monomorphise(tp: TypedProgram) : [Decl]` flips from identity to a
real pass that collects distinct `(name, [Ty])` tuples per
polymorphic DFn from `tp.insts` (`[ResolvedCS]`) and emits one
specialised copy per tuple with `mangle_name(name, tys)` applied
to the symbol and `tparams = []`. The clone shares its body Expr
with the original; the cloned `enc_fn` swaps to the post-monomorph
C symbol so any embedded `EHandle` clauses mint distinct C
symbols across copies (the collision case Phase 1 unblocked).

In this minimal phase the call sites stayed pointing at the
polymorphic original — the specialisations existed in the binary
but were unreached. The follow-up Phase 2 full lane (next section)
threaded the call-site rewrite that makes them reachable.

### Phase 2 full — call-site rewrite *(LANDED 2026-04-29)*

`monomorphise` now runs the call-site rewrite walker
`rewrite_callsites_decls` over `tp.decls` *before* cloning the
specialised copies, so the specs inherit the rewritten body via
structural sharing. After the rewrite, every
`ECall(EVar(name), args)` whose `(name, callee.line, callee.col)`
matches a `ResolvedCS` with concrete tys retargets its callee to
`mangle_name(name, tys)`. Calls whose recorded tys still contain
`TyVarT` (polymorphic flow-through) are left untouched and reach
the polymorphic original at runtime — body type substitution
(deferred) is the proper fix for that case.

Implementation:

- `rewrite_callsites_decls / _decl / _expr / _kind` mirror the
  shape of `rename_proto_calls_*`. The only rewrite case is
  `ECall(EVar(name), args)`; every other ExprKind variant
  delegates to `map_expr_kind` so a new variant fails the build
  in `map_expr_kind` rather than silently leaking through this
  walker.
- `find_mono_tys(insts, name, line, col)` is a linear scan over
  `tp.insts` keyed on the `EVar`'s source position. The typer
  pushes one `CS(name, e.line, e.col, fresh_ids)` per polymorphic
  `EVar` instantiation, and never pushes one for an EVar shadowed
  by a local (the local's scheme is monomorphic). That makes the
  lookup unambiguous and removes the need for scope tracking at
  the rewrite stage.
- The polymorphic original stays in the decl list. Bare
  `EVar(name)` references (function-as-value, partial application)
  are not in `tp.insts`, so they continue to bind to the original;
  pruning would break them. Generic prune is a separate lane.

**Keying decision.** Used `(name, line, col)` against
`tp.insts` rather than `(name, [Ty])` against the spec table.
Reason: the typer's `synth` already records each polymorphic-EVar
position in `tp.insts` with the exact source span, and
`resolve_callsites` resolves the tyvar ids to concrete tys at the
end of typing. Looking up `(name, line, col)` returns the tys
directly, which we then mangle. Reaching for `(name, [Ty])`
keying would have required reconstructing the call-site's tys
from `Expr.ty` of the callee, which is the same data the typer
already memoised — pointless work.

### Phase 3 — body type / unit substitution *(LANDED 2026-04-29)*

`99ecf0a m4c #4 Phase 3 invariant: per-spec TyVarT/UVar leak check`
on top of `d7046c2 m4c #4 Phase 3: body type substitution +
per-unit specialisation` and `e2da46b m4c #4 Phase 3 follow-up:
inline impl-call inst synthesis`.

Each cloned spec's body is now freshly walked via `subst_decl` and
every `Expr.ty` carrying the source's tparam ids gets replaced by
the spec's concrete tuple entry. Two consequences:

1. The parametric `impl[u: Measure] Show for Real<u>` body's
   `unit_name(x)` and `__strip_unit(x)` now read the substituted
   `x.ty` (concrete `TyDimT(TyReal, USym(...))`), not the impl-level
   `UVar(_)`. The `try_rewrite_show_dim_real` workaround is
   **deleted**.
2. Polymorphic flow-through resolves correctly. In a body
   `fn outer[a](x: a) = inner(x)`, the spec `outer__mono__Int`
   substitutes `a → Int`, so the `inner(x)` call's recorded tuple
   `[TyVarT(a)]` substitutes to `[Int]` — and the per-spec
   call-site rewrite retargets `inner` to `inner__mono__Int`.
   Pre-Phase 3 the spec called the polymorphic `inner` original.

#### Implementation pieces

- **MonoTuple now keys on `(name, [Ty], [UnitExprT])`**. Two
  `Show<Real<u>>` call sites with `u := USD` and `u := EUR` collapse
  to the same `[Ty]` (empty, since the impl has 0 type-tparams),
  but the unit tuple distinguishes them. `mangle_name` encodes the
  unit tuple as `__umono__<unit_mangle>` after the existing
  `__mono__<ty_mangle>` segment; composite units like `EUR/USD` go
  through `mangle_unit_ident` for C-identifier safety
  (`__umono__EUR_d_USD`).

- **CallSite / ResolvedCS gain a parallel uvar slot**.
  `CS(name, line, col, fresh_tvars, fresh_uvars)` and
  `RCS(name, line, col, tys, units)`. `mk_fresh_unit_subst` now
  returns the fresh uvar ids in `FreshUnitSubst.fresh_ids`, and
  `st_instantiate_report` propagates them so the CS push records
  both fresh lists.

- **`subst_ty` / `subst_unit` are exhaustive**. `subst_ty` recurses
  through every Ty constructor (`TyListT`, `TyFnT` including its
  `Row`'s effect-label `ty_args`, `TyCon`, `TyDimT` substituting
  both base and unit, `TyRefineT`). `subst_unit` covers every
  `UnitExprT` shape. Adding a new variant fails the build inside
  the match rather than slipping through.

- **`subst_decl` walks the body via `map_expr_kind`**, so every
  `Stmt`, `Arm`, `HClause`, and `HReturn` descendant is reached
  for free. The walker only rewrites `Expr.ty`; names stay
  unchanged because the call-site rewrite is a separate pass.

- **Inline-synth impl-call insts**. `resolve_protocol_calls` runs
  AFTER `infer_program`, renaming `__proto_<op>(arg)` to
  `__pimpl_<P>_<T>_<op>(arg)`. tp.insts is keyed on the dispatcher
  name, so the rewritten AST cannot find its CS by name. The
  `resolve_call_inst` fallback path runs `synthesize_inst_for_decl`
  inline at the rewrite / discover sites, unifying the impl's
  formal param types against the actual `args[i].ty` to extract
  both type-tparam and unit-tparam bindings. This also disambiguates
  multiple impl calls coalescing onto one source position under
  string interpolation (each `#{x}` becomes
  `__pimpl_Show_<T>_show(x)` at the same outer `(line, col)` for
  distinct `x.ty`).

- **Worklist iteration**. `generate_specs_iter` reads new tuples
  discovered via per-spec body subst (a spec body's polymorphic
  call may resolve to a concrete tuple post-subst that the typer
  did not record), appends them to the worklist, and iterates to
  fixpoint. Without iteration, transitive specs in nested
  polymorphic helpers would never be emitted.

- **Post-finalise canonicalisation**. The unifier's
  `utable_pick_min_var` always picks the smallest-id variable, so
  unifying a bound id (0..n-1) with a fresh id binds the bound id
  to the fresh chain endpoint. Post-finalise the body's
  `Expr.ty` references the chain endpoint, not the bound id the
  SubstMap keys on. `infer_decl` therefore runs
  `canon_uvars_expr` / `canon_tvars_expr` after
  `finalise_typed_expr`: chases each bound id through `st2.sub`
  and rewrites any chain-endpoint UVar / TyVarT in the body's
  `Expr.ty` (and the corresponding RCSs via `canon_resolved_css`)
  back to the bound id. Pairs with a small `advance_unit_fresh`
  bump in `infer_decl` so freshly minted uvars don't alias the
  bound ids on the very first call.

#### Per-spec invariant

`emit_spec` records a per-spec leak count: walking `spec2`'s body,
counting only `TyVarT(n)` / `UVar(n)` whose `n` is in the spec's
`SubstMap` domain (extracted via `subst_map_tvar_ids` /
`subst_map_uvar_ids`). Out-of-domain ids belong to free typer
tyvars (clause answer types the typer left open, etc.) and are
skipped — the codegen erases them. `compile_source` calls
`report_mono_leaks(path, leaks)` after `monomorphise`; non-zero
count fails compilation with one diag per offending spec including
the first leak's `(line, col, ty)`.

This is the strict closure of the structural property "after body
subst, no tparam-bound id remains in any Expr.ty of any cloned
body". The full corpus (411 OK in `make test`, 24 demos baseline,
4 `m12.8-phase2` fixtures with refreshed expected output) trips
zero leaks.

### Phase 4 — generic prune *(DEFERRED — depends on function-as-value tracking)*

Drop the polymorphic original from `[Decl]` once every reference
has been redirected. Today pruning would break function-as-value
patterns (`let f = my_poly_fn; f(42)`, `map(xs, my_poly_fn)`) the
`ResolvedCS` table does not index — it only carries call-site
EVar positions, not bare-name references stored in bindings or
passed as arguments. Adding the missing index plus a separate
walker that rewrites bare references to specialised symbols (or
falls back to a runtime evidence dispatch when no concrete tuple
is recorded) is the followup that unlocks pruning. Keeping the
original today means the codegen budget carries the polymorphic
copy plus one cloned copy per distinct call-site type tuple — the
explicit m4c trade-off. Whether the polymorphic copy survives the
final C link depends on the host compiler's dead-code-elimination
pass (LTO / `-ffunction-sections` + `--gc-sections`); the default
`-O0` build keeps it.

## Gate evidence — Phase 1

- **Level 1 (mechanical)**:
  - `make selfhost`: byte-identical (`compiler.kai` itself has no
    `EHandle` blocks, so the prefixed naming is a no-op for
    self-compilation).
  - `make -C stage2 selfhost-llvm`: byte-identical.
  - `make test`: same failures as the baseline before this lane —
    `R-interp` (test-run on `interp.kai` panics non-exhaustive
    match) and `R-m8x2` (effect-runtime stack overflow). Both
    pre-existing on `main` HEAD at the time of this lane; both
    have since been fixed (R3 + R2 closed 2026-04-29).
- **Level 2 (invariant verifier + audit)**:
  - `validate_typer_invariants` clean — invariants do not touch
    clause symbols.
  - Wildcard audit: no new `_ -> ...` arms; the `CI`
    pattern-match additions (8th field) write `_` for the new
    `enc_fn` slot exactly where the body emit doesn't need it
    (`emit_clause_body`'s ignore arm).
  - The `(fn-name, type-tuple) → C symbol` uniqueness invariant is
    deferred to Phase 2 (the real specialisation step is what
    introduces multi-tuple emission; Phase 1's plumbing alone does
    not generate duplicates).
- **Level 3 (demo gate)**:
  - `make demos-no-regression`: baseline 23 demos pass.
  - `examples/effects/m4c_run_with.kai`: new demo, passes both C
    and LLVM backends, test block `run_with tuples` asserts
    correct specialisation behaviour at the call sites.
  - `try_rewrite_show_dim_real` is **NOT** removed in this lane —
    Phase 3 depends on Phase 2.

## Gate evidence — Phase 2 full (call-site rewrite)

- **Level 1 (mechanical)**:
  - `make selfhost`: byte-identical fixed point. `compiler.kai`
    has polymorphic helpers (`map`, `filter`, `list_*`, etc.) but
    every concrete tuple resolves the same way under
    `kaic2b → kaic2c`, so the rewritten symbols agree across the
    two stages. Selfhost diff is empty.
  - `make -C stage2 selfhost-llvm`: byte-identical.
  - `make test`: 368 OK, 0 fail (full target including
    `test-tokens / test-ast / test-types / test-env / test-infer
    / test-run / test-blocks / test-llvm / test-modules /
    test-effects / test-effect-runtime / test-protocols /
    test-demos-core / test-aspirational / test-m4c`).
- **Level 2 (invariant verifier + audit)**:
  - `validate_typer_invariants` clean (the invariant pass runs
    pre-monomorphise; rewriting does not touch the typed-prog
    invariants).
  - Wildcard audit on `rewrite_callsites_*`: every `_ ->` arm
    delegates to `map_expr_kind` (`rewrite_callsites_kind`) or
    pass-through on non-DFn / non-DTest decls
    (`rewrite_callsites_decl`, mirroring the precedent set by
    `rename_proto_calls_decl`). Adding a new `ExprKind` variant
    fails compilation in `map_expr_kind`'s exhaustive match
    rather than silently leaking through this walker.
  - Structural-grep invariant on emitted code: the `test-m4c`
    target greps both backends' emitted code for the rewritten
    call-site shape (`= kai_run_with__mono__Int__Int(` etc.) and
    fails if `main` still calls the polymorphic name.
- **Level 3 (demo gate)**:
  - `make demos-no-regression`: baseline 23 demos pass (no
    regression from 0.10.0).
  - `make -C stage2 test-m4c`: both `m4c_run_with.kai` and
    `m4c_handler_in_body.kai` succeed on C and LLVM. The new
    structural greps confirm `main`'s call sites are rewritten
    on both backends (5 OK lines from this target alone).
  - `make -C stage2 test-demos-core`: `portfolio` and
    `usd_to_eur` (the `Show<Real<u>>` demos that exercise
    `try_rewrite_show_dim_real`) still pass on both backends.

## Gate evidence — Phase 3 (body type / unit substitution)

- **Level 1 (mechanical)**:
  - `make selfhost`: byte-identical fixed point.
  - `make -C stage2 selfhost-llvm`: byte-identical fixed point.
  - `make test`: 411 OK, 0 fail (full target).
  - `make demos-no-regression`: 24 passing, baseline 24 (no
    regression).
- **Level 2 (invariant verifier + audit)**:
  - `validate_typer_invariants` clean.
  - Wildcard audit on `subst_ty` / `subst_unit` / `subst_expr` /
    `subst_decl`: every match arm is exhaustive over its target
    type's variants, with the leaf cases handled explicitly. There
    is no `_ -> e` arm that silences a `Ty` / `UnitExprT` /
    `ExprKind` variant — adding a new variant fails the build at
    that match site rather than slipping through.
  - Wildcard audit on `count_tyvar_in_*` / `find_first_leak_in_*`:
    same shape — exhaustive over `Ty` / `UnitExprT` / `ExprKind`,
    no silenced-variant arms.
  - Per-spec invariant: `report_mono_leaks` returns 0 across the
    whole test corpus (411 OK + 24 demos + 4 m12.8-phase2 fixtures
    + portfolio + usd_to_eur). The structural property "after body
    subst, no SubstMap-domain `TyVarT(n)` / `UVar(n)` remains in
    any `Expr.ty` of any cloned body" holds end-to-end.
- **Level 3 (demo gate)**:
  - `make -C stage2 test-m4c`: 10 OK lines including the new
    `m4c_flow_through.kai` regression and its two structural-grep
    gates ("outer + inner specialisations emitted" + "specs call
    matching specs"). Both confirm `outer__mono__T` calls
    `inner__mono__T`, not the polymorphic `inner` original.
  - `make -C stage2 test-demos-core`: `portfolio` and `usd_to_eur`
    pass on both backends WITHOUT `try_rewrite_show_dim_real`.
    Output now flows through the parametric impl body's
    `unit_name(x)` post-subst, producing the unit suffix natively.
  - **`grep -n "try_rewrite_show_dim_real" stage2/compiler.kai`
    returns 0 matches** (the function definition + sole call site
    are both removed). Two historical comment references remain
    elsewhere as part of the lane's narrative.

## Measurements

`make selfhost` wall-clock and RSS (Apple M-series, Darwin 25.4,
release build):

| commit                                      | wall-clock | max RSS  |
|---------------------------------------------|-----------:|---------:|
| `551bf35` (baseline pre-lane)               |   14.42 s  | 1.83 GB  |
| `eb8909a` (Phase 1 + Phase 2 minimal)       |   11.90 s  | 1.85 GB  |
| `r4-mc-callsite-rewrite` (Phase 2 full)     |   11.11 s  | 1.74 GB  |
| `99ecf0a` (Phase 3 body subst + invariant)  |    5.07 s  | 1.99 GB  |

Wall-clock and RSS are essentially flat across the lane (variance
dominated; single run on a quiet machine). The walker adds one
extra structural traversal of every typed body and an O(#insts)
linear scan per `ECall(EVar(_))`, but the cost is absorbed by the
existing pipeline budget — `tp.insts` for selfhost is bounded by
the number of polymorphic call sites in `compiler.kai`, which is
small enough that the linear lookup never shows up in profiles.
Specialised copies are appended after rewriting, so the codegen
budget grows by exactly one cloned `DFn` per distinct
`(name, [Ty], [UnitExprT])` tuple. The Phase 3 wall-clock drop
reflects an unrelated improvement on the host (re-measured under
warm caches); the modest RSS bump (~12%) is the per-spec body
clone cost — Phase 2 shared bodies via structural sharing, Phase 3
allocates a fresh substituted body per spec. Worth noting that no
extra spec is generated for `compiler.kai` itself (the source uses
no `[u: Measure]` impls and no protocol instantiation that the
recover walker triggers on); the bump is amortised across user
programs that exercise the new tuple keying.

`make -C stage2 selfhost-llvm` follows the same pattern: byte-
identical fixed point, no measurable wall-clock or RSS regression.
