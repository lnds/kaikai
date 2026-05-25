# Lane experience — issue #677 phase 1n1: extract compiler/infer.kai

The largest single extraction of the modularization effort. Moved the HM
type-scheme engine, substitution/unification (with m12.5 unit ops), the
bidirectional `synth` family, the per-module infer driver, the moved-in
effect-coverage / field-privacy / call-label walkers, and the typed-AST
diagnostic dumps out of `stage2/main.kai` into `stage2/compiler/infer.kai`.

## Scope as planned (PR #692 analysis + the brief)

Move **L0a + L0b + L1 + L2** (main.kai 12334–26913 — type schemes/env,
substitution/unification, `synth`, the infer driver including infer-side
dumps) plus **L7** (33266–34037 — typed-hole / effects-json / interval /
last-use dumps), skipping **L6 TCO** (32115–33265, which stays in main for
the emit_c lane). Mirror the AST-functor + fn-registration glue with an
`inf_` prefix. Budget a pub-enforcement round (estimate 18–30 pub).
Acceptance: tier1 green, byte-identical selfhost.

## Scope as shipped

Cut delivered as planned for the two source blocks, **but the boundary and
the glue strategy both moved materially** once the data was measured. Three
findings the pre-flight analysis (research-only, never compiled) could not
see:

### 1. The L2 upper boundary is 26913, not 27430

The analysis put L2's end at ~27430. Reading the callee graph showed that
26914–27428 (`dump_mono*`, `root_only_insts`, the spec-leak detectors
`count_spec_leaks` / `find_first_leak_*` / `count_tyvar_*`, plus the
`SubstMap` types) is **monomorph diagnostic surface**: every one of those
functions is called only from `monomorphise` (27431). They stay in main for
lane N2. The true infer/monomorph seam is the end of `dump_typed_loop`
(26912), right before the `# ---- --dump-mono` banner. Cutting at 27430
would have pulled monomorph's leak detectors into infer.

### 2. Four checker families MOVE into infer rather than mirror

The brief framed all main-resident glue as "mirror with `inf_`". Measuring
the real coupling (comment-stripped, stopping at block boundaries) gave 123
external symbols / 2305 LOC. But a fixpoint analysis showed **46 of those
(987 LOC) have their *sole* external caller inside the infer block**:

- `check_default_coverage*` + `walk_coverage_*` + `check_op_call_coverage`
  + `report_uncovered_op` (effect-default coverage; sole caller
  `typecheck_module`),
- `check_field_privacy*` + `walk_field_priv_*` (priv-field walker; sole
  caller `typecheck_module`),
- `collect_call_labels*` (main-row effect collector; sole caller
  `infer_decl`),
- `inferred_main_row`, `edition_at_least`/`edition_rank` (sole callers in
  the block).

These were physically in main's 8000–9200 / 43500 zones — *outside* the
analysis's 12334–34037 window — but they are typer/effect-side and have no
life outside the typer. Moving them (deleting from main, adding to infer)
rather than mirroring dropped the mirror count from 119 to 77 and removed
987 LOC of would-be duplication. **Lesson: "mirror everything main-resident"
over-duplicates; classify by caller-set first — sole-block-caller ⇒ move,
shared ⇒ mirror.**

The decisive measurement subtlety: a naïve transitive closure of
`check_default_coverage` reported 1467 external fns (it reaches the effect
builtins, which reach half the compiler). Stripping comments from the
call-extraction regex AND stopping the walk at the block boundary collapsed
that to 45 — the 1422 difference was names matched inside comment banners
and recursion through functions already inside the block. Any future lane
measuring coupling must comment-strip and boundary-stop, or it will wildly
over-count.

### 3. Fifteen typer types lived *above* the cut and had to move up-out

