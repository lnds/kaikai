# Module-system cross-section analysis — issue #677 phase 1l

Status: **extracted** (not analysis-only). Unlike the driver (PR #687),
the module system *is* a genuine pipeline layer that the driver invokes
downward, so it extracts — but only after splitting one contaminated
eslabón (the import-load orchestrator) back out and leaving it in
`main.kai`. This doc records the dependency data the cut rests on, the
verdict, and the surprises the line-range brief did not anticipate. The
companion retro is `docs/lane-experience-issue-677-phase1l.md`.

## TL;DR

- The brief scoped the cut as lines ~43000–45885 of `stage2/main.kai`
  (5 sub-sections: A=module resolution / import loading, B=qualified
  call rewrite, C=qualified type rewriter, D=import-hole symbol-index,
  E=`pub` enforcement). 192 top-level decls.
- The line range is a *starting hypothesis*, not the cut (the same
  lesson as phase 1i resolve). The callee graph decides.
- **The import-load orchestrator (`expand_imports` / `process_imports` /
  `resolve_module` + the `ResolveState` it threads, 20 decls) calls
  UPWARD** into the typer (`validate_module_same_module_collisions` →
  5 `validate_*_collisions_decls`), the lowering layer
  (`tag_decls_module_origin` → `lower_axiom_one` / `lower_const_one`)
  and lexer reporting (`report_errors`). That is pipeline orchestration
  — "the driver disguised" — and it **stays in `main.kai`**.
- **Everything the orchestrator computes over** (file resolution, the
  `ModuleEntry` module-table type, the qualified rewriters, the
  import-hole index, `pub` enforcement) sits BELOW the driver and moved
  to `compiler/modules.kai` (175 decls, 24 `pub`).
- Net: `main.kai` 54156 → 51602 LOC (−2554). `modules.kai` 2670 LOC.

## Method: comment-free callee graph

The first closure pass (counting `\bsym\b` anywhere in a body) pulled
**111 of 192** decls into the orchestrator's transitive closure — far
too many. The culprit was a single *comment* mention: a doc-comment in
`process_imports` reads "…so `validate_pub_access` can split the flat
stream…", and the naïve regex treated that as a call edge, dragging all
of E (and through E, all of C) into the orchestrator.

Re-running the closure on **comment-stripped** bodies (cut each line at
the first `#` outside a `#{…}` interpolation) shrank the orchestrator to
**20 decls**, all in sub-section A. Lesson for future pipeline lanes:
**strip comments before computing the callee graph**, or doc-comments
that name downstream passes will inflate the "must stay together" set.

## The partition

### STAYS in `main.kai` — the import-load orchestrator (20 decls)

`resolve_module`, `process_imports`, `expand_imports`, `ResolveState` +
the 8 `rs_*` state helpers, `ExpandedProgram`, `canon_paths`,
`non_import_decls`, `collect_pub_exports`, `collect_unstable_exports`,
`collect_variant_names`, `collect_pub_names_for_unstable`.

Why these stay: `resolve_module` is the I/O driver of module loading. It
reads files, tokenizes, parses, validates collisions (typer), tags
module origin (lowering), and reports lex errors — three calls UPWARD
into layers above the module system. It is `go/build` + the loader, not
`go/types`. It belongs next to `compile_source`.

### MOVES to `compiler/modules.kai` (175 decls, 24 `pub`)

- **File resolution:** `find_module_file`, `module_to_path`,
  `dots_to_slashes(_loop)`, `try_candidate(s_loop)`, `CandidateFile`,
  `dir_of`, `last_slash`, `format_cycle_chain`, `join_with_arrow`.
  Pure path mechanics + on-disk lookup (`File` effect), zero upward
  calls. The orchestrator (in main) calls `find_module_file`; the
  import-hole pass calls it too.
- **`ModuleEntry`** (module-table entry, ctor `ME`) + accessors
  (`me_has_export`, `me_lookup_export`, `mt_lookup`,
  `me_export_is_unstable`, `name_starts_upper`).
- **B — qualified-call rewrite** (m6.2 Phase 2.3): `rqc_decls` /
  `rqc_expr` + the `rqc_*` walkers; `collect_module_origins`.
- **C — qualified-type rewrite** (issue #232): `qualtype_decls` /
  `qtchk_decls` + the `qualtype_*` / `qtchk_*` walkers;
  `unstable_check_decls` + walkers.
- **D — import-hole resolver** (m7f §7): `report_import_holes`,
  `strip_import_holes`, the `import_hole_*` helpers.
- **E — `pub` enforcement** (issue #510): `validate_pub_access`, the
  `vpa_*` walkers, the `pae_*` access-table helpers, the `HomeAnchor`
  home-hint machinery, `PreludeSegment`.

## Dependency-arrow verification (the load-bearing data)

With the 20-decl orchestrator pinned to main and the other 172 to
modules, the boundary edges are:

- **UPWARD (extractable → orchestrator): NONE.** No symbol moving to
  modules.kai calls back into the orchestrator cluster. The arrow never
  inverts. (Verified by comment-free callee graph over all 172.)
- **DOWNWARD (orchestrator → extractable): 3 symbols.** `main.kai`
  needs `ModuleEntry`, `dir_of`, `find_module_file` from modules — all
  consumed through `import compiler.modules`, the correct direction.

Plus the driver invokes the 5 pass entry points downward from
`compile_source` (main.kai): `qualtype_decls`, `rqc_decls`,
`report_import_holes`, `strip_import_holes`, `validate_pub_access`,
`unstable_check_decls`. main → modules throughout.

## Surprises the line-range brief did not anticipate

1. **Two AST-generic helpers ride along.** `pat_binders` (pattern
   binder names) and `fn_param_names` (parameter names) live in the
   range because sub-section B uses them, but the typer/desugar in
   main.kai call them 6× each. They are NOT module-system concepts.
   They moved (the rewriters need them) and are `pub`, but a future
   lane should promote them to `compiler.ast` or `compiler.util`. Same
   shape as the `collect_type_names` outsider in the resolve lane (phase
   1i), except here they move *with* the cut rather than staying behind,
   because the bulk of their callers are the rewriters.

2. **`ModuleEntry` is a shared type.** 39 + 10 (ctor `ME`) external
   uses in the typer (@14091–14302) and emitter (@24378). It is `pub`
   in modules.kai and resolves through the import. The language
   architect flagged it (and `PreludeSegment`) as candidates to promote
   into `compiler.ast` — a "shared AST-adjacent type" rather than a
   "module-system internal". Deferred to keep this lane's blast radius
   to a single new file.

3. **`PreludeSegment` was an invisible upward dep.** It is defined at
   main.kai:48192 (driver / prelude-loading layer, `type PreludeSegment
   = PSeg(String, [Decl])`) but consumed in the *signatures* of E's
   `validate_pub_access` / `collect_pub_access_table` / `cpa_segs`. The
   outbound scan missed it because it scanned for **call** edges
   (`ident(`), and `PreludeSegment` appears only as a **type in
   signatures**, never called. It surfaced as a `pub`-access error on
   the first selfhost. Fix: moved `PreludeSegment` into modules.kai
   (issue #510 territory — it is the per-prelude-file split the
   pub-access validator consumes), `pub`, with the driver constructing
   `PSeg` values and threading them downward. **Lesson:** the
   cross-section scan must track types-in-signatures, not just call
   edges, or a shared type defined outside the range slips through.

4. **Two trivial helpers became local mirrors.** `join_with` (one-liner
   `string_join`) and `diag_warning` (warning + source caret) are
   ubiquitous in main.kai (60+ and a handful of callers respectively),
   so they cannot move. The rewriters call them too. Per the
   downward-glue rule (do not `pub`-export upward), they were mirrored
   locally as `mods_join_with` / `mods_diag_warning` — the `mods_`
   prefix avoids a duplicate `kai_join_with` / `kai_diag_warning` C
   symbol in the flat bundle. Both mirrors depend only on already-
   imported symbols (`emit_source_caret` from diag, `string_join` /
   `eprint` / `int_to_string` from the prelude), so they carry no
   further coupling.

## Why this is NOT the driver

The driver (PR #687) was analysis-only because it is the pipeline
*apex*: it calls ~143 head symbols (the whole pipeline beneath it), so
extracting it would force ~140 `pub` exports and invert the arrow. The
module system is the opposite: it is a pipeline *layer* the driver sits
on top of. Four of its five sub-sections (B/C/D/E) are AST
transformations the driver invokes downward — identical to the resolve
pattern. Only sub-section A's orchestrator has the apex shape, and that
eslabón stays. 24 `pub` is higher than cache (4) or resolve (9), but
that is the honest cost of a genuine pipeline layer consumed by the
driver, typer, and emitter — not a dependency inversion.

## Verification commands

```sh
# section boundaries
grep -nE "^# (module resolution|m6\.2 Phase 2\.3|Issue #232|m7f §7|Issue #510|m12\.8)" stage2/main.kai

# orchestrator stays: these 5 must still be in main.kai
grep -nE "^fn (resolve_module|process_imports|expand_imports)|^type (ResolveState|ExpandedProgram)" stage2/main.kai

# the 3 downward symbols main imports from modules
grep -nE "^pub (fn|type) (ModuleEntry|dir_of|find_module_file)" stage2/compiler/modules.kai

# no extractable symbol calls back into the orchestrator (UPWARD = none)
# (the canary: resolve_module / process_imports must NOT appear in modules.kai)
grep -c "resolve_module\|process_imports" stage2/compiler/modules.kai   # comments only

# the critical gate
cd stage2 && make selfhost   # kaic2b.c == kaic2c.c
```
