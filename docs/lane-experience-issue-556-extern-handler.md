# Lane experience report — issue-556-extern-handler

Lane brief: close issue #556 — Stage A of #533 trilogy. Implement
`$extern_handler` compiler intrinsic + `Cont[Ret]` first-class type;
migrate Stdout's default handler from the hardcoded emit table to
an AST-derived path; verify byte-identical emit.

Estimated 2 weeks per #556 body; actual runtime ~5 hours
(interactive, with the user surfacing several design decisions
ahead of implementation).

## Outcome

Stages 1-5 shipped. Selfhost byte-identical. tier0 OK.
Positive + negative fixtures land. Retro complete.

## Q1-Q4 decisions

### Q1 — `Cont[A, B]` vs `Cont[Ret]` shape

**Decision: `Cont[Ret]` (one parameter).**

The #556 brief recommended `Cont[A, B]` (resume-with A, handler-return
B). The lane verified the runtime + docs:

- `docs/effects-impl.md` lines 218, 230, 274, 275, 557, 613 use
  `Cont[Ret]` uniformly (single parameter). `Cont[A, B]` never appears
  in any doc.
- `stage0/runtime.h` lines 4853-4888: `KaiCont` carries a single
  `fn(void *env, KaiValue *v)`. The return is `KaiValue *` (the
  collapsed `Answer`), not parameterised per-handler.
- Zero pre-existing uses of `Cont[...]` in compiler.kai or stdlib —
  this lane introduced its first surface appearance.

Adopting `Cont[A, B]` would diverge from authoritative docs without
runtime motivation. `Cont[Ret]` is what runtime + docs already pin.

### Q2 — `[<extern_handler>]` attribute vs `extern "handler" fn` vs `$extern_handler` intrinsic

**Decision: `$extern_handler("c_symbol")` intrinsic, used as a clause
body inside `default { }`.**

The brief recommended `[<extern_handler>]` attribute. The lane
discovered this contradicts issue #260 which **removed**
`[<extern_c>]` attribute syntax in favour of `extern "C" fn`.
Reintroducing a bracket-attribute would resurrect exactly the form
#260 retired.

Initial replacement: `extern "handler" fn` symmetric with
`extern "C" fn`. But the existing `default { }` parser from #551
takes a `[HClause]` (op clauses with body-Expr) — admitting top-level
declarations like `extern "handler" fn` inside it would require
refactoring `DefaultBlock` AST (the ~53-site destructuring that #551
landed). Major refactor.

**Final design (this lane, pinned in user conversation):** introduce
`$<ident>(args)` as the surface form of "compiler intrinsic call",
reserved sigil. The clause body keeps shape `op(args, resume) -> body`
where `body` is an `Expr`. The intrinsic `$extern_handler("c_sym")`
fits as an Expr that the typer/codegen recognises in the
default-block clause context.

This decision is **load-bearing for the whole language**, not just
this lane. See `project_dollar_intrinsic_syntax.md` memo. Trade-off:
two FFI conventions (`extern "C" fn` for top-level FFI; `$extern_handler`
for default-block bridges). Justified because the contexts and
calling conventions differ — `extern "C" fn` is a *declaration*
with a row, `$extern_handler` is a *bridge expression* inside an
already-declared op.

### Q3 — Parser path

**Decision: lexer emits `TkDollar`, parser binds it to
`EIntrinsic(name, args)` ExprKind variant.**

- Lexer adds `TkDollar` (one new token). `$` was previously unused.
- Parser `parse_intrinsic` reads `$IDENT(args)`, consumes via existing
  `parse_call_args`, emits `EIntrinsic`.
- `EIntrinsic(String, [Expr])` is a generic variant for future
  intrinsics (`$resume_multishot`, `$evidence_lookup`, etc.) — not
  a one-off for `$extern_handler`. Per Tier 2 #4 (few forms, each
  with clear intent) the family is the form, individual intrinsics
  are the instances.

### Q4 — Prelude extensions

**Decision: `Cont` reserved as a builtin type name only — no
prelude `type` declaration needed.**