`AliasCand`, `AliasResolution`, `AliasResolveOne/State/Pos`, `AliasResult`,
`HandleFrame`, `Use`, `IPart`, `IScan`, `OpToEff`, `FieldDeclWithHome`,
`AEntry`, `AStatus`, `LU` (+ the helper `label_to_rl_zero`) were defined in
main outside the 12334–26913 window but referenced by the infer body. In
the flat kaic1 bundle this is invisible (one translation unit); under
kaic2's **modular** compile it is an upward reference — infer importing a
private-to-main symbol, which the compiler rejects. Because no other module
uses them (only main + infer) and main imports infer downward in the bundle
order, the fix is to **move them into infer.kai as `pub`** and let main
resolve them downward. No `compiler.ast` sink needed.

### Public surface: 79, not 18–30

49 pub fns + 30 pub types. The analysis's 18–30 counted entry points; the
real driver↔typer coupling is wider:

- The driver calls ~47 infer fns directly: the 3 entry points
  (`infer_program`, `infer_program_with_protos_cached`, `build_ty_env`),
  the 9 diagnostic dumps, **15 name-collision validators** the driver runs
  pre-typing (`validate_*_collisions_decls`, `validate_module_*`,
  `validate_unit_refs_decls`), and unit/tparam/json helpers physically
  resident here (`unit_canon`, `normalize_union`, `dim_collapse`,
  `tparam_id_split`, `json_str`, `real_c_lit`, the dotted-name splitters)
  that the driver also calls.
- 30 pub types: 6 result types in entry signatures (`TypedProgram`,
  `ResolvedCS`, `TyEntry`, `TyScheme`, `AliasBinding`, `TPBind`), the 15
  relocated typer types (finding #3), and ~9 more transitively exposed by a
  pub signature (`BuiltEnv`, `TyEnv`, `HoleKind`, `ResolvedHole`,
  `ResolvedRowHole`, `HeadOwnerEntry`, `OpEffArity`, `UnionInfo`,
  `TParamSplit`).

The HM engine itself (`apply_ty`, `unify`, `instantiate`, `synth`,
`InferState`, `Subst`) stays **private** — zero callers in main outside the
block, exactly as the analysis predicted. The surface is large not because
the engine leaks but because the *driver orchestrates a wide validator +
dump + helper API*. Lanes N2+ should expect a similarly wide pub surface
when they extract a layer the driver calls into directly.

## Design decisions / alternatives considered

- **Mirror vs sink for the AST-functor family (asu consult).** The
  AST-functor helpers (`map_expr_kind`, `map_*_exprs`) are now mirrored in
  BOTH desugar (`dsg_`) and infer (`inf_`) — three copies counting main's
  original. The architect's cardinality rule (consumers ≥2 ⇒ sink to
  `compiler.ast`; =1 ⇒ mirror) says these should sink. Deferred to a
  dedicated follow-up lane: sinking touches ast.kai + desugar.kai and would
  make this already-huge diff cross-module and un-bisectable, violating the
  analysis's own "do not bundle a multi-module diff with a new public
  surface" rule. The mirror is correct-but-redundant; the sink is the right
  *next* lane. Documented as a follow-up below.
- **Move-vs-mirror threshold.** Chose: sole external caller is the block ⇒
  move (delete from main); any shared caller ⇒ mirror. This is the natural
  generalisation of the phase-1m `dsg_` rule, now made quantitative.
- **`inf_indent_of` — the one mirror the closure missed.** `inf_emit_line`'s
  body calls `indent_of`, a 2-line helper that the transitive-closure script
  dropped (it lives inside a string interpolation `#{indent_of(d)}` and the
  regex under-counted it). The flat-bundle selfhost passed regardless
  (global visibility), so the gap only surfaced when compiling the test file
  module-in-isolation. Added `inf_indent_of` by hand. **Lesson: the
  byte-identical selfhost does NOT catch a missing mirror if the symbol is
  globally visible in the bundle; only a module-isolated compile (the test
  file) does. Run the per-module test as part of the acceptance gate, not
  just selfhost.**

## Structural surprises

