# Lane experience — issue #677 phase 1n2: extract compiler/monomorph.kai

The L3 layer of the cross-section analysis
(`docs/lane-infer-cross-section-analysis.md` §"Lane N2"): the m4c
monomorphisation pass moves out of `stage2/main.kai` into a new
`stage2/compiler/monomorph.kai`. This is the **relaunch** of a lane that
was abandoned cleanly (no commits) when it discovered `monomorphise`
calls `resolve_protocol_calls_decl` — 537 LOC that, at the time, lived
inside the protocols block. PR #698 extracted protocols as
`compiler.protos`, so this lane imports that helper downward and the
coupling is no longer a blocker.

## Scope as planned (the brief)

- Move ~1174 LOC (entry `monomorphise` at line 11262 through line 12435).
- Import `compiler.protos` (for `resolve_protocol_calls_decl`) and
  `compiler.infer` (for `TypedProgram` / `ResolvedCS` / `TyScheme` +
  ~6 pub typer helpers). Mirror ~8 head symbols with a `mono_` prefix.
- Pub surface 8–12. Add `test_monomorph.kai` + this retro.

## Scope as shipped

- Moved **1504 LOC** (the m4c region 10814–12317), not 1174. Pub
  surface **4** (1 fn + 3 types), below the 8–12 estimate. **11
  mirrors** (the 8 estimate undercounted the AST-functor helper family
  + `join_with`). 11 unit tests + 2 property checks.

### 1. The brief's line boundary was wrong on both ends

The brief said "11262–12435, entry point `monomorphise` at 11262." The
real pass is **10814–12317**:

- **Above 11262**: the m4c region opens at the `# ---- m4c` header on
  line 10814 with ~30 leak-detection / tyvar-counting helpers
  (`count_spec_leaks`, `first_spec_leak`, `find_first_leak_in_*`,
  `count_tyvar_in_*`) and the `SubstMap` type — all consumed only by
  the pass, none with an external caller. The brief picked the entry
  *fn* and missed its own helper prelude.
- **Below 12317**: the brief's tail (12435, `tcrec_rewrite_decls`)
  swept in **four TCO helpers** (`tcrec_has_tail_self_call`,
  `tcrec_walk_tail`, `tcrec_walk_arms`, `tcrec_block_scope`, starting
  at 12346). TCO is L6 — emit-coupled, explicitly **deferred to the
  emit_c lane** by the same analysis doc. Including them would have
  inverted emit→tco into tco→emit and dragged the goto-string minting
  into monomorph. The clean cut is after `find_mono_inst` (12317),
  before the `# ---- m37 / TCO` header (12319).

Lesson: the entry-point fn name is a stable anchor, but the *layer
extent* must be read from the section headers (`# ---- m4c …` to
`# ---- m37 / TCO …`), not from the entry fn ± an estimated LOC. The
brief's range was a guess; the grep + header scan corrected it.

### 2. Most of the "9 head symbols" were already pub in infer

