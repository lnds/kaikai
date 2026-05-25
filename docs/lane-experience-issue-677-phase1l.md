# Lane experience — issue #677 Phase 1l: extract the module system into compiler/modules.kai

**Lane branch:** `modules-extract`
**Closes:** issue #677 Phase 1l — first of the "vertical pipeline" extractions
**Predecessors:** Phase 1i resolve (PR #685), 1j fmt (PR #688), 1k cache (PR #689)

## Scope as planned vs as shipped

**Planned:** move the ~2884-LOC "module system" block (lines ~43000–45885
of `stage2/main.kai`: A=module resolution, B=qualified call rewrite,
C=qualified type rewriter, D=import-hole resolver, E=`pub` enforcement)
into `compiler/modules.kai`, expecting "~80–100 decls, 5–15 `pub`".

**Shipped:** moved **172 of 192** decls (1727 LOC of source, 2670 LOC
with header) into `stage2/compiler/modules.kai` with a **24-`pub`**
surface. **Left the 20-decl import-load orchestrator in `main.kai`** —
it calls upward into the typer/lowering/lexer, so it is "the driver
disguised" and belongs next to `compile_source`. The cross-section
analysis is in `docs/lane-modules-cross-section-analysis.md`; this retro
is the bitácora. `main.kai`: 54156 → 51602 LOC (−2554).

This is **not** the analysis-only outcome the driver got (PR #687). The
module system is a genuine pipeline *layer* the driver invokes downward,
so it extracts — it just needed the orchestrator carved back out.

## Cross-section analysis (the load-bearing finding)

The brief's line range is a hypothesis, not the cut (the phase-1i
lesson, again). The callee graph decided the boundary:

- **The import-load orchestrator** (`expand_imports` / `process_imports`
  / `resolve_module` + the `ResolveState` it threads, 20 decls) calls
  UPWARD into three layers above the module system:
  `validate_module_same_module_collisions` (typer, → 5 collision
  validators), `tag_decls_module_origin` (lowering, →
  `lower_axiom_one` / `lower_const_one`), and `report_errors` (lexer
  reporting). Those are orchestration concerns. **Stays in main.kai.**
- **Everything else** — file resolution, the `ModuleEntry` table type,
  the qualified rewriters (B/C), the import-hole index (D), `pub`
  enforcement (E) — sits below the driver and moved. With the
  orchestrator pinned to main, **there are zero upward edges** from the
  extracted set, and main needs exactly 3 symbols downward
  (`ModuleEntry`, `dir_of`, `find_module_file`) plus the 5 pass entry
  points the driver invokes.

### Methodological surprise: strip comments before the callee graph

The first transitive-closure pass pulled **111 of 192** decls into the
orchestrator's "must stay together" set. The culprit was a single
*doc-comment* in `process_imports` that names `validate_pub_access`; the
naïve `\bsym\b` regex counted it as a call edge, dragging all of E (and
through E, all of C via `vpa_kind → rqc_decls`) into the closure.
Stripping comments (cut each line at the first `#` outside a `#{…}`
interpolation) before building the graph shrank the orchestrator to its
true 20 decls. **Future pipeline lanes must strip comments first**, or
prose that references downstream passes inflates the cut.

## The two surprises the scan missed (caught by selfhost)

The outbound scan looked for **call** edges (`ident(`). Two upward
dependencies hide as **types in signatures**, never called — invisible
to that scan, surfaced only when the self-hosted kaic2 ran `pub`
enforcement on itself:

1. **`PreludeSegment`** (`type PreludeSegment = PSeg(String, [Decl])`,
   main.kai:48192, driver/prelude-loading layer) appears in the
   *signatures* of E's `validate_pub_access` / `collect_pub_access_table`
   / `cpa_segs`. It is issue-#510 territory (the per-prelude-file split
   the pub-access validator consumes), so it moved into modules.kai,
   `pub`, with the driver constructing `PSeg` values and threading them
   downward.