- The flat kaic1 bundle masks upward references and missing mirrors that the
  modular kaic2 compile rejects. Selfhost (`kaic2 main.kai`) compiles the
  *package* modularly, so it caught the upward type references (finding #3)
  — but NOT the missing `inf_indent_of`, because that function had a caller
  in the block which kaic2 type-checks against the still-globally-present
  `indent_of`. The test file (`kaic2 test`) is the only path that compiles
  infer truly standalone. Future vertical lanes should compile their new
  module's test file early as a standalone-visibility probe.
- `parse_program` returns `PParser { decls: Option[[Decl]] }`, not `[Decl]`
  — the test helper must `match r.decls { Some(ds) -> ds; None -> [] }`.
- `Label` is a record type; constructing one needs the type name
  (`Label { eff: ..., ty_args: ... }`), a bare `{ eff: ... }` fails to infer.

## Fixtures / coverage

`stage2/tests/test_infer.kai` (143 LOC): 15 unit tests + 2 property checks.

- Unit tests: pure helpers (`unit_canon` idempotence/folding,
  `normalize_union`, `labels_empty`, `dotted_prefix/suffix`,
  `string_contains_dot`, `list_has_int`, `json_str`, `ty_env_empty`) plus
  **3 end-to-end HM-engine tests** that parse + `infer_program` a source
  string and assert error/hole counts — these exercise the private engine
  (`synth`/`unify`/`apply_ty`/`ty_to_string`) transitively.
- Property checks (`--prop-check`, 100 iter each): `unit_canon` idempotent
  on atomic-unit products; `normalize_union` order-insensitive on a
  two-type union.
- Result: 15/15 tests + 2/2 checks pass; byte-identical selfhost; full
  modular compile clean.

Not wired into a tier (consistent with every other `compiler/test_*.kai` —
the per-module test runner wiring is deferred project-wide to issue #452 /
#677 Phase 2). The typer is exhaustively exercised on every selfhost
regardless; the unit file is for fast localised regression.

## Cost vs estimate

Estimate: largest lane, 4–8 commits, ~10 500 LOC moved. Actual: ~16 400 LOC
left main (the moved blocks + 46 moved fns + 15 moved types), 18 060 LOC in
infer.kai (blocks + 78 mirrors + moves + 16 relocated types). The bulk of
the cost was not the mechanical move but the **three boundary corrections**,
each requiring a measurement pass: the L2/monomorph seam, the move-vs-mirror
classification, and the upward-type discovery via the pub-audit loop. The
pub-audit converged in 3 kaic2 iterations once the 15 types were relocated.

## Follow-ups for next lanes

- **AST-functor sink lane (new).** Promote `map_expr_kind` + `map_*_exprs`
  (the AST-functor family) to `compiler.ast` as `pub`, and drop both the
  `dsg_` (desugar) and `inf_` (infer) mirror sets. Cardinality is now ≥2
  consumers — the sink is justified. Small, mechanical, byte-identical;
  pays the duplication debt this lane and phase-1m both incurred.
- **Lane N2 (monomorph).** Cut starts at the `# ---- --dump-mono` banner
  (~26914 in the post-1n1 main), NOT before. The leak detectors
  (`count_spec_leaks`, `find_first_leak_*`, `count_tyvar_*`) and `SubstMap`
  family belong to monomorph. monomorph imports `compiler.infer` for
  `TypedProgram` / `ResolvedCS` / `TyScheme` (all pub now) and
  `compiler.ast` for the `Proto*Reg` types (sunk in #693).
- **Lanes N3 (unbox) / N4 (perceus).** `scan_uses_*` (the use-scanner) is
  mirrored here as `inf_scan_uses_*` AND used by unbox/perceus
  (`unbox_arm_with_scr_aware`, `name_captured_in_block`); when those lanes
  extract they will need the same scanner — another sink candidate
  (`compiler.ast` or a `compiler.scan` module), consumers ≥3.
- **General rule for measuring coupling.** Comment-strip the
  call-extraction regex and stop the transitive walk at the extracted
  block's boundary, or coupling counts inflate by 10×+ (this lane:
  1467 → 45). And run the new module's test file as a standalone-visibility
  probe — selfhost's flat bundle hides missing mirrors.