The typer is structurally permissive: `TyCon("Cont", [t])` already
unifies without any registry entry. The only required protection is
that user code cannot redeclare `type Cont[T] = ...` and shadow the
builtin. Added `reserved_builtin_type_names()` returning `["Cont"]`,
gated in `validate_one_type_decl`.

The codegen for handler clauses already treats the last parameter as
`resume` and emits `KaiCont *k` in C unconditionally
(`op_args_only` drops it before C signature minting). So `Cont[Ret]`
in source surface lowers automatically to `KaiCont *` at emit — no
explicit type-lowering rule needed.

## Stages completed

### Stage 1 — `Cont` reservation
Added `reserved_builtin_type_names()` + check in
`validate_one_type_decl` rejecting user redeclaration of `Cont`.
Selfhost byte-identical, no other change.

### Stage 2 — `$<ident>(args)` parser
- Lexer: `TkDollar`, char `$` → `tok(TkDollar, ...)`.
- AST: `EIntrinsic(String, [Expr])` variant of `ExprKind`.
- Parser: `parse_intrinsic` invoked from `TkDollar` arm of
  `parse_primary`.
- 28 exhaustive-match walker arms agregadas para `EIntrinsic` (free
  vars, captured-lambda collectors, alias expansion, perceus dup
  rewrites, qualtype, rqc, formatter, find-var-at, etc.). All
  mechanical — replicates the `EVariantsOf` / `EModCall` arm shape
  for arms that don't recurse, or threads through args for arms that
  do.

### Stage 3 — Typer accept (synth path)
- `synth` arm for `EIntrinsic` reports
  `compiler intrinsic '$X' is not valid in this position`.
