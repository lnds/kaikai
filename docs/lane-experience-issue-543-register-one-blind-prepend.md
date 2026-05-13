# Lane retro — issue #543: `register_one` blind-prepend leaks duplicate `effect` / `const` / `axiom`

Closes the three remaining decl-kind holes in `register_one`'s blind-prepend pattern.
Companion to PR #542 (DFn, refs #538) and PR #518 (DType). After this lane the four
decl kinds that flow through `register_one` (DType, DFn, DEffect, DConst, DAxiom)
all reject same-module same-name redeclarations.

## Scope as planned

- Three negative fixtures (`effect`, `const`, `axiom`) + three validators wired into
  `compile_source` alongside the existing DType / DFn validators.
- Selfhost byte-identical (prediction held).
- Refactor parametric `validate_decl_name_collisions(kind, ...)` listed as optional
  follow-up in the briefing — left for a separate lane.

## Scope as shipped

Exactly the planned scope. Option A (three paralel validators) over Option B
(parametric refactor):

- Three new types — `EffectNameClaim` / `ConstNameClaim` / `AxiomNameClaim`.
- Three new validators — `validate_effect_name_collisions_decls`,
  `validate_const_name_collisions_decls`, `validate_axiom_name_collisions_decls`.
- Three new claim-walk helpers and three new diagnostic emitters, each modeled
  after `validate_fn_name_collisions_decls` byte-for-byte modulo the kind keyword.
- Three new wires in `compile_source` plus three new branches in the exit-code
  aggregator, mirroring the existing pattern.
- Three new negative fixtures + `.err.expected` goldens in
  `examples/negative/modules/`.

`tools/test-negative.sh` count: 90 → 93 PASS (the runtime tier was already at
90 because protocol fixtures landed in #547 between brief drafting and lane
execution; the +3 from this lane is the expected delta).

## Design decisions

### Option A vs Option B

Brief recommended Option A on blast-radius grounds. Lane agreed. The four-kind
validators (type, fn, effect, const, axiom) now share ~70% of their structure,
and a `validate_decl_name_collisions(kind, name_of, line_of, col_of, decls, file)`
refactor would consolidate them. But:

- The refactor needs a way to pattern-match on the decl variant per kind. Stage 2
  doesn't have closures-over-pattern-match; the natural shape is an enum
  discriminator with a giant inner match, which is barely shorter than the parallel
  validators we have today.
- Each kind needs slightly different diagnostic text ("effect 'X' is already declared"
  vs "const 'X' is already declared" vs "axiom 'X' is already declared") and a
  slightly different help phrase ("'effect X { ... }'" vs "'const X = ...'" vs
  "'axiom X(...)'"). A parametric helper that takes a closure for diagnostic
  rendering is feasible but adds an indirection that obscures the diagnostic
  shape from a casual reader.
- The risk surface of a refactor that touches all four collision validators is
  larger than the risk surface of three paralel additions. Selfhost being
  byte-identical was the principal acceptance signal; we preserve it trivially
  with Option A.

Follow-up filed implicitly in this retro: if a sixth decl kind ever needs the
same treatment, revisit the parametric refactor. With four kinds the duplication
is tolerable.

### DConst needed a pre-lowering walk

The natural first attempt fed `root_only_decls` to all three new validators.
That worked for DEffect and DAxiom but silently passed every duplicate-const
case. Cause: `lower_consts` (line ~57060 in `compile_source`) rewrites every
DConst to a `DAttribPure(DFn arity-0)` per issue #269, *before* `root_only_decls`
is built (the rewrite has to run before `qualtype_decls`, which is exhaustive
on Decl and would panic on DConst). So when the const-walker matched on
`DConst(...)`, it found zero matches in `root_only_decls`.

Fix: split `expanded_decls_raw` (the pre-lowering decl list) into root-only
form via `pae_drop(expanded_decls_raw, n_imports_raw)`, mirroring the same
split that produces `root_only_decls` from `qualified_decls`. The const walker
gets this `root_only_decls_raw` instead of the post-rqc `root_only_decls`.

The DAxiom case did NOT need this treatment — `lower_axioms` runs later in
the pipeline (line ~57153), after the validators have already executed. DEffect
is preserved through the full pipeline (it gets `register_one`'d on the post-rqc
list).

This was the only real structural surprise of the lane. Brief had not anticipated
the lowering-order asymmetry between consts (early) and axioms (late). The lane
retro of #542 should have flagged this since fn-name validation didn't have to
deal with it.

### Diagnostic anchoring

Each diagnostic anchors at the *second* declaration site, with the body citing
`file:line:col` of the earlier one. Help line nudges toward "remove or rename
the second '<kind> X ...' declaration". This mirrors the DFn and DType
diagnostic shape exactly, including the `concat_all([...])` rendering style.

### What this lane does NOT cover

Three things deliberately deferred per the brief:

- **Variants of `DType` cross-decl** — two `type A = X | Y` and `type B = X | Z`
  in the same module. The DType-level collision check at #518 catches some of
  this transitively; the variant-level walk via `register_variants_env` still
  has a blind `env_add` per variant. Worth a separate audit — file as
  follow-up issue if the next typer lane finds a repro.
- **`DProtocol` / `DImpl` / `DDerive` / `DUnit`** — route through other
  pipelines and may have their own collision validation. Not audited this
  lane.
- **Parametric refactor** — addressed above; left intentionally open.

## Selfhost outcome

`make tier0` (which runs the selfhost golden) is byte-identical post-lane.
This was the expected outcome: stdlib and the compiler itself have no
duplicate effect / const / axiom names (verified by manual grep prior to
the implementation). The new validator branches are dead code on the
production input set; they only fire for ill-formed user code.

## Fixtures

- `examples/negative/modules/duplicate_effect_decl.kai` — two `effect Logger { ... }`.
  Initial draft used `fn` inside the effect body; corrected to bare-name
  operations (`log(msg: String) : Unit`) per the parser convention used in
  `stdlib/trace.kai`. Fixture authors take note: effect operations are not
  `fn`-prefixed.
- `examples/negative/modules/duplicate_const_decl.kai` — two `pub const MAX : Int`.
- `examples/negative/modules/duplicate_axiom_decl.kai` — two
  `axiom unsafe_cast[A, B](x: A) : B`.

Each `.err.expected` is a single line with the substring matcher used by
`tools/test-negative.sh` (first-line `grep -F`).

## Cost vs estimate

Brief estimate: 1 day. Actual: a few hours including the
DConst lowering-order detour. The mechanical part (validators + fixtures
+ wiring) was the predicted few-hour shape from #542. The DConst surprise
cost ~15 minutes of debugging plus the additional `root_only_decls_raw`
plumbing — small enough that the estimate held.

## Follow-ups left for next lanes

- **Variant-level collision walk** in `register_variants_env`
  (`stage2/compiler.kai:10722`). The DType validator at #518 catches the
  *parent* type duplicating, but two distinct types sharing a variant name
  in the same module is still silently shadowing at the variant level. File
  an issue if the next pattern / typer lane surfaces this.
- **`DProtocol` / `DImpl` / `DDerive` / `DUnit` audit** — separately
  scoped lane. Hold until a real bug surfaces.
- **Parametric refactor** — see *Option A vs Option B* above. Defer until a
  fifth or sixth decl kind joins this set.
