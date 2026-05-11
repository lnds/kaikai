# Lane experience — issue #487 (collectable structured diagnostics)

Best-effort retrospective by the implementing agent.

## Scope as planned vs as shipped

**Planned (per issue #487 + lane brief):**

- New types `Diagnostic`, `Severity`, `RelatedInfo` near `diag_*` helpers.
- A `DiagSink` carrying a 178-site collection of diagnostics, addressed
  as a global / threaded sink so the 178 typer call sites stay
  unchanged.
- A new public API `compile_to_module_with_diags : (TypedModule, [Diagnostic])`
  consumed by LSP v1 (#447).
- 6 fixtures (one per template T1-T5, plus a multi-error fixture)
  under `examples/library_mode/`.
- Tier 0 + Tier 1 green; selfhost byte-identical.
- Retro + PR; no merge.

**Shipped (narrowed to Approach A, scope = m11 T1-T5 only):**

- Types `Severity`, `RelatedInfo`, `Diagnostic` and the helpers
  `mk_diag`, `ri_note`, `ri_help`, `push_diag`, `finalize_diags` ship
  near the existing `diag_error/note/help` helpers (stage2/compiler.kai
  ~line 1040).
- `[Diagnostic]` accumulators land on `InferState.diags` (typer) and
  `Env.diags` (chk_*). The two threaded-state types are the *natural*
  carriers because all five m11 emit sites are already mutating them.
  The 178-site refactor was not feasible (see "Approach A vs B vs C").
- The five m11 emit sites (T1, T2, T3, T4, T5) now build a `Diagnostic`
  in parallel with their existing `diag_error/help/note` stderr block.
  Stderr output is byte-identical to baseline; the structured value is
  additional. Other ~140 `diag_*` call sites remain stderr-only.
- `TypedDecl.diags` plumbs each decl's collected list out of
  `infer_decl`; `infer_all_loop` aggregates them into `TypedProgram.diags`
  via the existing `prepend_reverse_*` pattern used for `holes` /
  `row_holes` / `insts`.
- New public API: `type ModuleWithDiags = { tm: TypedModule, diagnostics: [Diagnostic] }`
  and `fn compile_to_module_with_diags(file, decls) : ModuleWithDiags / Console`.
  It runs both the lexical-scope pre-pass (`chk_decls`, for T3) and the
  inferer, concatenating `Env.diags` + `TypedProgram.diags` in source
  order.
- New CLI flag `--diags-json` and `MDiagsJson` mode driving
  `dump_diags_json`, which serialises `compile_to_module_with_diags`'s
  output as JSON. Schema:
  ```json
  {"file": String,
   "diagnostics": [{"severity": String, "file": String, "line": Int,
                    "col": Int, "message": String,
                    "related": [{"file": String, "line": Int, "col": Int,
                                 "message": String}]}, ...]}
  ```
- 6 fixtures under `examples/library_mode/diags_*.kai` with
  `.diags.expected` goldens; new `test-diagnostics-collected` make
  target, wired into `tier1`.
- Tier 0 green; Tier 1 green locally; selfhost byte-identical
  preserved (stderr stream unchanged, additive collection only).
- Retro + PR per the lane-handoff exception. PR opened, **not merged**.

**Not in scope (deferred to follow-up lanes):**

- The other ~140 `diag_*` call sites (m4c invariant leaks, refinement
  violations, ambiguous bare-name warnings, unused bindings, row-label
  arity, etc.). Each migrates as its own narrow lane; the data types
  and `push_diag` helper are already in place.
- LSP server itself (#447). This lane only delivers the prerequisite.
- JSON-RPC framing.
- Severity-tuning, error codes, warning-as-error config.
- An LLM-only `--diags-machine-json` flag that drops the human prose
  in favor of a tighter schema. The current schema is the human
  diagnostic prose, lifted verbatim.

## Why narrow (Approach A vs B vs C)

The original brief assumed an `errs_ref : Ref[Int]` plumbing pattern
inside the typer that we could extend to `diags_ref : Ref[[Diagnostic]]`
without touching call sites. **That plumbing does not exist.** The
typer threads error counts as `Int` return values (e.g.
`TypedDecl.errs`, `TypedProgram.errs`, `DPRDecl.errs`); there is no
captured mutable cell.

I considered three approaches:

- **A — sink redirection via a global / threaded reference.** Requires
  either a top-level `var` (kaikai stage 2 has no module-level mutable
  state) or a `Ref[T]` carried through every `Unit / Console` call
  site (178 signature changes, plus their callers).
- **B — return-value refactor.** Every `diag_*` call becomes a return
  value collected by the caller. Touching every site directly.
- **C — FFI escape hatch.** Add `__diag_sink_push` / `__diag_sink_drain`
  intrinsics backed by a C-level global, wire the default `Mutable`
  handler. Adds a new audited runtime escape; the existing escapes
  list (`docs/effects-stdlib.md`) does not yet include "compiler
  diagnostics".

After STOP-and-reporting to the human integrator, the lane was
scoped to **the five m11 templates only**. Concretely:

- T1, T2, T4, T5 emit from leaf positions that already thread
  `InferState`. Adding `InferState.diags` extends the existing
  threaded record by one field and updates 16 constructor sites.
- T3 emits from `chk_expr`, which threads `Env`. Adding `Env.diags`
  extends a 4-field record by one field and updates ~5 constructor
  sites (env_new, env_push, env_pop, env_add, env_mark_err).

Total touch: ~25 constructor sites, all mechanical, plus 5 emit-site
rewrites. The remaining ~140 legacy `diag_*` sites keep stderr-only
behavior and migrate as future incremental lanes when their consumer
(LSP / IDE / linter) actually needs them.

This is sufficient to **unblock LSP v1 (#447)**: the LSP consumer
needs T1-T5 for `publishDiagnostics` — typer errors, exhaustiveness,
unbound names, arity, missing effects. The legacy stderr-only sites
(e.g. m4c Phase 3 invariant leaks, RC-trace internal debug) are
not what an IDE-side user clicks on.

## Design decisions

### Stderr backward-compat: both, not mode flag

Every emit site keeps its existing `diag_error/note/help` calls
**and** pushes a `Diagnostic`. The stderr stream is unchanged
byte-for-byte; the in-memory collection is additive. CLI users see
the same colored output; library / LSP consumers read
`TypedProgram.diags`. Selfhost byte-identical holds because the
collection path is never exercised during `kai build` (only
`--diags-json` reads the collected list).

The alternative — a mode flag (`Stderr` vs `Collect`) — would have
silenced stderr in library mode, which the existing
`dump_library_mode` consumers do not expect. The cost of "build the
Diagnostic value even when it will be discarded by the CLI path" is
~6 short allocations per emit site, only on the error path. The
shipped path values byte-identical CLI behaviour over a marginal
allocation save.

### Field name `diags` vs `diagnostics`

Used the shorter `diags` everywhere internally (`InferState.diags`,
`Env.diags`, `TypedDecl.diags`, `TypedProgram.diags`,
`prepend_reverse_diags`) for tight match-arm width. The public
`ModuleWithDiags.diagnostics` uses the long form because the LSP /
JSON wire schema expects `diagnostics`.

### Variant tag `DR` vs `RI` for `RelatedInfo`

Stage 2 has a single global variant-tag namespace; `RI` was already
in use as `type RecInfo = RI(String, [String], [FieldDecl])` (line
26701). The lane uses `DR` (diagnostic-related) for the new tag.
The wire format is field-positional, so the tag name has no impact
on JSON consumers.

### T3 collected via the pre-pass

`compile_to_module` did not previously run `check_program` — only
`infer_program`. So T3 (unbound name) was not even visible to
library-mode consumers before this lane. `compile_to_module_with_diags`
runs the pre-pass first and concatenates `Env.diags` with the
typer's collected list. The CLI driver path is unchanged: T3 still
fires from `check_program` before `infer_program` as it did before.

### Multi-error fixture ordering

The shipped order is T3 first, then T4 (typer). This reflects pass
order: `check_program` runs before `infer_program` so its
diagnostics naturally precede. The fixture pins this so an LSP
client can rely on stable ordering.

## Structural surprises

- **53 InferState constructor literals**, of which 16 actually mint a
  fresh value (the others are filtered grep hits — fn signatures,
  pattern args). The 16 real ones split as: 13 with `proto_impls:
  st.proto_impls`, 1 with `proto_impls: pis`, 1 with `proto_impls: []`,
  1 with an unusual multi-line shape (`st_restore_entries`). All four
  patterns needed an `, diags: ...` append.
- **My first script run silently skipped one block** (the `type Env`
  declaration + 4 constructors) because the `assert old_block in src`
  triggered but the assertion was inside a try-free script that
  printed "OK" on a no-op replacement. Caught by tier0 on the next
  build (`no field 'diags' on Env`). Lesson: prefer Edit over Python
  bulk-replace; if you must use Python, exit non-zero on `assert`
  failure.
- **The arity-collection emit site doubles two helpers** —
  `emit_wrong_arity` (stderr) and `build_wrong_arity_diag` (structured).
  Same idea for T1 (`emit_call_arg_notes` / `build_call_type_mismatch_diag`)
  and T2 (`emit_nonexhaustive_notes` / `build_nonexhaustive_diag`).
  The duplication is small (~20 lines each) and intentional: refactoring
  the existing helpers to return both `Unit / Console` and `Diagnostic`
  would mutate selfhost. The follow-up lane that migrates more
  templates can consolidate.

## Fixtures added

Six fixtures under `examples/library_mode/`:

- `diags_t1_type_mismatch.kai` — T1 type mismatch in function call.
- `diags_t2_non_exhaustive.kai` — T2 non-exhaustive match on `type Color = Red | Green | Blue`.
- `diags_t3_unbound_name.kai` — T3 cannot find name (no did-you-mean —
  no Levenshtein candidate within distance 3).
- `diags_t4_wrong_arity.kai` — T4 wrong number of arguments (1 of 2).
- `diags_t5_missing_effect.kai` — T5 missing `Stdout` on a fn without `/ Console`.
- `diags_multiple_errors.kai` — T3 + T4 from a single compile,
  pinning the relative order.

Each pairs with a `.diags.expected` golden capturing the exact JSON
output. Run via `make test-diagnostics-collected` or the wider
`make tier1`.

## Coverage gaps left

- No fixture for the "did-you-mean" variant of T3 — i.e. a misspelling
  within Levenshtein distance 3. The shipped fixture deliberately uses
  `missing_name` (no close candidate) so the related list is empty.
  A follow-up fixture should exercise the help line.
- No fixture for T2's union-of-types path
  (`check_one_union_comp` / atomic-variant subpath) or the list-non-
  exhaustive path (`check_list_exhaustive`). These compile through
  the same `mk_diag` machinery but their related text differs
  slightly. Future migration lanes should add fixtures.
- No fixture for T1's no-callee-name path (lambda-as-callee), nor for
  arity mismatch where the callee is a higher-order argument with no
  display name. Both compile but the help line differs.

## Real cost vs estimate

The issue body and brief estimated 3-5 days. Actual cost: roughly one
focused session of an Opus 4.7 agent. The estimate assumed the full
178-site sink redirection, which we did not do. The narrowed lane
(5 emit sites, ~25 constructor sites, 6 fixtures, the
`--diags-json` driver) fits comfortably in one session.

## Follow-ups for next lanes

- **Migrate more diagnostics.** Highest-value batches:
  1. Refinement-contract violations (already structured at the message
     layer; just push `mk_diag` next to each `diag_error`).
  2. Row-label arity, alias-cycle, parametric-eff arity. All on
     `InferState`, so the field is already in place.
  3. m4c Phase 3 invariant leaks. Lives on a different state shape
     (no `InferState`); needs its own threading or a return-value
     change to `report_mono_leaks`.
- **Add `--diags-machine-json`** with shorter field names + no human
  prose, optimised for LLM agent consumption. Schema TBD; should
  preserve enough structure for LSP-style consumers.
- **Severity tuning.** Currently every collected Diagnostic is
  `SevError`. Warnings (unused bindings, ambig-efn) live in other
  helpers and should be promoted to `SevWarning` when migrated.
- **Error codes.** LSP `Diagnostic.code` field is unfilled. Adding it
  to `Diagnostic` is a straightforward record extension; the open
  design question is the code namespace (`KAI_E0001`-style,
  `kaikai/typer/type_mismatch` URL-style, or just template ids T1-T5).
- **Quick-fix anchors.** Each `RelatedInfo` currently carries a
  message but no machine-readable action. The LSP `CodeAction` API
  expects edit / command payloads — a follow-up lane can encode the
  help text into structured remediation hints.

## Files touched

- `stage2/compiler.kai` — diag types + helpers + InferState/Env/TypedDecl/
  TypedProgram fields + 5 emit-site upgrades + `compile_to_module_with_diags`
  + `dump_diags_json` + `MDiagsJson` mode + `--diags-json` flag.
- `Makefile` — `test-diagnostics-collected` target + tier1 wiring.
- `examples/library_mode/diags_t{1,2,3,4,5}_*.kai` (+`.diags.expected`).
- `examples/library_mode/diags_multiple_errors.kai` (+`.diags.expected`).
- `docs/lane-experience-issue-487-collectable-diagnostics.md` (this file).
