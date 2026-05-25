# Lane experience — issue #677 phase 1n3: extract compiler/unbox.kai

The "easy warm-up" of the remaining vertical-pipeline lanes, per the
cross-section analysis (docs/lane-infer-cross-section-analysis.md
§"Lane N3"). It was not easy. The lane shipped as **two commits**: a
pre-sink that the analysis did not anticipate, then the unbox extraction
the brief actually asked for.

## Scope as planned (PR #692 analysis + the brief)

Move the Tier 2.5 unboxing pass (`unbox_pass` + ~50 internal defs,
main.kai ~12943–13842 at the brief's HEAD) into a new
`compiler/unbox.kai` importing **`compiler.ast` only** — a pure
`[Decl] → [Decl]` structural pass with zero typer-type dependencies.
The analysis verified 0 uses of `InferState` / `Subst` / `TyScheme` /
`TypedProgram` / `ProtocolReg` in the range and estimated a 2–4 symbol
public surface. Framed as the smallest of the five lanes, a good warm-up
after the massive N1 (infer.kai).

## Scope as shipped

The "imports ast only" framing was **wrong**, and measuring the real
coupling reshaped the lane into a pre-sink + the extraction.

### The unbox pass weaves three shared subsystems through its walk

The brief assumed the registry/scanner glue (`efn_resolve`,
`lookup_ufn_sig`, `register_fn_decls` — the 3 helpers the analysis
named) was trivially mirrorable into unbox with a `ubx_` prefix, the
phase-1m `dsg_` discipline. It is not, for two reasons the analysis (a
grep-level scan, never compiled) could not see:

1. **`register_fn_decls` drags the whole unbox classifier.** Its body
   calls `fn_of_decl → classify_unbox_sig`, whose transitive closure
   (call-position-only, comment-stripped — the N1 boundary-stop
   discipline) is **49 functions**: the `body_*_effectful` family
   (~12), `all_params_unboxable`, `resolve_ty` / `row_of_expr` /
   `row_of_expr_with_rbinds` + the row helpers, `param_ty`,
   `ty_is_unboxable_t`, `body_is_ffi_extern`. A naïve token scan
   reported 1497 — the same 10×+ inflation N1 warned about; counting
   only `name(` call sites and stopping at module boundaries collapsed
   it to 49.

2. **The registry and the use-scanner are shared by THREE consumers,**
   not just unbox: main (the C/LLVM emitters + the bare-name-ambiguity
   diagnostic), unbox, and the future perceus lane (N4). `EFn` is
   171×-resident in main; `register_fn_decls` has 8 main callers;
   `scan_uses_*` has 55. Per the asu cardinality rule (consumers ≥2 ⇒
   sink to the module everyone imports downward, =1 ⇒ mirror), mirroring
   the 49-fn classifier into unbox AND again into perceus is exactly the
   duplication N1's move-vs-mirror rule forbids.

### Resolution: a pre-sink lane (asu consult, user-endorsed)

Commit 1 creates **`compiler/fnreg.kai`** between infer and unbox in the
bundle order, holding the 49 classifier/registry/scanner fns + `EFn` /
`UFnSig`. The decisive measurement that made this a *mechanical* MOVE
rather than a redesign: the 49-fn closure's only downward externals are
`cat3` / `escape_str_body_for_c` (util), `parse_interp_expr` (parse),
and `string_contains_dot` / `unit_canon` (infer, both already `pub`).
Zero upward references — verified no module above fnreg (util…infer)
calls any of the 49. The `Use` / `IPart` / `IScan` types the scanner
needs were **already `pub` in infer.kai** (the N1 lane left them there),
so fnreg imports infer downward and **no new ast.kai sink was needed** —
correcting the asu pre-flight's guess that `Use` and the AST→Ty utils
should sink to ast.

Commit 2 is the unbox extraction the brief described, now importing
`compiler.fnreg` for the registry+scanner instead of mirroring it.

### Public surface: fnreg 51, unbox 8

- **fnreg.kai: 49 pub fns + 2 pub types** (`EFn` / `UFnSig`). Every one
  of the 49 is `pub` because main calls each downward — this is
  infrastructure, not a curated API, the same "wide because the driver
  orchestrates a wide surface" shape N1 hit (49 pub fns there too).
- **unbox.kai: 8 pub fns, 0 pub types.** The analysis estimated 2–4. The
  extra came from the C/LLVM emitter consuming the raw-op classifiers
  directly: beyond `unbox_pass`, the emitter calls `op_is_raw_arith` /
  `op_is_raw_cmp` / `op_is_raw_logical` / `raw_op_str` /
  `ty_is_integral_raw` / `uses_have_in_lam` / `expr_has_interp_use` to
  decide native-operator lowering. `LocBind` / `UbStmts` / `UbStmt`
  stay private (in no external signature).

## Design decisions / alternatives considered

- **Mirror-everything (option A) rejected.** Mirroring `register_fn_decls`
  means mirroring the 49-fn classifier closure into unbox AND perceus —
  the deep-tree duplication N1's rule prohibits. The pre-sink pays the
  glue once for both N3 and N4.
- **Sink to ast.kai (option C) rejected.** The classifier is unboxing
  *policy* (which fns take raw scalars), not an AST node type; ast.kai
  is for AST shapes. A dedicated `fnreg.kai` keeps ast.kai clean.
- **Re-scope to the pure walk (option D) rejected.** The walk threads
  `fns: [EFn]` through every node and resolves UFn-ness mid-walk
  (`call_resolves_to_ufn`); there is no clean seam that leaves the
  registry in main without a lateral import back up.
- **Two commits, one PR (not two PRs).** The pre-sink is a pure
  relocation with a byte-checkable selfhost fixed point, so it is its
  own bisectable commit; bundling both in one PR keeps the integrator's
  merge atomic while preserving per-commit localisation.

## Structural surprises

- **Selfhost determinism is the real gate; byte-identical-vs-baseline is
  not achievable for a type move.** Moving `EFn` / `UFnSig` / `LocBind`
  / `UbStmts` / `UbStmt` to different modules **renumbers the global
  user-variant tags** (assigned by declaration order, per
  docs/variant-tags.md), and the gensym / lambda-id counters shift with
  the reorganised program text. After normalising away (a) the
  `kai_fnreg__` / `kai_unbox__` module symbol prefixes, (b) variant-tag
  numbers, and (c) gensym/lambda counters, the diff vs the pre-lane
  baseline is **empty** — pure relocation, no logic change. The tag
  renumbering is correctness-neutral (the matcher still routes on the
  variant *name*; CLAUDE.md / variant-tags doc confirm). `selfhost
  determinism: OK (kaic2b.c == kaic2c.c)` is the authoritative proof the
  new compiler is a fixed point.
- **The N1 lesson held: the flat bundle hides nothing here because the
  cut was clean, but the modular selfhost is what would have caught a
  missing pub.** All 49 fnreg fns + 8 unbox fns compiled modularly on
  the first selfhost — no missing-mirror surprise like N1's
  `inf_indent_of`, because fnreg/unbox call *downward* into already-pub
  infer symbols rather than mirroring main helpers.
- **`TyRefineT(base, pred)` carries a syntactic `Expr` predicate**, not a
  list — a test that tried `TyRefineT(TyInt, [])` failed to construct;
  swapped to a `TyDimT` recursion case (`TyDimT(TyReal, UOne)` is not
  integral-raw).

## Fixtures / coverage

`stage2/tests/test_unbox.kai` (≈140 LOC): **14 unit tests + 2 property
checks**, all green (`--test` harness 14/14; checks 100 iter each OK;
`--check` HM-clean, exit 0).

- Unit tests: the pure raw-op classifiers (`op_is_raw_arith` /
  `op_is_raw_cmp` / `op_is_raw_logical` / `raw_op_str`),
  `ty_is_integral_raw` (incl. the Dim-strip recursion), the use-scanner
  consumers (`uses_have_in_lam` / `expr_has_interp_use`), and three
  **end-to-end** `unbox_pass` tests driven through the real parser +
  `infer_program` (so each Expr carries the `.ty` the pass reads) that
  assert decl-count conservation on 0-, 1-, and 2-fn modules.
- Property checks (body Bool, not assert): `raw_op_str` is idempotent;
  the three raw-op classes are mutually exclusive (`a + c + l <= 1`).

Not tier-wired, consistent with every other `tests/test_*.kai` — the
per-module runner wiring is the project-wide Phase 2 (#452 / #677). The
`--test` harness's `main` currently invokes only `test` blocks, not
`check` blocks (checks compile + typecheck but a manual call confirmed
runtime pass); wiring checks into the runner is the same deferred item.
The pass is exhaustively exercised on every selfhost regardless.

## Cost vs estimate

Estimate: smallest lane, 1–3 commits, ~900 LOC moved, ast-only import.
Actual: 2 commits, ~1570 LOC left main (776 to fnreg + ~790 to unbox),
4 modules touched (main + Makefile + 2 new). The bulk of the cost was
the **boundary discovery** — establishing that the registry+scanner is
shared infrastructure (not mirrorable glue), bounding the classifier
closure to 49, and confirming zero upward refs — not the mechanical
move. The pre-sink was the right call but it makes N3 a peer of N1 in
*structural* difficulty even though it is a fraction of the LOC.

## Follow-ups for next lanes

- **Lane N4 (perceus) now imports `compiler.fnreg` for free.** The
  registry (`register_fn_decls` / `efn_resolve`) and the use-scanner
  (`scan_uses_expr` + family) it needs are pub in fnreg below it. No
  mirror, no second pre-sink. perceus imports `compiler.ast` +
  `compiler.fnreg` (+ `compiler.infer` for `Use` if it references the
  type directly). This is the payoff that justified the pre-sink.
- **Lane N2 (monomorph)** is unaffected — it consumes typer types from
  infer.kai, not the fnreg registry. Cut still starts at the
  `# ---- --dump-mono` banner per the N1 retro.
- **The AST-functor sink lane (still open from N1)** is orthogonal:
  `map_expr_kind` + `map_*_exprs` are mirrored in desugar (`dsg_`) and
  infer (`inf_`); unbox does NOT mirror them (it open-codes its tree
  walk), so the sink lane's scope is unchanged.
- **Checks-in-runner wiring (#452 / #677 Phase 2)** would let
  `tests/test_*.kai` check blocks run without a manual harness patch.
