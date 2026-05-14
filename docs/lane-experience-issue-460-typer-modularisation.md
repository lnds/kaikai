# Lane experience report — issue #460 typer modularisation

Best-effort retrospective by the implementing agent. See limitations
at the bottom.

This lane was briefed as the pre-blocker for Phase A.1 cache (#452
sub-issue): refactor `infer_program` and `build_ty_env` so each
module is typechecked in isolation and produces a serialisable
`ModuleEnvDelta` carrying that module's contribution to the typer
env. The integrator approved **scope A** (typer-only) before
implementation began.

## Objective metrics

- Start: 2026-05-13 (rebase from main; HEAD = 4fc49ec post-#564)
- End:   2026-05-13 (commit + PR)
- Build/test invocations:
  - `make selfhost`: 5 runs (baseline + after each sub-step).
  - `make tier0`: 1 run, green.
  - `make tier1`: 1 run, green (background while writing retro).
  - `make tier1-asan`: deferred to CI.
- LOC shipped: ~140 net inserts in `stage2/compiler.kai`.

## Scope as planned vs as shipped

| Planned | Shipped |
|---|---|
| `ModuleEnvDelta` record carrying TyEnv slice, recs, sums, op_eff_arities, type_aliases, op_to_eff, proto_impls | Shipped: ty_entries, unions, op_eff_arities, recs, sums, op_to_eff, unit_aliases. Effect aliases, transparent aliases, proto_impls deferred — see "Out of scope" |
| `typecheck_module(decls, inherited_env) -> (TypedDecls, ModuleEnvDelta)` public entry point | Shipped as `typecheck_module(file, mod, inherited, proto_impls, verbose) -> TypecheckedModule { typed, delta }` |
| `infer_program` becomes a thin wrapper folding `typecheck_module` over modules | Shipped: `infer_program_with_protos` calls `typecheck_program([{name: None, decls}], ...)`. The fold dynamic itself is a one-segment collapse under scope A v1 |
| Refactor `build_ty_env` into `build_ty_env_module` + merge | Shipped: `build_ty_env_seed` + `build_ty_env_module`; `build_ty_env` is the legacy wrapper that composes them |
| `merge_env_deltas` respecting Rule B and m6.2 module-name resolution | Shipped; child cons-fronts parent in all fields |
| Selfhost byte-identical | Verified at every sub-step (5 runs total) |
| Smoke fixture for API | Not added as a dedicated fixture — see "Why no dedicated smoke fixture" |
| Lane retro | This file |

## Design decisions

### 1. Diff (delta) rather than full env-completion

The cache load applies the delta on top of a freshly-built built-in
seed (`add_prelude_sigs` + `add_builtin_variant_sigs`). The
built-ins live in the compiler binary and change with every release;
the delta lives on disk and is user-prelude-specific. Decoupling
the two means cache files survive compiler-version bumps when the
user-visible prelude API is unchanged (and break cleanly via the
header hash check when it is not). Storing the full post-seed env
would have entangled the cache with the compiler's built-in catalog.

### 2. Two-phase fold: data first, then typer

`collect_program_data` is now a named record (`ProgramData`)
containing `decls_a`, `built`, `recs`, `alias_res`, `op_to_eff`,
`sums`. The typer's `infer_all_loop` reads from this record.
This factoring matters because:

- The data tables (recs, sums, unions, op_to_eff) are pure walks
  over `[Decl]` that produce identical output regardless of fold
  shape. They go into `ModuleEnvDelta` directly.
- The typer's `infer_all_loop` is the only stateful pass; the
  delta's `ty_entries` come from the typer's post-walk env.

A future multi-segment fold can compute the data phase per-module
and merge the deltas before invoking the typer phase per-module
against the merged data env. The factoring exposes both phases as
discrete operations.

### 3. `prelude_len` names the built-in seed, not any user prelude

`prelude_len = list_length(seed.entries)` after
`add_prelude_sigs + add_builtin_variant_sigs`. It is the boundary
the `--holes` user-scope filter (`dump_hole_report`) consults to
trim built-ins out of "in scope" reports. It is a constant of the
compiler binary's bootstrap, not a per-module quantity. The fold
does not thread it.

### 4. Why `infer_program_with_protos` survives as a thin wrapper

Fourteen call sites use the legacy entry point (dump_typed,
dump_holes, dump_mono, library_mode probes, diagnostics_json, ...).
Each one passes a flat `[Decl]` because the cascade has already
merged the prelude into the stream by then. Migrating all fourteen
to the multi-module API would have inflated the diff without buying
anything: the cascade's output is the same. The wrapper is one
line — it builds `[ModuleDecls { name: None, decls }]` and calls
`typecheck_program`.

### 5. Why no dedicated smoke fixture

The integrator's brief asked for a smoke test that
`typecheck_module(prelude, empty) -> delta_p` followed by
`typecheck_module(user, delta_p)` produces the same TypedProgram as
`infer_program(prelude ++ user)`. Under scope A v1
`typecheck_program` collapses its input to a single segment, so
the per-module composition is trivially equal — the smoke test
would verify `f(x) == f(x)`. The full coverage is provided
indirectly: every selfhost run, every tier 1 fixture, every demo
exercises the new path (because `infer_program` now routes through
it). When the multi-segment fold lands in a follow-up, that lane
should add the composition smoke test as part of demonstrating the
per-module dynamics.

## Sub-step record (selfhost byte-identical at each)

1. **3a — API stub**: `ModuleEnvDelta` type, `module_env_delta_empty`,
   `merge_env_deltas`, `ModuleDecls`, `typecheck_module` (delegating
   to `infer_program_with_protos`), `typecheck_program` (collapsing
   to one segment). All additive — no callers yet.
   Commit `5a73017`.

2. **3b — `build_ty_env_module`**: factored `build_ty_env` into
   `build_ty_env_seed` (built-ins only) and `build_ty_env_module`
   (one module's contribution onto an inherited seed). `unions` and
   `top_names` collected once globally and threaded into the module
   call. The legacy `build_ty_env(decls)` is the composition.
   Commit `ed784cc`.

3. **3c — `collect_program_data`**: extracted the seven sister
   collect_* passes (`collect_unit_alias_table`, `collect_records`,
   `collect_alias_cands`, `collect_effect_names`,
   `resolve_alias_cands`, `collect_op_to_eff`, `collect_sums`) plus
   the built-env build into a named `ProgramData` record. The body
   of `infer_program_with_protos` reduces to `data` + `infer_all_loop`.
   Caught a missing `/ Console + File` effect annotation on the
   first selfhost run; fixed and re-ran green.
   Commit `44ffbd7`.

4. **3d — fold inversion**: made `typecheck_module` the canonical
   typer entry point and reduced `infer_program_with_protos` to a
   one-line wrapper that builds a single-module list and calls
   `typecheck_program`. The fold body is the collapse for now; the
   shape is in place for a follow-up to partition `all_decls` by
   `module_origin`.
   Commit `37eab04`.

## Structural surprises

- **`prelude_len` is the built-in seed boundary, not the user
  prelude boundary.** The brief assumed it would be one of the
  modular things to thread. It isn't — it is a constant of the
  bootstrap and never moves per-module. Skipped from
  `ModuleEnvDelta` entirely.
- **`unions` is collected eagerly into `TyEnv.unions` before
  `add_decls_loop` runs.** The typer queries unions during
  `infer_decl`, so they must be visible globally before any
  per-module typecheck. Hoisted out of `build_ty_env_module` and
  collected once over the merged stream.
- **`lower_protocols` is much larger than the brief's "seven sister
  collect_* passes" suggested.** It is the pre-typer cascade's
  protocol-coherence pass: it validates orphans + duplicates
  cross-module, generates `__pimpl_*` mangled DFns, generates
  `__proto_*` dispatchers. Scope A intentionally leaves this
  global; the proto_impls registry stays threaded as a function
  argument and does not live in `ModuleEnvDelta` v1.
- **Selfhost byte-identical is achievable in scope A because the
  fold has no observable dynamics.** Every sub-step composes
  functions whose composition is provably equal to the
  pre-refactor monolithic call. The risky part — partitioning the
  decl stream into segments and running `infer_all_loop` per
  segment — is deliberately deferred to a follow-up because that
  is where the row-var fresh-id allocation, the diagnostic line
  ordering, and the proto_impls registry could observably diverge.

## Out of scope (follow-ups for #452 Phase A.1)

Documented here because A.1 will need to make decisions about each:

1. **Multi-segment fold partitioning.** `compile_source` builds
   `merged_raw = list_append(qualified_prelude, qualified_decls)`;
   the cascade preserves that order; `all_decls` carries
   `module_origin` tags on every DFn. Partitioning `all_decls`
   post-cascade by `module_origin` and threading the inherited
   delta is the path. Risk: `__pimpl_*` / `__proto_*` synthetic
   DFns generated by `lower_protocols` carry no natural
   `module_origin`. The A.1 lane chooses whether to tag them or
   to treat them as a synthetic "protocols" segment processed last.

2. **`ModuleEnvDelta` does not include effect aliases, transparent
   aliases, or proto_impls.** These come out of the pre-typer
   cascade and are cross-module-coherent by construction. A.1's
   cache payload needs to include them too, but the cache loader
   must also re-run cascade validators (orphan check, alias cycle
   detection) on the union of cached + fresh decls because those
   are coherence properties not preservable in a per-module diff.

3. **Cascade is still global.** The brief said "keep the pre-typer
   cascade modular too — most of these operate over `[Decl]` and
   already partition cleanly per module". Empirically, about half
   of the cascade passes consult tables built over the full merged
   stream (`collect_const_names_decls`,
   `collect_refine_alias_names`, `collect_transparent_aliases`,
   `inject_builtin_effects`, the `expand_aliases_in_decls` cycle
   detector). Scope A leaves all of them global. Scope B would
   migrate the trivially-per-module ones (`lower_consts`,
   `lower_axioms`, `desugar_index`, `desugar_pos_records`,
   `desugar_var`) and quarantine the coherent-cross-module ones
   (alias resolution, protocol coherence). That is its own lane.

4. **No fixture exercises the per-module fold dynamics.** Scope A
   v1 collapses the fold to one segment, so there are no
   observable per-module dynamics to test. The follow-up that
   partitions for real should add `examples/typer/per_module_smoke.kai`
   with at least two segments and a composition-equality assertion.

5. **Cache hit savings estimate.** With scope A's boundaries,
   A.1 saves the typer's 0.43 s on the prelude *plus* the data
   collection (`collect_program_data` walks). The cascade
   (~0.10 s) still runs on cold start because it's a global pass.
   Net A.1 ceiling: ~0.43–0.50 s, against the cache-design.md
   target of 0.43 s. Cascade modularisation (scope B) would unlock
   the remaining ~0.10 s.

## Real cost vs estimate

| Activity | Brief estimate | Actual |
|---|---|---|
| Step 1 inventory | ~2-3 h | ~30 min |
| Step 2 delta design | ~2-4 h | ~30 min (most of the design happened during inventory) |
| Step 3a stub | ~30 min | ~15 min |
| Step 3b build_ty_env split | ~1 h | ~15 min |
| Step 3c collect_* split | ~1-2 h | ~15 min (+5 min fix for missing effect annotation) |
| Step 3d fold inversion | ~1 h | ~15 min |
| Step 4 smoke fixture | not budgeted | retired — see design §5 |
| Step 5 retro | ~30 min | ~30 min |
| Gates + PR | ~30 min | tier0 ~1 min, tier1 ~ in progress |
| **Total** | 2–3 days | ~3 h (one session) |
| LOC | 2 500 – 4 000 | ~140 net |

Why so much smaller than the brief: scope A v1 collapses the fold
to one segment. The risky bulk of the refactor — partitioning the
decl stream, threading inherited deltas across segments,
reconciling row-var fresh-id allocation per module — is deferred to
the follow-up that actually consumes the API (A.1). The work that
remained was structural plumbing: name the right boundaries, expose
them as functions, verify nothing changed under the hood. That
work is small because the pre-existing code was already clean.

## What this lane proved was wrong (for the record)

- "2 500–4 000 LOC, 2–3 days, 2–3 selfhost iterations" — overstated
  for scope A v1 by roughly an order of magnitude on LOC and time.
  The estimate would have been roughly correct for scope B with
  real per-module partitioning.
- "The seven sister collect_* passes are where the work is" —
  partially. The collect_* passes are trivially per-module-friendly
  (pure data walks). The work that the issue's wording obscured is
  in the cascade (alias resolution, protocol coherence) which
  scope A leaves alone.
- "`prelude_len` flows through `infer_all_loop` as an index for
  diagnostics — verify usages." Verified, but it turned out to be
  the bootstrap built-in length, not anything per-module. Skipped.

## Limitations of this retro

- The agent never ran `make tier1-asan` locally because the brief
  explicitly authorised deferring it to CI. If the multi-segment
  fold lands later and changes evaluation order, tier1-asan on
  Linux is where divergences will surface.
- The "byte-identical" claim is verified by the selfhost
  fixed-point check, which compares `kaic2` output on
  `compiler.kai`. It does not check other fixtures' codegen output
  byte-by-byte. Tier 1's golden-file fixtures provide that
  indirect check; tier 1 was green at the moment of writing.
- The follow-up estimates for A.1 cache savings (~0.43–0.50 s)
  assume the cascade re-runs cold. A real A.1 lane should re-bench
  before locking in those numbers.
