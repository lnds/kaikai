# Lane experience — issue #677 phase 1r: extract compiler/emit_c.kai (+ emit_shared sink)

The final pipeline-layer extraction before the driver lane. Planned as
"move the C emitter + TCO into compiler/emit_c.kai, and decide whether to
sink the helpers the LLVM lane (#699) duplicated as `lvm_*` mirrors."
Shipped as a **two-module** lane: a new `compiler/emit_shared.kai`
(backend-shared lowering layer) **plus** `compiler/emit_c.kai`, with the
137 `lvm_*` mirror copies in emit_llvm.kai deleted and rewired to import
the sink. selfhost byte-identical, LLVM selfhost byte-identical, modular
compile clean. The headline: the sink was the right call, the boundary
was 336 LOC tighter than the brief, and the sink's *own* downward glue
(3 helpers the brief didn't flag) was the one real surprise.

## Scope as planned

Move the `# emitter` block (brief range 772–12253, ~11500 LOC) into
`compiler/emit_c.kai`, TCO included. Decide sink-vs-skip for the 43
"genuinely shared" helpers the #699 retro named. Brief recommended
**sink** (option 1) if the list was cleanly identifiable.

## Scope as shipped

Two new modules, one rewired. The sink decision was **yes** — but the
data that drove it differed from the brief in two load-bearing ways.

### 1. The shared set is 133, not 43

The #699 retro said "43 genuinely shared." Empirically, **all 137 `lvm_*`
mirrors have a same-name original in main**, and 133 of those originals
sit in the emit_c range. The "43" was the subset called directly from
`emit_fn_body`/`emit_program`/dumps; the real transitive closure the LLVM
backend duplicated is 133. This inverted the brief's cost model: option 2
(skip) would have crystallised a **137-mirror** divergence debt, 3× the
retro's estimate, growing with every codegen fix. With 129 of the 133
shared helpers called *only* from inside the emit layer (4 had external
callers — `join_with`, `ty_to_string`, `main_row_labels`,
`effect_default_block_all_extern`), the shared set is a cohesive layer
(symbol minting, FFI marshalling, free-var analysis, alias rewriting,
protocol-dispatch decoding, the AST-functor glue), not a grab-bag. The
asu architecture consult (recorded in the lane) confirmed: this is the
codegen-common / lowering layer two backends share, the GIMPLE/SDAG
pattern — sink it.

Naming: asu argued for `codegen.kai` over `emit_shared.kai` to forestall
grab-bag drift. I kept `emit_shared.kai` (the brief + the #699 retro both
named it that three times; an internal module name is not
language-surface, and documentary continuity outweighed the marginal
cohesion-signal of a rename). The header states the membership rule
explicitly so the name doesn't invite drift.

### 2. The real boundary is 11917, not 12253 (the #699 trap, again)

Lines 11919–12253 (`ResolveState` / `rs_*` / `collect_pub_exports` /
`resolve_module` / `process_imports` / `expand_imports` / `canon_paths`)
are **module-resolution driver code**, not the emitter — exactly the
island the #699 retro flagged at the *upper* end of its own block. The
brief's 12253 would have pulled ~336 LOC of import machinery into
emit_c. The emitter (TCO included) ends at `tcrec_emit_rebinds_raw`
(11917); everything below is the driver lane's. A stray
`# ---- --dump-last-use ----` comment (the dumps left with infer in N1)
was cleaned from main's head.

### 3. The sink's own downward glue — the one real surprise

The brief and my pre-flight scan both checked what *emit_c* needs from
main's head/driver (5 helpers: `type_expr_display`, `unit_expr_display`,
`row_label_names`, `row_effects_without_default_handler`, `ty_name_zero`).
What neither anticipated: the **sink fns themselves** call three of those
head helpers (`ty_name_zero`, `row_label_names`, `unit_expr_display`).
The flat bundle compiled green; only the **modular** `kaic2 main.kai`
surfaced it as "private to module main" errors (the N1 "flat bundle hides
upward-refs" lesson, fourth lane running). Fix: mirror the three into
emit_shared with an `es_` prefix (their originals stay in main's
head/driver for the driver lane to reconcile). This is the phase-1m
mirror-downward discipline applied to a *second* module in the same lane.

emit_c's own glue (`type_expr_display`, `unit_expr_display`,
`row_effects_without_default_handler` — the 3 it uses, once each) is
mirrored with a `cmt_` prefix, same discipline.

## Design decisions / alternatives considered

- **Sink (option 1) vs skip (option 2).** Sink. The 133-not-43 data made
  the divergence debt of skipping too large to defer, and the 129/133
  emit-only ratio made the boundary clean. Documented per integrator
  request.
- **`join_with` → compiler.util, not emit_shared.** It is a one-line
  `string_join` wrapper used by head + emit_c + driver — generic, not
  emit-specific. Moving it to util (next to `cat2`–`cat5`) keeps it out
  of the emit layer and resolves its head/driver callers downward. It was
  in the lvm-mirror set (`lvm_join_with` deleted, callsites point at the
  util version).
- **`BuildMode` → emit_c (pub), not mirrored.** `emit_program` matches on
  it; the driver constructs it. It travels with its primary consumer (the
  emitter) rather than staying in the driver region with a forward
  reference. The driver imports it from emit_c.
- **4 residual `lvm_*` kept in emit_llvm.** `row_effects_without_default_handler`,
  `row_label_names`, `ty_name_zero`, `unit_expr_display` — their
  originals live outside the emit_c range (main head/driver), so they are
  not in emit_shared. emit_llvm keeps these 4 mirrors until the driver
  lane. Mirror debt cut 137 → 4 (97%).
- **`pub` enforcement round.** The modular compile flagged two emit_c
  types (`AmbigEntry`, `TAEntry`) exposed by pub fns but left private —
  `pub`'d them (the phase-1m six-type surprise, smaller here).

## Structural surprises

- The emit-layer coupling is *intra-emit*, confirming #699: emit_c and
  emit_llvm share a plumbing layer wholesale; the vertical pipeline lanes
  (infer/monomorph/unbox/perceus) were clean because they sit *above*
  emit and consume typed `[Decl]`. Emit-layer extraction is a different
  shape — and now, with the sink, it is finally factored that way.
- The sink graph is a clean DAG: 0 back-edges sink → emit_c once
  `find_substring` joined the sink (it was the single helper a sink fn
  needed that otherwise stayed in emit_c) and `join_with` left for util.

## Fixtures / coverage

- `stage2/tests/test_emit_shared.kai`: 15 unit tests + 2 property checks.
  `c_sym` (bare / module-qualified), `evar_find` / `evar_find_tag` /
  `evar_find_tag_opt` (present / absent), `ffi_ret_kind_of` (None=Unit,
  Int), `find_substring` / `substring_eq` (start / mid / absent
  length-sentinel), `EV` field round-trip. Property checks (Int
  generators): `c_sym` preserves bare-name length, `evar_find_tag` on
  `[]` is the -1 sentinel.
- `stage2/tests/test_emit_c.kai`: 8 unit tests + 2 property checks.
  `emit_program` end-to-end (non-empty C, runtime.h include, `int main`
  scaffold), `tcrec_rewrite_decls` decl-count preservation (non-recursive
  + self-recursive), `BuildMode` variant distinguishability. Property
  checks: `tcrec_rewrite_decls` keeps a one-decl module at one decl,
  `emit_program` non-empty for any single-constant module.
- **Repaired `stage2/tests/test_emit_llvm.kai`** (debt this lane created):
  deleting `lvm_find_substring` orphaned the #699 test. Rewired it to
  `find_substring` (now imported from emit_shared) — the two were
  byte-identical (same `string_length` absent-sentinel), so the test
  semantics are preserved.

Not tier-wired (consistent with every `tests/test_*.kai`; the per-module
runner is #452/#677 Phase 2). All three compile clean under the modular
`kaic2`, and the emit layer is exercised on every selfhost.

## Cost vs estimate

Estimate: ~11500 LOC moved, sink ~30–60 min extra, 10–25 pub. Actual:
main 16373 → 5203 LOC; emit_shared.kai ~2865 LOC (133+3 fns, 13 types,
146+3 pub/doc); emit_c.kai ~10000 LOC (17 pub fns + `BuildMode`, 3 `cmt_`
glue); emit_llvm.kai 7332 → 5031 LOC (137 mirrors + 13 types deleted, 4
residual). The sink was *not* 30–60 min extra — it was the lane. The
relocation was mechanical; the cost was the closure-correctness iteration
(43→133), the es_-glue surprise the modular compile exposed, and the
test_emit_llvm repair.

## Follow-ups for the driver lane (the last lane of #677 phase 1)

- **main.kai is now 5203 LOC**: module-resolution (`ResolveState` /
  `resolve_module` / `expand_imports`, ~11940→12253-era code, now
  ~786+), the dumps that read it, and the `# cli + driver` block. That is
  the driver lane's territory.
- **Reconcile the glue mirrors.** `type_expr_display`, `unit_expr_display`,
  `row_label_names`, `row_effects_without_default_handler`, `ty_name_zero`
  have originals in main's head + mirrors in emit_c (`cmt_`), emit_shared
  (`es_`), and emit_llvm (`lvm_`, 4 of them). When the driver extracts,
  these display/construction helpers are candidates to sink to a shared
  module (util or a small `type_display`), collapsing all copies.
- **4 residual `lvm_*` in emit_llvm** go away once those 4 originals move
  out of main's head/driver into a shared module the LLVM backend can
  import.
- **`BuildMode` lives in emit_c now**, not the driver region — the driver
  imports it from emit_c.
- Token-renamer hygiene held: the prefix renames (`lvm_`→bare, `Lvm`→
  original ctors, `es_`/`cmt_` glue) all skipped string literals; the
  post-rename `grep '"…token…"'` audit found no corruptions.