The analysis listed 9 outbound head symbols. The cross-section scan
found **6 of them already pub in `compiler.infer`** (`fn_scheme_of_decl`,
`dim_collapse`, `unit_canon`, `normalize_union`,
`collect_implicit_tparams_in_decl`, `tparam_id_split`) plus
`module_slot_compat` (also infer, not on the brief's list), so
`import compiler.infer` covers them with **no mirror**. Only **3** were
genuinely main-private and needed mirroring: `mangle_name`,
`map_expr_kind`, `unit_expr_display` — plus `ty_to_string` (also not on
the brief's list, found by the scan). `resolve_protocol_calls_decl` is
pub in protos → import, no mirror.

So the mirror set is the AST-functor family + the manglers + the
display tree, not the typer helpers. After transitive closure:
`mono_map_expr_kind` + 8 map helpers, `mono_mangle_name` /
`mono_mangle_ty` / `mono_mangle_unit` / `mono_mangle_unit_ident` (+ loop),
`mono_ty_to_string` / `mono_ty_string_list` / `mono_row_to_string_suffix`
/ `mono_unit_canon_display` / `mono_unit_expr_display`, and
`mono_join_with`. `labels_empty` / `label_to_string` (needed by
`mono_row_to_string_suffix`) are **pub in infer** so they stay bare.

### 3. `mono_map_expr_kind` is the FIFTH copy of `map_expr_kind`

`map_expr_kind` is now mirrored in desugar (`dsg_`), infer (`inf_`),
perceus (`prc_`), protos (`proto_`), and monomorph (`mono_`) — the
AST-functor family triplication the N1 retro (#5) flagged. Per the
brief I mirrored rather than blocked; **flagging it here as a sink
candidate for a future AST-functor consolidation lane**. Five identical
50-line copies of the same `ExprKind → ExprKind` functor (plus their
helper families) is the strongest sink signal in the codebase. It is
not blocking and not this lane's job.

### 4. The leak-report types moved to monomorph and went pub

`LeakLoc` / `MLeakRecord` lived in the main *head* (lines 70–71) next to
`report_mono_leaks`, but every value of them is built inside the pass
and the driver consumes them via the `[MLeakRecord]` carried in
`MonoOutput`. They moved into `monomorph.kai` as **pub types**;
`report_mono_leaks` stays in main and reads them through the downward
`import compiler.monomorph`. This is cleaner than an ast sink — they are
monomorph-specific concepts, not shared AST, and main already imports
monomorph for `monomorphise`.

## Cross-section findings for downstream lanes

- **emit_c (includes TCO).** The four `tcrec_*` walkers this lane left
  behind (12346+) are the M1 half of the TCO pass; the full TCO block
  runs to the `# LLVM backend` header (~13559 pre-#698-merge, drifts on
  the parallel `emit-llvm-extract` merge). They call
  `emit_expr` / `emit_expr_raw` / `c_sym` / `raw_c_type` and consult
  the pub perceus use/last-use helpers (`pcs_count_non_lam_uses` etc.) —
  extract TCO **with** emit_c, never standalone, never with monomorph.
- **driver (apex).** The driver's monomorphise call site
  (`let mono_out = monomorphise(typed_prog, proto_reg,
  post_local_arities)` + the `MonoOut(ds, _)` / `MonoOut(_, ls)`
  destructures + `report_mono_leaks`) is the only external consumer.
  When the driver finally extracts, it imports `compiler.monomorph` for
  `monomorphise` + the three result types. Nothing else crosses.
- **The flat-bundle blind spot bit again.** `make kaic2` builds the
  kaic1-concatenated `build/bundle.kai` (a flat file where module
  privacy does not apply) — it compiled green even though
  `mono_*` mirrors called `join_with`, which is **private to main**.
  Only `make selfhost` (the modular kaic2 recompiling the package with
  real module boundaries) surfaced the `join_with is private to
  module main` errors. Same lesson as N1/N3/N4: **the bundle hides
  privacy violations; the modular selfhost is the real gate.** Run
  `make selfhost`, not just `make kaic2`, before trusting an extraction.

## Fixtures + coverage

`stage2/tests/test_monomorph.kai`: 11 unit tests + 2 property checks.
Because the pub surface is one fn + three result types (all helpers
private), coverage drives `monomorphise` end-to-end through
`parse_program` + `infer_program` with an empty `ProtocolReg`:

- non-generic programs preserve decl ordering / names and stay
  leak-free (the m4c Phase 3 invariant holds trivially);
- a generic `id[T]` used at a concrete type specialises while staying
  leak-free;
- the pub result types are exercised for constructibility + projection.

Property checks (Int generators only — String generators trip the
pre-existing `--prop-check` fragility): `MonoOut` round-trips its leak
list length and its decl list length.

Coverage gap: the substitution engine, call rewriters, and impl-tuple
discovery are exercised only transitively (no concrete generic that
forces a `__pimpl_*` rewrite in the test, since that needs a protocol
impl + the real driver wiring). The selfhost run covers them — the
whole self-compile rides this pass — but the unit file does not isolate
them. Acceptable: they have no isolatable pub entry.

## Cost vs estimate

Estimate (brief): ~1174 LOC, one PR. Actual: 1504 LOC moved, 4 pub, 11
mirrors, 2 commits (extraction + tests). The boundary-correction
(reading the real m4c extent vs the brief's guessed range) and the
`join_with` privacy fix found only by `make selfhost` were the two time
sinks; both are now documented as the standing checks above.

## Public surface (4)

- `pub fn monomorphise(tp, reg, local_arities) : MonoOutput` — entry.
- `pub type MonoOutput = MonoOut([Decl], [MLeakRecord])` — driver result.
- `pub type MLeakRecord = MLeakRec(String, Int, Option[LeakLoc])` —
  consumed by main's `report_mono_leaks`.
- `pub type LeakLoc = MLeakAt(Int, Int, String)` — embedded in the above.

Everything else — 86 helper fns + 7 internal types (`MonoTuple`,
`SubstMap`, `GenSpecsResult`, `SpecEmitOut`, `PolyFn`, `SubstBindings`,
`MonoInstPair`, `SubstPairTy`, `SubstPairUnit`) — stays private.

## Verifications

- `make kaic2` — modular bundle builds clean.
- `make selfhost` — **kaic2b.c == kaic2c.c** (byte-identical).
- `kai test stage2/.` — test_monomorph 11/11, all sibling module tests
  still green (test_protos 16/16, test_perceus 13/13, test_infer, …).
- `kai check stage2/tests/test_monomorph.kai` — 2/2 property checks.
- fizzbuzz smoke compile + run via the new kaic2 — correct output.
