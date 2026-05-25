# Lane experience — issue #677 phase 1n4: extract compiler/perceus.kai

The fourth and last of the vertical-pipeline pass lanes (N1 infer, N2
monomorph deferred, N3 unbox, N4 perceus). It shipped the closest to the
brief's estimate of any lane in phase 1n — the pre-sink debt N3 paid off
(`compiler/fnreg.kai`) made the registry + interp-scanner glue a clean
downward import, so this lane never needed a second pre-sink. One commit
of mechanical relocation; the only friction was a fragile `kai check`
runtime bug that is pre-existing and out of lane.

## Scope as planned (PR #692 analysis §"Lane N4" + the brief)

Move L5, the Perceus RC pass (`perceus_pass` + ~150 internal defs,
main.kai ~12358–14841 at the brief's HEAD = post-#695) into a new
`compiler/perceus.kai`. A pure `[Decl] → [Decl]` structural transform:
the analysis verified 0 uses of `InferState` / `Subst` / `TyScheme` /
`TypedProgram` / `ProtocolReg` in the range, so perceus imports
`compiler.ast` (+ now `compiler.fnreg`, the N3 pre-sink), never the
typer. Eight head symbols named: `map_expr_kind`, `pat_bindings`,
`pat_bindings_skip_raw`, `register_fn_decls`, `is_ufn_decl`, `mangle_ty`,
`body_is_ffi_extern`, `iscan_collect`. Public-surface estimate 2–4.

## Scope as shipped

Delivered as planned — one source block, one new module — with two
boundary refinements the grep-level analysis could not see.

### 1. The block boundary is 12316–14723, not 12358–14841

The brief's range started at the `perceus_pass` fn (12358) and ended at
`tcrec_rewrite_decls` (14842). The real cut:

- **Top:** the `# ---- m5 — basic Perceus` comment banner at 12316 (the
  block's docstring), right after `find_mono_inst` (monomorph) closes at
  12314.
- **Bottom:** `last_use_for_loop` closing at 14723. Lines 14725+ are the
  `# ---- m37 / TCO` banner and the `tcrec_*` family — which **stays in
  main** for the emit_c lane (it mints C goto strings + calls the
  emitter, the phase-1m sub-pass-F / N1-retro pattern). The brief's
  14841 figure would have pulled four `tcrec_*` walkers into perceus.

`tcrec_*` sits physically *below* perceus, so unlike N1 (which skipped
over TCO to grab L7 dumps) this cut just stops short of it — no
skip-over.

### 2. Public surface is 5, not 2–4 — the tco pre-pass is the extra consumer

`perceus_pass` is the only entry point the driver calls. But four
internal helpers have callers **outside the block, in the still-resident
tco pre-pass** (`pcs_make_self_tail_rewrites`, main.kai ~14873–15182):

- `pcs_collect_uses_expr` (2 callers), `pcs_branch_aware_skip_params`
  (2), `pcs_count_non_lam_uses` (2), `last_use_for` (3).

The tco rewrite reuses perceus's use-collection + last-use machinery to
align its goto-loop drops with the wrap's exit drops (so the goto path's
drops are byte-equivalent to what it jumps over). This is the **exact
shape N3 hit**: an emit-adjacent pass (there the C/LLVM emitter, here
tco) consuming a structural pass's classifiers directly. Those four +
`perceus_pass` are `pub`; the internal types (`PcsBlockState`,
`PcsRwBlock`, `PcsRwStmt`, `DupMark`) stay private (no external sig).
**5 pub fns, 0 pub types.** When the emit_c lane later extracts tco,
these four pub helpers become its downward import from perceus — the
same fnreg-style payoff, planned one lane early.

### Of the 8 named head symbols, 3 import / 5 mirror

- **Import downward (fnreg, pub since N3):** `register_fn_decls`,
  `body_is_ffi_extern`, `iscan_collect`. The N3 pre-sink's payoff: zero
  mirror cost for the whole registry + interp scanner.
- **Mirror `prc_` (still in main, not pub):** `map_expr_kind` (+ its
  9-fn AST-functor family), `mangle_ty` (+ `join_with`), `pat_bindings`
  (+ `pat_bindings_loop`, `pfield_bindings_loop`), `pat_bindings_skip_raw`,
  `is_ufn_decl`. Plus `pat_binders` (pub in modules but a single
  consumer here ⇒ mirror, not import) with its two helpers. **19
  `prc_`-prefixed mirror fns total**, copied verbatim from the canonical
  `inf_*` (infer), main, and modules definitions.

The `Use` / `LU` / `IPart` / `IScan` types the use-scanner and
last-use machinery need are **pub in infer.kai** (left there by N1,
reconfirmed by N3), so perceus imports `compiler.infer` for the types —
no new ast.kai sink. `tokenize` (lex) and `parse_interp_expr` (parse)
are imported downward for the interpolation re-scan; `LocBind` (a brief
candidate) turned out **unused** by perceus.

## Design decisions / alternatives considered

- **Mirror vs import for the 5 main-resident helpers.** Each has exactly
  one consumer in perceus ⇒ mirror per the phase-1m `dsg_` / N1
  move-vs-mirror rule (consumers = 1 ⇒ mirror, ≥2 ⇒ sink). The
  AST-functor family (`map_expr_kind`) is now mirrored a *third* time
  (desugar `dsg_`, infer `inf_`, perceus `prc_`); the standing
  AST-functor-sink follow-up from N1/N3 is now even better justified
  (consumers = 3) but stays orthogonal — sinking it touches ast.kai +
  three modules and would make this clean single-module diff
  cross-module and un-bisectable.
- **No second pre-sink needed.** N3's retro flagged "if you find glue
  shared with a future lane that isn't in fnreg, consider a pre-sink
  before continuing." Checked: perceus's only shared-with-tco helpers
  (the 4 pub use/last-use fns) are *defined by perceus itself* and
  exported downward to the future tco lane — that is the normal pub
  surface, not glue needing relocation. No pre-sink.
- **Copy `prc_` bodies from `inf_*`, not from main.** The `inf_map_*`
  family and `inf_mangle_ty` in infer.kai are already selfhost-verified
  mirrors of the main originals; renaming `inf_` → `prc_` reuses a known
  fixed point rather than re-deriving from main's source.

## Structural surprises

- **`kai check` String-generator fragility (pre-existing, out of lane).**
  A property check with a `String` generator (`with nm: String`) that
  imports any heavy compiler module (`compiler.infer` and up) crashes
  the `--prop-check` runtime with a Bus error / Trace trap on the
  *second* check in the file — and it is **content-fragile**: the
  shipped `test_unbox.kai` (2 String checks) passes, but a near-identical
  file with a trivially different second check body crashes. Reproduced
  on clean `main` (22a80ae) with the `test_unbox.kai` shell and a trivial
  second String check ⇒ **not caused by this lane**. Worked around by
  writing both `test_perceus.kai` property checks with an `Int`
  generator (stable). This is a runtime/RC-discipline bug (the project's
  known "emitter has no RC discipline / 97% allocs leak" shape); it
  deserves its own issue but opening one is unauthorised here, so it is
  logged in this retro for the next maintainer.
- **The empty-list LU sentinel.** First `Int`-generator check
  (`last_use_for of a single non-lambda use is always LUAt`) found a real
  counterexample at `n = -1`: `last_use_for_loop` seeds `mx_line = -1` as
  the "unseen" sentinel, so a use at line/col `< 0` classifies as
  `LUUnused`, not `LUAt`. Real ASTs are 1-based, so the check now clamps
  the generator to `>= 0`. The property is true for valid positions; the
  counterexample is a correct report about an out-of-domain input.
- **The N1 upward-ref discipline held with zero surprises.** A
  comment-stripped external-call scan of the finished perceus.kai
  resolved every lowercase callee to an imported module or the prelude —
  **zero MAIN-ONLY symbols**, so no missing-mirror like N1's
  `inf_indent_of`. Running the scan *before* the first compile (rather
  than waiting for a modular-compile rejection) caught the would-be gaps
  up front; the mirrors were complete on the first `make kaic2`.

## Fixtures / coverage

`stage2/tests/test_perceus.kai` (167 LOC): **13 unit tests + 2 property
checks**, all green (`kai test` 13/13; `kai check` 2/2, 100 iter each).

- Unit tests: the four pub use/last-use helpers exercised directly —
  `last_use_for` (LUUnused / LUAt / LUBlocked classification + latest-position
  pick + other-name isolation), `pcs_count_non_lam_uses` (matching /
  non-matching / in-lambda exclusion), `pcs_branch_aware_skip_params`
  (empty param list) — plus **four end-to-end** `perceus_pass` tests
  driven through the real parser + `infer_program` (so each Expr carries
  the `.ty` / `.mode` the pass reads) asserting decl-count conservation
  on 0-, 1-, 2-fn and multi-use-param modules. These exercise the
  private rewrite walk (`perceus_decl` / `pcs_rewrite_expr` /
  `pcs_prepend_unused_drops` / the dup-marking) transitively.
- Property checks (body Bool, `Int` generator): `pcs_count_non_lam_uses`
  additive in its accumulator; a single non-lambda use (clamped `>= 0`)
  always classifies as `LUAt`.

Not tier-wired, consistent with every other `tests/test_*.kai` (the
per-module runner wiring is project-wide Phase 2, #452 / #677). The pass
is exhaustively exercised on every selfhost regardless.

## Cost vs estimate

Estimate: ~2 100 LOC moved, ast-only (+ fnreg) import, 2–4 pub. Actual:
2 408 LOC left main (29335 → 26928), 2 738 LOC in perceus.kai (block +
19 mirrors + header), 5 pub fns. One commit for the extraction. The
closest-to-estimate lane of phase 1n — the N3 pre-sink eliminated the
boundary-discovery cost that made N1 and N3 expensive; the only
unplanned time went to diagnosing the pre-existing `kai check` String
fragility (and confirming it on main).

## Follow-ups for next lanes

- **Lane N2 (monomorph)** is the remaining vertical pass. It consumes
  typer types from infer.kai (`TypedProgram` / `ResolvedCS` / `TyScheme`,
  all pub) and the `Proto*Reg` types (sunk in #693), not the perceus or
  fnreg registries. Cut starts at the `# ---- --dump-mono` banner per the
  N1 retro; the leak detectors (`count_spec_leaks`, `find_first_leak_*`,
  `count_tyvar_*`) and the `SubstMap` family belong to it.
- **Emit_c / tco lane.** When tco extracts, it imports `compiler.perceus`
  downward for the 4 pub use/last-use helpers (`pcs_collect_uses_expr`,
  `pcs_branch_aware_skip_params`, `pcs_count_non_lam_uses`,
  `last_use_for`) — already pub for exactly this. tco also calls the
  emitter (`emit_expr` / `c_sym` / `raw_c_type`), so it must extract
  *with* emit_c, not before it.
- **AST-functor sink lane (open since N1, now 3 consumers).**
  `map_expr_kind` + the `map_*_exprs` family is mirrored in desugar
  (`dsg_`), infer (`inf_`), and now perceus (`prc_`). Cardinality 3 ⇒
  the sink to `compiler.ast` as `pub` is the clear next mechanical lane;
  it would drop all three mirror sets.
- **`kai check` String-generator crash (needs an issue).** A `String`
  property generator under a heavy-module import crashes the
  `--prop-check` runtime fragilely (content-dependent, second-check).
  Reproducible on `main`. Out of this lane's scope; flagged for the
  maintainer to file + triage (likely the RC-discipline gap).