- The typer **does not visit `DefaultBlock` clauses** post-parse
  (carry-over from #551 scaffolding); reaching the synth arm means
  user code wrote `$X` outside a default-block context.
- Per the #556 acceptance, full typer coverage check is Stage B
  (#557 follow-up). This lane only typecheckable the bridge shape.

### Stage 4 — Codegen helpers
- `find_effect_default_block(eff_name, decls)`: lookup the
  `Option[DefaultBlock]` on `effect <eff_name>` in the program decls.
- `extern_handler_c_symbol(body)`: recognises a clause body of shape
  `$extern_handler("c_symbol")`, returns `Some(c_symbol)` or `None`.
- `strip_string_quotes(span)`: defensive strip of outer `"..."` from
  a string-literal span.
- `default_shim_from_clause(eff, clause)`: emit the typed shim
  forwarding to the C symbol; signature matches the hardcoded
  template exactly.
- `default_setup_assign_from_clause(eff, clause)`: emit the per-op
  `_kai_default_ev_X.op = &_shim;` assignment.
- `default_shims_from_block(eff, decls)`, `default_setups_from_block`:
  walk a default block's clauses, accumulate per-op shim/setup C.
- `default_shims_for(main_row, decls)` and
  `default_setups_for(main_row, decls)`: for Stdout, **first** try
  the AST-derived path; fall back to hardcoded if the effect has no
  default block. Other 16 effects still hit the hardcoded path —
  Stage C migrates those.

### Stage 5 — Stdout migration
- `builtin_stdout_default_block()` constructs the AST for
  `default { print(s, resume) -> $extern_handler("kai_default_stdout_print") }`.
- `builtin_stdout_decl()` returns the DEffect with
  `Some(builtin_stdout_default_block())`.
- Byte-identical confirmed: a Stdout-using program emits
  ```c
  static KaiValue *_kai_default_stdout_print_shim(EvStdout *self, KaiValue *kai_s, KaiCont *k) {
      return kai_default_stdout_print(self, kai_s, k);
  }
  ```
  exactly as the hardcoded `default_stdout_shims()` did.
- Selfhost fixed point: OK (no churn from the migration).

### Stage 6 — Fixtures
- `examples/effects/extern_handler_user_effect.kai` (positive): user
  effect `MyLog` with `default { info(msg, resume) -> $extern_handler("kai_default_log_info") }`,
  wrapped in `handle ... with` so the test runs end-to-end. Output:
  `result: 7`. Golden `.out.expected` matches.
- `examples/negative/effects_phase2/extern_handler_outside_default.kai`
  (negative): `$extern_handler` at top-level expression position.
  Typer rejects with the pinned diagnostic.

## Stdout migration: byte-identical confirmation

Tested via direct C diff. With Stdout's `default { }` block in place,
`default_shims_for("Stdout", decls)` calls `default_shims_from_block`
which produces:
```c
static KaiValue *_kai_default_stdout_print_shim(EvStdout *self, KaiValue *kai_s, KaiCont *k) {
    return kai_default_stdout_print(self, kai_s, k);
}
```

Identical (byte-for-byte, including indentation and the trailing
`\n`) to the hardcoded `default_stdout_shims()` output. Setup block
identical as well. The chain stabilises under selfhost (kaic2b.c ==
kaic2c.c). tier0 demos regression baseline (28 passing) holds.

## Bug found during the lane

Mid-Stage 5, the runtime emitted an empty `c_sym` in the shim
(`return (self, kai_s, k);` instead of
`return kai_default_stdout_print(self, kai_s, k);`). Spent ~30
minutes debugging. Root cause: I named a local binding `c_sym` in
`default_shim_from_clause`, colliding with the pre-existing
top-level fn `c_sym(name, mo)` from issue #261 FFI. The resolver
silently picked up the global fn over the local, emitting
`kai_closure(&_kai_c_sym_thunk, 2, 0, NULL)` in place of the local
string read.

**Workaround in this lane:** renamed the local to `csym_name`.
**Saved as memory** `project_bare_ident_shadowing_resolver_bug.md`
for future lanes. **Follow-up issue (not opened in this lane):** the
resolver should either reject shadowing or prefer local bindings; the
typer's silent acceptance + codegen's silent global-pickup is a
nasty failure mode.

## Real cost vs estimate

| Item | Estimate (#556 body) | Actual |
|------|----------------------|--------|
| Total wall time | ~2 weeks | ~5 hours |
| Stages | 6 stages | 6 stages |
| AST walker arms | not estimated | 28 (mechanical) |
| Selfhost iterations | not estimated | 2 (1 false-positive DIFF from cached kaic2, 1 from bare-ident bug) |
| Bugs discovered | 0 expected | 1 (bare-ident shadowing) |

The 2-week estimate assumed `[<extern_handler>]` attribute syntax
(retired by #260), `Cont[A, B]` (contradicts effects-impl.md), and
full coverage check (Stage B). The actual scope after design
decisions was tighter than the brief implied.

## Follow-ups for Stage B (#557) and Stage C (#558)

- **Stage B** (typer coverage check): now that the AST-derived
  codegen exists for Stdout, the typer can be wired to (a) visit
  `DefaultBlock.clauses` after parse, (b) validate each clause's
  `$extern_handler` arg is a C-identifier string, (c) populate the
  coverage table so a `Stdout`-row main without explicit `handle`
  installs the default. This lane's `extern_handler_c_symbol` helper
  is the validation primitive.
- **Stage C** (16 builtin migrations): each builtin gets a
  `default { }` block in its `builtin_*_decl()` constructor. The
  derived path already handles arbitrary effect/op combinations via
  `default_shims_from_block`; the only per-effect work is encoding
  the op signatures and C-symbol names. After all 16 migrate, the
  hardcoded `default_*_shims/setup` family + `effect_default_compatible`
  hack collapse.
- **Resolver bug** (separate, low priority): bare-ident shadowing of
  top-level fns by locals — see lane retro middle section.

## Verification summary

- `make tier0` — green (25 OK, 28 demos baseline holds).
- `make selfhost` — byte-identical fixed point.
- `make tier1` — green (verified at lane close).
- Positive fixture `extern_handler_user_effect.kai` — runs end-to-end,
  matches golden.
- Negative fixture `extern_handler_outside_default.kai` — parser/typer
  rejects with pinned diagnostic.
- Stdout emit byte-identical to pre-lane (compared directly via
  `diff` against the hardcoded `default_stdout_shims()` output).