2. **`find_module_file`** + **`CandidateFile`** — `resolve_module` (which
   stays in main) calls `find_module_file` and pattern-matches `CF(…)`.
   Both had to become `pub` in modules.kai. My initial `pub` set listed
   `dir_of` / `last_slash` but omitted `find_module_file`, so the first
   selfhost failed on it. (The mechanical lesson: enumerate the
   orchestrator's downward calls explicitly, don't rely on the
   inbound-from-head scan alone — `resolve_module` is the one caller and
   it stays in main, so the call shows as "main → modules", which the
   range-internal scan does not flag.)

**Takeaway for the desugar / infer / protocols / emit lanes:** the
cross-section scan must track **types used in signatures**, not just
call edges. A shared type defined outside the range (or a callee in the
stay-cluster) slips through a call-only scan and only surfaces at
selfhost, where kaic2's own `pub` enforcement is the real gate.

## Downward-glue: two local mirrors

`join_with` (`string_join` one-liner) and `diag_warning` (warning +
source caret) live in main.kai and are used ubiquitously there (60+ and
a few callers). The rewriters in modules.kai call them too. Per the
downward-only rule, I did not `pub`-export them upward; I mirrored them
locally as **`mods_join_with` / `mods_diag_warning`** — the `mods_`
prefix is mandatory because the flat bundle would otherwise mint a
duplicate `kai_join_with` / `kai_diag_warning` C symbol against main's
copies (the redefinition error the first build hit). Both mirrors depend
only on already-imported symbols (`emit_source_caret` from diag,
`string_join` / `eprint` / `int_to_string` from the prelude), so they
carry no further coupling. This matches the
`module_extraction_downward_glue` precedent.

## Public surface (24, target was 5–15)

Higher than cache (4) or resolve (9) — the honest cost of a pipeline
layer consumed by the driver, typer, AND emitter. Breakdown:

- **Orchestrator needs (downward from main):** `ModuleEntry` (+ ctor
  `ME`), `dir_of`, `find_module_file`, `CandidateFile`, `last_slash`.
- **Pass entry points the driver invokes:** `qualtype_decls`,
  `qtchk_decls`, `rqc_decls`, `unstable_check_decls`,
  `report_import_holes`, `strip_import_holes`, `validate_pub_access`.
- **Home/access helpers the typer + emitter consume:**
  `collect_module_origins`, `decl_home_hint_reset`, `HomeAnchor`,
  `ha_home`, `impl_first_fn_home`, `imports_home_anchors`, `pae_take`,
  `pae_drop`, `PreludeSegment`.
- **AST-generic helpers (misplaced, see below):** `pat_binders`,
  `fn_param_names`, `rqc_expr`.

Each carries a single-line context in the header's "Public surface"
section. The un-pub audit was clean: the range had zero `pub` markers in
source (verbatim move), so all 151 internals are private by
construction; only the 24 entry symbols got `pub` added.

## Structural surprises

1. **Two AST-generic helpers ride along.** `pat_binders` and
   `fn_param_names` are not module-system concepts — the typer/desugar
   call them 6× each from outside the range. They moved (the rewriters
   need them) and are `pub`, but a future lane should promote them to
   `compiler.ast` / `compiler.util`. Same shape as resolve's
   `collect_type_names` outsider, except these move *with* the cut
   because the rewriters are the bulk of their callers.

2. **`ModuleEntry` is a shared type** (39 + 10 external uses in typer +
   emitter). The language architect (asu consult) flagged it and
   `PreludeSegment` as candidates to promote into `compiler.ast` — a
   "shared AST-adjacent type" rather than a "module-system internal".
   Deferred to keep this lane's blast radius to one new file.

3. **The orchestrator stays — and that is correct, not a compromise.**
   The asu consult was explicit: `resolve_module` is `go/build` + the
   loader; the four rewriter passes are `go/types`. The loader calls the
   checker, never the reverse. Leaving the loader in main keeps the
   arrow pointing the right way.

