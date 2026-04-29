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

The blocker, pinned in `docs/m5x-followup.md` §3, is symbol
collision: `clause_fn_name(line, col, op)` minted C symbols for
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

### Phase 3 — remove `try_rewrite_show_dim_real` *(DEFERRED — depends on body type substitution)*

The callsite rewrite in `try_rewrite_show_dim_real`
(`stage2/compiler.kai:27465`) bypasses the parametric impl body
because the body's `unit_name(x)` reads `x.ty` which is the
impl-level uvar after identity monomorphisation. To remove the
rewrite, real specialisation would need to substitute type
variables in the specialised copy's body — every `Expr.ty` carrying
the impl-level uvar must be replaced with the concrete call-site
unit. That walker is more invasive than Phase 2's call-site rewrite
because compile-time intrinsics like `unit_name` and `__strip_unit`
must read the substituted types at code generation, not at typing.

A simpler closure path is: keep `try_rewrite_show_dim_real` but
move it from a special-case rewrite under
`try_rewrite_proto_call` to a general-purpose protocol-impl
specialisation step that can fan out to other dim-aware impls.
That option is also pinned for design review — it preserves the
current shape of the impl-body emission while removing the
"show is special" inversion.

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
    pre-existing on `main` HEAD; pinned in
    `docs/known-regressions.md`.
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

## Measurements

`make selfhost` wall-clock and RSS (Apple M-series, Darwin 25.4,
release build):

| commit                                      | wall-clock | max RSS  |
|---------------------------------------------|-----------:|---------:|
| `551bf35` (baseline pre-lane)               |   14.42 s  | 1.83 GB  |
| `eb8909a` (Phase 1 + Phase 2 minimal)       |   11.90 s  | 1.85 GB  |
| `r4-mc-callsite-rewrite` (Phase 2 full)     |   11.11 s  | 1.74 GB  |

Wall-clock and RSS are essentially flat across the lane (variance
dominated; single run on a quiet machine). The walker adds one
extra structural traversal of every typed body and an O(#insts)
linear scan per `ECall(EVar(_))`, but the cost is absorbed by the
existing pipeline budget — `tp.insts` for selfhost is bounded by
the number of polymorphic call sites in `compiler.kai`, which is
small enough that the linear lookup never shows up in profiles.
Specialised copies are appended after rewriting, so the codegen
budget grows by exactly one cloned `DFn` per distinct (name,
[Ty]) tuple — same as Phase 2 minimal.

`make -C stage2 selfhost-llvm` follows the same pattern: byte-
identical fixed point, no measurable wall-clock or RSS regression.
