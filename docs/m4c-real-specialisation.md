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

## What is deferred

### Phase 2 — real specialisation *(DEFERRED — design review)*

`monomorphise(tp: TypedProgram) : [Decl]` still returns `tp.decls`
unchanged. Real specialisation needs:

- Collect distinct `(name, [Ty])` tuples per polymorphic DFn from
  `tp.insts` (`[ResolvedCS]`).
- For each distinct tuple, emit a specialised copy with
  `mangle_name(name, tys)` and `tparams = []`.
- Walk the full AST and rewrite every call site at the recorded
  `(line, col)` to point at the specialised name. New AST walker,
  shaped like `rename_proto_calls_*` with a `(line, col, name) →
  mangled_name` lookup parameter (~300 LOC of mostly-mechanical
  recursion).
- Decide whether to prune the original polymorphic decl. Pruning
  breaks function-as-value patterns (passing the polymorphic name
  as an argument, partial application beyond what `ResolvedCS`
  records); keeping it means the codegen budget grows by exactly
  the number of specialised copies, which is the explicit m4c
  trade-off.

The blocker here is no longer technical (Phase 1 unblocks it) but
design: the call-site rewrite ordering against
`rename_proto_calls`, the protocol-impl resolver, and `derive`
expansion needs a careful pass to avoid scope-aware rewrites
clobbering each other. Pinned for human review per the lane's
design-decision policy.

### Phase 3 — remove `try_rewrite_show_dim_real` *(DEFERRED — depends on Phase 2 + body type substitution)*

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

## Measurements

`make selfhost` wall-clock and RSS (Apple M-series, Darwin 25.4,
release build):

| commit                         | wall-clock | max RSS  |
|--------------------------------|-----------:|---------:|
| `551bf35` (baseline pre-lane)  |   14.42 s  | 1.83 GB  |
| `376d3f3` (Phase 1 landed)     |     ~ s    | ~ GB     |

The ClauseInfo arity bump (7→8 fields) and the `cls: [ClauseInfo]`
parameter threading add a fixed-shape overhead per emitted record.
Phase 1 is expected to be a wash on RSS (no extra heap allocation
in steady state) and a small wall-clock cost from the threading
boilerplate. To be filled in once the bench runs after the
final commit lands.
