# Lane experience — issue #677 Phase 1m: extract the desugar passes into compiler/desugar.kai

**Lane branch:** `desugar-extract`
**Closes:** issue #677 Phase 1m — the second vertical-pipeline-layer extraction (after modules)
**Predecessors:** Phase 1i resolve (PR #685), 1j fmt (#688), 1k cache (#689), 1l modules (#690)

## Scope as planned vs as shipped

**Planned:** move the ~4150-LOC "desugar passes" block (lines
~9920–14070 of `stage2/main.kai`: A=positional record sugar, B=`var`
rewriter + const/axiom/refinement/contract lowering, C=`use Effect`
desugar, D=cap-binding nursery rewrite, E=brand-mismatch check,
F=string-interpolation lift) into `compiler/desugar.kai`, target
"5–15 pub over ~150–200 decls".

**Shipped:** moved **sub-passes A–E** (267 decls, 4384 LOC with
header) into `stage2/compiler/desugar.kai` with a **24-pub** surface
(18 fn + 6 result types). **Left sub-pass F (interp lift) in main.kai**
— it depends on the emit-aware interp scanner, so lifting it would
import emit upward into desugar. **Promoted `RecInfo` + its lookups (9
decls) to `compiler.ast`** as a shared AST-adjacent type. `main.kai`:
51604 → 47441 LOC (−4163). The cross-section analysis is in
`docs/lane-desugar-cross-section-analysis.md`; this retro is the
bitácora.

This is **not** the analysis-only outcome the driver got (PR #687).
A–E are genuine pipeline transforms the driver invokes downward — the
modules pattern, again. F is the one eslabón that doesn't fit, for a
reason the line-range brief could not see.

## Cross-section analysis (the load-bearing findings)

The brief's line range is a hypothesis, not the cut. Three findings
moved the boundary off the line numbers:

### 1. Sub-pass F is emit-coupled — it stays in main

F (`desugar_interp_decls` + ~25 walkers) lifts `"...#{e}..."` into
`string_concat_all([..., show(e), ...])`. It reads the interp
scanner's `IpLit(_, raw_for_c)` parts, where `raw_for_c` is the
C-escaped literal precomputed by `iscan_collect → raw_lit_for_c →
escape_str_body_for_c` — an emit-layer concern. The scanner is also
called by `emit_interp_parts` (7 call sites across resolve/perceus/
emit), so it is a transverse service, not a desugar sub-pass.
Extracting F would either import emit upward (inversion) or require
refactoring `IPart` to defer the escape to emit (a 5-layer, 7-call-
site change — its own lane). F stays in main with a one-line pointer
to the future scanner-layer lane. **The asu consult was explicit: F is
not "deferred to emit"; the pure scanner belongs to a future
scanner/string-lift layer, and F extracts cleanly only once that layer
exists.**

### 2. `RecInfo` is this lane's `ModuleEntry` — promoted to ast.kai

The positional-record sugar consumes `[RecInfo]` (the record table) in
16 pass signatures; the typer threads the same table through inference
(49 uses). It is a shared AST-adjacent type. The choices were: park it
in desugar.kai pub (modules did this with `ModuleEntry`), or promote it
to ast.kai. The asu consult gave the rule: **a type in a public
signature consumed by two layers (here desugar + the future infer
lane) goes to ast.kai — the only sink both import without a lateral
arrow.** Parking it in desugar.kai would force the infer lane to
`import compiler.desugar` sideways. Promoted `RecInfo` + the pure
lookups (`rec_find`, `rec_find_with_field`, `collect_records` + their
loops + `field_decl_list_has`) to ast.kai, 9 pub. The typer-only
`rec_find_arity` wrapper and `rerank_recs_*` / `option_str_eq` /
`module_slot_compat` stayed in main (they import `rec_find_arity_loop`
from ast). This is the follow-up the modules retro recommended,
executed in the lane that touched the layer.

Note `OpAr` would have been a second `RecInfo` (typer type in interp-
desugar signatures) — but it appears ONLY in F's signatures. With F
left in main, `OpAr` did not need to move at all. A satisfying
consequence: leaving F in main shrank the ast.kai blast radius too.

### 3. The FFI/proto/string cluster falls in the range by adjacency

`body_is_ffi_extern`, `ffi_extern_symbol_of`, the `proto_dispatch_*`
family, `string_starts_with_q` — all physically inside the line range
(sub-section B grouped lowering helpers next to them) but consumed by
emit / perceus / typer (later layers). Per the asu rule "a value
consumed by a later-extracted layer stays in main, out of range",
they were excluded from the cut. Moving them to desugar.kai would mint
spurious emit→desugar arrows.

## Methodological notes carried forward

- **The type-in-signature graph earned its keep again.** Six result
  types (`DPRDecls`, `PureExtract`, `UseRwProgram`, `NurseryRwProgram`,
  `BrandRec`, `BrandCall`) are returned by pub entry points but were
  not `pub` themselves. The call-only outbound scan does not see types;
  kaic2's own pub-enforcement flagged all six on the first modular
  compile. Budget the pub-enforcement round — the type graph predicts
  the count, the compiler confirms it.
- **Strip comments before the callee graph** (modules lesson) held —
  no false closure this time because the scan was comment-free from
  the start.

## Downward-glue: five local mirror families (`dsg_` prefix)

The passes call AST-generic + trivial helpers ubiquitous in main:

- `map_*` AST-functor family (`map_expr_kind` + 8 siblings, 25 callers)
- `pat_bindings` family (19 callers)
- `rename_*` binder-name walkers
- `string_starts_with_q` / `starts_with_loop` (7 callers)
- `ffi_extern_tag` / `ffi_extern_tag_prefix` (string-literal constants)

All mirrored with a `dsg_` prefix (the `mods_` precedent — avoids
duplicate C symbols in the flat bundle). The `ffi_extern_tag` mirror
is the producer/consumer contract: the axiom lowering (moved here)
writes the `__kai_ffi__` tag, the FFI-shim emitter (stayed in main)
reads it. Because the tag is a string literal, the mirror cannot
diverge.

The passes also reuse three helpers the modules lane already promoted
(`pat_binders`, `fn_param_names`, `validate_pub_access`) through
`import compiler.modules` — the correct downward direction. (`pat_
binders` from modules and the mirrored `dsg_pat_bindings` are distinct
functions and coexist without conflict.)

## Public surface (24 in desugar, +9 in ast)

desugar.kai: the 18 pass entry points the driver invokes
(`desugar_pos_records_decls`, `desugar_var_decls`,
`validate_var_uses_decls`, `desugar_use_decls`,
`desugar_const_refs_decls`, `collect_const_names_decls`,
`synthesize_refine_pred_fns`, `collect_refine_alias_names`,
`lower_pattern_narrow_decls`, `extract_pure_names_decls`,
`validate_contract_predicates_decls`, `rewrite_nursery_caps_decls`,
`check_nursery_brand_mismatch_decls`, `dump_brands`, `lower_axioms`,
`lower_consts`, `lower_axiom_one`, `lower_const_one`) plus the 6
result types they expose in signatures. Each carries a single-line
`#[doc]`. The range had zero `pub` markers in source (verbatim move),
so all 243 internals are private by construction.

ast.kai: the RecInfo cluster (9), `#[doc]` on the four documented
entry points (`RecInfo`, `rec_find`, `rec_find_with_field`,
`collect_records`).

## Fixtures / coverage

`stage2/tests/test_desugar.kai`: **10 unit tests + 3 property checks**.

- Unit: positional-record sugar (markers renamed away, no errors on
  arity match, named-field literal preserved, idempotent re-run);
  `lower_consts` (no DConst left, decl count preserved); `collect_
  const_names_decls` (after lowering); `desugar_var_decls` (SVar
  rewritten away); `collect_refine_alias_names` (empty on a
  refinement-free program).
- Property: positional-record sugar re-run introduces no errors
  (Int field-count generator); `lower_consts` preserves the decl count
  (Int const-count generator); `collect_const_names_decls` counts
  exactly the consts (Int generator).

**Two test-design facts the lane surfaced (the right pipeline order
is load-bearing for the tests):**

1. **Positional records use the brace form `P { 1, 2 }`, not `P(1,
   2)`.** The parser emits `ERecordLit("P", [FI(pos_marker, _), ...])`;
   the sugar renames the markers to the declared field names. `P(1, 2)`
   parses as a plain `ECall` and the sugar leaves it alone. My first
   draft used the paren form and the assertions failed — checked `kai
   info` / `examples/sugars/positional_record_basic.kai` and switched
   to braces.

2. **`collect_const_names_decls` reads DAttribPure(DFn), not DConst.**
   `lower_const_one` rewrites a `DConst` into `DAttribPure(DFn(...))`
   (a pure zero-arg thunk), and the name collector matches on that
   wrapped form. The collector therefore runs AFTER `lower_consts` in
   the driver — feeding it the raw parse finds nothing. My first draft
   ran the collector on the raw parse and got 0; fixed by lowering
   first. The test now encodes the pipeline order explicitly.

The internal walkers (dpr_* / use_rewrite_* / rewrite_nursery_* /
check_brand_* families) are covered transitively here and exhaustively
on every selfhost — the prelude rides positional records, const
lowering, and the nursery rewrite on every build.

## Acceptance gate

- `make selfhost` — **kaic2b.c == kaic2c.c** (the critical gate:
  desugar transforms the pre-typer AST; a byte-identical self-compile
  proves the extraction — including the `RecInfo` move and the five
  mirror families — is behaviour-preserving). Ran it twice: once after
  the cut, once after adding the 6 result-type `pub` markers.
- `kai test stage2/tests/test_desugar.kai` — 10/10.
- `kai check stage2/tests/test_desugar.kai` — 3/3.
- `make tier1` — green end-to-end (run locally before push).

## Real cost vs estimate

The brief estimated "~4150 LOC, 5–15 pub". The LOC held (4163 removed
from main); the pub surface landed at 24 + 9 — higher than the
estimate, but the honest cost of a pipeline layer whose result types
cross the cut and whose record table is shared with the typer. The
bulk of the lane was the cross-section scan and two asu consults (F's
emit-coupling, the RecInfo/OpAr/cluster placement rule), not the
mechanical move. No compiler-behaviour edits; the `RecInfo` move and
the mirrors are pure relocations. The two test-design facts (brace
form, lower-then-collect order) cost a diagnostic round each.

## Follow-ups for the remaining pipeline lanes (infer / protocols / emit_c / emit_llvm)

- **The scanner/string-lift layer is now a named gap.** Sub-pass F
  (`desugar_interp_decls`) + the interp scanner (`iscan_collect` /
  `IPart` / `IScan` / `raw_lit_for_c`) want a shared layer between
  desugar and emit. Refactoring `IPart` to defer the C-escape to emit
  would let F extract. That is the unblocking lane — flagged, not
  opened (no issue authorised).
- **`RecInfo` is now in ast.kai** — the infer lane consumes it from
  there, not from desugar. One fewer lateral arrow to untangle.
- **`OpAr` still lives in main** (protocols layer) — it will migrate
  with the protocols lane; the interp-desugar (F, in main) is its only
  current desugar-side consumer, and that consumer stays in main too.
- **The FFI/proto-dispatch/string cluster stays in main** until the
  emit lane — `body_is_ffi_extern` / `ffi_extern_symbol_of` /
  `proto_dispatch_*` / `string_starts_with_q` belong to emit (or a
  shared tag/util module). The `dsg_ffi_extern_tag` mirror in desugar
  is the producer side of that contract; when emit extracts, the two
  copies reconverge or the tag promotes to a shared constant.
- **The driver is still the apex and still last.** It imports modules,
  desugar, and (eventually) infer/protocols/emit — bodies, not
  back-imports.