## Fixtures / coverage

`stage2/tests/test_modules.kai`: **13 unit tests + 3 property checks**.

- Unit: `dir_of` / `last_slash` (path mechanics, fixed inputs),
  `collect_module_origins` (table → origins), `fn_param_names` (off a
  parsed DFn), and the qualified-call rewriter `rqc_decls` driven
  end-to-end through the real parser (`mod.fn(x)` → `EModCall` when the
  synthetic table exports `fn`; stays `EField` otherwise / unknown
  module / plain local call).
- Property: rewrite-to-EModCall verdict equals the export verdict (Bool
  generator toggling the table); `collect_module_origins` preserves
  entry count (Int generator); `last_slash` of a slash-free string is
  -1 (Int generator).

**Coverage-discipline note:** the property checks deliberately feed
generated values into PURE helpers only, never interpolated into source
text. An early draft generated `String` and built `"m.#{fname}(1)"`;
this crashed the `kai check` runtime (Trace/BPT trap) on `string_slice` /
`char_at` over generated multibyte Strings inside `dir_of` — a runtime
fragility in `string_slice` with UTF-8-shaped generated input, NOT in
the extracted code (verbatim move; the existing `test_diag` String
checks pass because `levenshtein` tolerates the same input). Rewrote the
checks to use `Bool` / `Int` generators and slash-free ASCII so they
exercise the helpers without tripping the runtime. Flagged here rather
than chased — it is outside the lane and outside `modules.kai`.

The internal walkers (qualtype_* / qtchk_* / vpa_* / pae_* families) are
covered transitively here and exhaustively on every selfhost — the
prelude itself rides the qualified rewrite + pub enforcement on every
build.

## Acceptance gate

- `make kaic2` — compiles clean.
- `make selfhost` — **kaic2b.c == kaic2c.c** (the critical gate: modules
  touches the prelude-load + qualified-rewrite + pub-enforce pipeline;
  byte-identical self-compile proves the extraction is behaviour-
  preserving).
- `kai test stage2/tests/test_modules.kai` — 13/13.
- `kai check stage2/tests/test_modules.kai` — 3/3.
- `make tier1` — green end-to-end (run locally before push).

## Real cost vs estimate

The brief estimated "more cohesive than driver — expect it extracts".
Held, but with more verification than the cache lane: the comment-strip
discovery (111 → 20 false closure), the two signature-only upward deps
caught at selfhost, and the downward-glue mirrors were each a round of
diagnosis the verbatim cache move did not need. No compiler edits, no
AST changes. The bulk of the lane was the cross-section scan and the
selfhost-driven discovery of the two invisible type dependencies.

## Follow-ups for the remaining pipeline lanes (desugar / infer / protocols / emit_c / emit_llvm)

- **Strip comments before the callee graph.** A doc-comment naming a
  downstream pass will inflate the "must stay together" closure 5×.
- **Track types-in-signatures, not just call edges.** `PreludeSegment`
  and `find_module_file`/`CandidateFile` were invisible to a call-only
  scan and surfaced only at selfhost. Build the type-dependency graph
  too, or budget for a selfhost round to find them.
- **The orchestrator pattern recurs.** Any layer with an I/O or
  driver-facing entry that calls upward (validation, lowering,
  reporting) should leave that entry in main and extract the pure
  transformation beneath it. Don't extract the loader; extract what the
  loader computes over.
- **Promote `ModuleEntry`, `PreludeSegment`, `pat_binders`,
  `fn_param_names` to `compiler.ast` / `compiler.util`** in whichever
  lane touches those layers — they are shared types/helpers that landed
  in modules.kai for expedience, not domain fit. Doing so would shrink
  modules.kai's pub surface from 24.
- **The driver is still the apex and still last.** Unchanged from the
  cache retro: extract the remaining pipeline stages (desugar, infer,
  protocols, emit) before the driver, which imports modules — including
  this one — rather than bodies.
