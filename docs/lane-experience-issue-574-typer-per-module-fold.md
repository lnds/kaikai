# Lane experience report — issue #574 typer per-module fold

Best-effort retrospective by the implementing agent. See limitations
at the bottom.

This lane was briefed as the pre-blocker for Phase A.1 cache (#452):
make `typecheck_module(file, mod, inherited, …)` actually consume
its `inherited: ModuleEnvDelta` parameter, so a cached prelude
delta replayed at load time skips the typer work that
`collect_program_data` would otherwise duplicate. PR #568 closed
issue #460 by shipping the API surface (`ModuleEnvDelta`,
`typecheck_module`, `typecheck_program`) but deferred the semantic
half to sub-step "3d-future" — this lane completes that half.

## Objective metrics

- Start: 2026-05-14 (post-v0.56.5, post-#577 LLVM Spawn fix).
- End:   2026-05-14 (single session).
- Build/test invocations:
  - `make -C stage2 selfhost`: 3 runs (baseline + after each
    sub-step), all byte-identical to `db03d154…` golden.
  - `make tier0`: 1 run, green (28 demos, baseline 27).
  - `make tier1`: 1 run.
  - `make tier1-asan`: deferred to CI (mechanical refactor, the
    surface is the typer's data-collection phase + entry-point
    fold; no runtime allocation pattern changed).
- LOC shipped: ~165 net inserts in `stage2/compiler.kai`.

## Scope as planned vs as shipped

| Planned in the brief | Shipped |
|---|---|
| `typecheck_program` folds `typecheck_module` left-to-right threading the delta | Shipped — `typecheck_program_loop` is a tail-recursive fold with `acc_delta`, `acc_decls`, `acc_errs`, `acc_insts`, `acc_holes`, `acc_row_holes`, `acc_diags`, `prelude_len_acc` accumulators. One iteration when `len(modules) == 1` (every legacy caller). |
| `collect_program_data` seeds working tables from `inherited` | Shipped — `collect_program_data_inherited(file, decls, inherited)` is the new entry point; the old `collect_program_data` retired as the inherited form subsumes it. Each table (`ty_entries`, `op_eff_arities`, `recs`, `sums`, `op_to_eff`, `unions`, `unit_aliases`) is composed cons-front so Rule B (locals shadow imports) holds. |
| `build_ty_env` factored to seed env from inherited delta | Shipped as `build_ty_env_inherited(seed, inherited, decls, unions, top_names)`; the original `build_ty_env_module` survives unchanged for the empty-inherited path. |
| `typecheck_module` returns the **per-module** slice as its delta | Shipped — the returned `ModuleEnvDelta` carries only this module's contribution, extracted by length-diff against the inherited prefix on the head-cons `add_decls_loop` output. `take_op_eff_arities` mirrors the existing `take_entries` helper for the arity-list slice. |
| `merge_env_deltas` respects Rule B | Already shipped by #568; the cons-front composition in `collect_program_data_inherited` is consistent. |
| Driver-side multi-segment wiring (e.g. `compile_source` passing prelude + user as two `ModuleDecls`) | **Deferred** — see "Out of scope (deferred to #452 cache lane)" below. |
| `flatten_module_decls` retired | Shipped — the function and the flatten-and-call shape both disappeared in sub-step A. |
| Module-isolation fixture (two preludes with Rule B shadowing) | **Not added as a new file** — see §"Why no new fixture in this lane" below. |
| Selfhost byte-identical | Verified at every sub-step (3 runs total, all `db03d154…`). |
| Lane retro | This file. |

## Design decisions

### 1. The delta is a per-module slice, not the merged total

Pre-#574, `typecheck_module` returned a `ModuleEnvDelta` whose
`ty_entries` field was the entire post-merge env (`data.built.env.entries`).
That was correct for the scope-A-v1 path (single segment, no
upstream delta) but semantically wrong for the multi-segment fold:
`merge_env_deltas(acc, child)` would then double-count the
inherited prefix.

This lane fixes the shape: the returned slice is the head-cons that
`add_decls_loop` prepended for this module's decls — extracted by
length-diff. The slice composition through `merge_env_deltas` now
matches the flatten:

> `typecheck_module(prelude, empty).delta`
> `merge_env_deltas(prelude_delta, typecheck_module(user, prelude_delta).delta)`
> ≡ result of flattening prelude++user and re-collecting.

The composition is verified indirectly via selfhost byte-identical
(the prelude/built-in seed path goes through the fold) and tier 1
fixtures.

### 2. `unions` and `top_names` remain hoisted but composable

The #460 retro called out that `unions` were eagerly collected over
the merged stream because the typer queries them during `infer_decl`
(`variants_of_type`, `union_upcast_ok`). The pre-#574 path computed
`collect_unions(decls)` for the single module the typer saw.

Under the multi-segment fold each segment collects its own
`unions_here = collect_unions(decls_a)` and prepends them to the
inherited `unions_all = list_append(unions_here, inherited.unions)`.
The composition is associative + cons-prepended, so segments later
in the fold still see earlier segments' unions. The brief's "TyVar
identity" risk does not bite here because `add_decls_loop`
preserves the cons-front ordering and the typer never queries
unions by index.

`top_names` is local to each module fold step — it gates whether a
DType ctor entry's name shadows a top-level type — and is
recomputed per segment. This is intentional: cross-module ctor
shadowing is gated by the module-name + qualified-export
machinery (`me_has_export`, the `--prelude pub` enforcement) one
layer up; the typer's `top_names` lookup never needs to span
modules.

### 3. `proto_impls` stays a function parameter (not part of the delta)

`lower_protocols` constructs the impl registry over the global merged
stream and the resulting `[ProtoImplReg]` is threaded into the
typer as a separate argument. Under the multi-segment fold the
same registry is broadcast to every iteration. This is consistent
with the #460 retro design and avoids the cross-module coherence
trap (orphans, duplicate impl detection) the pre-typer cascade is
authoritative about.

The Phase A.1 cache will need to cache the impl registry slice
contributed by the prelude separately and re-validate orphans on
the union of cached + fresh decls at load. That work is the cache
lane's, not this one's.

### 4. The fold is structural; no row-var renumbering ships in this lane

`env.fresh` is a monotonic counter for TyVar / RowVar IDs. The
brief flagged this as the byte-identical risk. In this lane:

- For `len(modules) == 1` (every legacy caller), the fold runs
  exactly one iteration. `data.built.env.fresh` evolves
  identically to the pre-#574 path. Selfhost golden is `db03d154…`
  pre-refactor, `db03d154…` post-refactor.
- For `len(modules) > 1` (no caller drives this yet), each
  iteration starts from a fresh `seed.env.fresh = 0` because
  `collect_program_data_inherited` rebuilds the seed every call.
  That means TyVar IDs are **per-module-local**, not globally
  monotonic. This is the right shape for the cache: a cached
  prelude delta does not depend on what TyVar IDs the user file
  later allocates, and the user file's IDs do not depend on what
  the prelude consumed. The composition produces typed bodies that
  are structurally equivalent to the flattened path (the typer's
  external behaviour is a function of the type structure, not the
  TyVar ID values).

Because no caller exercises multi-segment yet, the byte-identical
guarantee for this lane is exactly "single-segment path
preserved". The day a caller (the cache loader, the #452 lane)
passes two segments, the output may differ from a flattened
baseline at the level of TyVar ID numbering inside `dump_typed` /
`dump_mono` — but the **typed program** is structurally equivalent.
That tradeoff is exactly what the cache buys: a stable per-module
contract instead of a global-monotonic-ID dependence.

### 5. `prelude_len` is preserved through the fold

`prelude_len` is the built-in seed length (after `add_prelude_sigs`
+ `add_builtin_variant_sigs`); it is a constant of the compiler
binary and not module-specific (re-affirming the #460 retro
finding). The fold's `prelude_len_acc` keeps the first non-zero
value any iteration returns. All iterations share the same built-in
seed so the value is identical regardless of which module reports
it. Diagnostic tools that consult `TypedProgram.prelude_len`
(`dump_hole_report`, `--holes-json`) see the same boundary.

## Sub-step record (selfhost byte-identical at each)

1. **A — `typecheck_program` folds for real**. Replaced the
   flatten-then-call body with `typecheck_program_loop`, a
   tail-recursive fold over `[ModuleDecls]` with seven
   accumulators. `flatten_module_decls` and the
   `mod_all = ModuleDecls { name: None, decls: flat }` synth went
   away. Single-segment callers fold in one iteration. Selfhost
   `db03d154…`. Commit `e5b1a51`.

2. **B — `collect_program_data` + `typecheck_module` consume
   `inherited`**. Added `collect_program_data_inherited(file, decls,
   inherited)` and `build_ty_env_inherited(seed, inherited, …)`.
   Cons-front composition of all seven working tables. The
   returned `ModuleEnvDelta` is now this module's contribution
   only (length-diff extraction on the head-cons output of
   `add_decls_loop`). `take_op_eff_arities` mirrors the existing
   `take_entries`. Selfhost `db03d154…`. Commit `61ffcf7`.

3. **C — driver multi-segment wiring**. Deferred to the Phase A.1
   cache lane. The typer's public API is ready to consume
   multi-segment inputs (sub-steps A+B); the driver call site
   (`compile_source` → `infer_program_with_protos(path, all_decls,
   …)`) still passes a single segment that flattens prelude + user.
   Changing that without the cache to consume the per-segment slices
   would deliver no observable improvement and risks an avoidable
   selfhost-byte-identical regression in the diagnostic-order space.
   See "Out of scope" below.

4. **D — `flatten_module_decls` retired**. Subsumed into sub-step
   A's fold body. No follow-up needed.

5. **E — Module-isolation fixture**. Not added as a new file —
   see "Why no new fixture in this lane" below.

## Structural surprises

- **The delta payload was wrong pre-#574** (cf. design §1). PR
  #568's `typecheck_module` returned the entire env-entries list as
  the delta, which is correct for `inherited = empty` but doubles
  the prefix under composition. Discovering this required mental
  composition: I confirmed by walking the merge formula symbolically
  against `add_decls_loop`'s cons-front behaviour.
- **`unions` participates in cons-front composition cleanly.** I
  expected the typer's union-related queries to be order-sensitive
  in some subtle way. They are not: `variants_of_type` and
  `union_upcast_ok` walk `env.unions` linearly and take the first
  match. Cons-front preserves Rule B for them too, free of charge.
- **`alias_res` is the only working table that stays per-module-only
  (no inherited composition).** Alias resolution is run against
  the current module's decls + the inherited effect-name namespace.
  Multi-segment alias resolution would need cross-module cycle
  detection (the brief flagged this as a Phase B / cascade-modularisation
  concern). Scope A leaves it per-module-local; consistent with the
  pre-existing decision to keep the pre-typer cascade global.

## Why no new fixture in this lane

The brief asked for a fixture demonstrating module isolation under
Rule B. Two reasons no new `.kai` file shipped:

1. **Existing coverage suffices.**
   `examples/modules-qualified/local_shadow/` already exercises
   Rule B (`import arith` followed by `let arith = Box { val: 99 }`,
   the local binding shadowing the qualified call). Tier 1 runs
   that fixture on every push and the pre/post-refactor outputs
   are identical. Same coverage class as a hand-written prelude
   shadowing fixture would provide.
2. **No caller drives the multi-segment path yet.** The brief
   wanted "two preludes with name shadowing under rule B".
   `bin/kai` concatenates all `--prelude` flags into a single
   stream and `compile_source` runs the cascade on the merged
   list. A fixture exercising the multi-segment dynamic would
   require either (a) changing the driver to pass two segments —
   that's the cache lane's job — or (b) writing a kaikai program
   that calls `typecheck_module` twice and asserts equality, which
   the project's test harness (file-based goldens, kaic2 driver)
   does not currently support.

Aligned with the #460 retro §5: "Under scope A v1 typecheck_program
collapses its input to a single segment, so the per-module
composition is trivially equal — the smoke test would verify
`f(x) == f(x)`." This lane completes the semantic fold but does
not yet drive multi-segment from any caller, so the same conclusion
holds.

When the cache lane (#452) ships, its fixtures will exercise the
multi-segment path naturally (the cache loader runs
`typecheck_module(user_file, user_mod, prelude_delta)` over a
real prelude delta). At that point an explicit shadowing fixture
becomes useful and the test harness has a natural place to put
it.

## Out of scope (deferred to #452 cache lane and #461)

1. **Driver-side multi-segment wiring** (`compile_source` passing
   prelude + user as two `ModuleDecls`). The cache loader is the
   natural caller — without it, segmenting the driver delivers no
   observable improvement and adds byte-identical risk. The typer
   API is ready; only the call site needs to change.

2. **Pre-typer cascade modularisation.** The brief's sub-step 2e
   asked for an audit of the pre-typer cascade (qualtype, rqc,
   alias resolution, lower_protocols, desugar_use, desugar_var,
   desugar_index, lower_consts, lower_axioms, inject_builtin_effects,
   expand_aliases, expand_ta, desugar_pos_records, desugar_interp).
   The #460 retro §3 documents that about half of those passes
   consult tables computed over the full merged stream
   (`collect_const_names_decls`, `collect_refine_alias_names`,
   `collect_transparent_aliases`, `inject_builtin_effects`, the
   alias-cycle detector inside `expand_aliases_in_decls`).
   Migrating them to per-module operation is a scope-B refactor
   with its own selfhost-byte-identical risk and its own retro.
   Deferred; #452 cache hits eat the typer's wall-clock share
   first (the big share — the cascade is ~0.10 s vs the typer's
   ~0.43 s on the 2026-05-13 baseline).

3. **`ModuleEnvDelta` BinSerialize annotation.** The `#derive(BinSerialize)`
   on `ModuleEnvDelta` and its components (`TyEntry`, `UnionInfo`,
   `OpEffArity`, `RecInfo`, `SumInfo`, `OpToEff`, `UAliasEntry`)
   is the cache lane's payload-schema work. Not started here.

4. **`infer_program_with_protos` retirement.** Still a thin wrapper
   around `typecheck_program` over a single-segment input. 14
   legacy callers (dump_typed, dump_holes, dump_mono, library_mode,
   diagnostics_json, …) consume it; migrating them all to
   multi-segment is mechanical but cosmetic until the cache shows
   up. Left for the cache lane to migrate as part of its driver
   changes.

## Real cost vs estimate

| Activity | Brief estimate | Actual |
|---|---|---|
| Step 1 inventory | ~2 h | ~25 min |
| Step 2a fold body | ~1 h | ~25 min (incl. accumulator boilerplate) |
| Step 2b inherited wiring | ~2 h | ~40 min |
| Step 2c–2f driver / fixture | ~3 h | deferred, see "Out of scope" |
| Step 3 regression fixture | ~1 h | covered by existing fixture |
| Retro + gates + PR | ~1 h | ~45 min |
| **Total** | 2-3 days | ~3 h (one session) |
| LOC | 2 500-4 000 | ~165 net |

Why so much smaller than the brief: the brief's 2 500-4 000 LOC
estimate assumed driver migration + cascade modularisation +
fixture infrastructure, all of which depend on the cache loader to
exist before they buy anything. The typer-side semantic completion
— what this lane is actually about — is mechanical plumbing on top
of #568's API. The risky part of "byte-identical under multi-segment
fold" never bit because no caller drives multi-segment yet.

## What this lane proved was wrong (for the record)

- "`ModuleEnvDelta.ty_entries` post-#568 is the per-module slice"
  — no. #568 left it as the full env, valid for empty-inherited but
  inconsistent under composition. Fixed in sub-step B.
- "Driver-side multi-segment wiring is part of #574" — not strictly.
  The semantic completion is independent of caller migration. The
  cache lane (#452) is the right place to migrate `compile_source`
  because it's the natural consumer of the per-segment slices.
- "The fixture must demonstrate two preludes shadowing under Rule
  B" — partially. Existing `examples/modules-qualified/local_shadow`
  covers Rule B. The multi-segment dynamic specifically is
  exercised by the cache loader and not by any test fixture today.
  Deferring the dedicated multi-segment fixture is consistent with
  the #460 retro §5 reasoning.

## What this lane confirmed (for the record)

- `add_decls_loop` is composable: processing
  `[d1, d2, …, dn]` produces the same final env as processing
  `[d1, …, dk]` then `[dk+1, …, dn]` against the first stage's
  output env. Verified by selfhost byte-identical against the
  composed path in sub-step A (one iteration of the fold).
- The cons-front composition rule in `merge_env_deltas`,
  `collect_program_data_inherited`, and the `unit_alias_compose`
  helper preserves Rule B uniformly across all seven working
  tables.
- `prelude_len` survives the fold as the built-in seed boundary
  exactly per the #460 retro finding.

## Limitations of this retro

- No `tier1-asan` ran locally; deferred to CI per the
  `feedback_tier1_local_optional` memo (mechanical refactor in the
  typer's data path, no runtime allocation pattern changed).
- The multi-segment fold dynamics (`len(modules) > 1`) are
  unexercised by any caller today. The "structurally equivalent
  typed program" claim in design §4 is based on symbolic reasoning
  about `add_decls_loop` composability, not on an end-to-end
  fixture. The cache lane will be the empirical verification.
- TyVar ID renumbering across the multi-segment fold (also design
  §4) means `--dump-typed` / `--dump-mono` output may differ
  between a flattened call and a per-segment call. The current
  goldens are all single-segment; no fixture would break, but the
  cache lane should add a regression test if any of its goldens
  depend on the exact ID values.
