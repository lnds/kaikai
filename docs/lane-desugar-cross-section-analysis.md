# Desugar cross-section analysis — issue #677 phase 1m

Status: **extracted** (not analysis-only). The six pre-typer desugar
sub-passes are a genuine pipeline layer the driver (`compile_source`)
invokes downward, so they extract — but only after (a) leaving one
sub-pass in main because it depends on the emit layer, (b) promoting a
shared record-table type to `compiler.ast`, and (c) mirroring a
handful of ubiquitous main helpers locally. This doc records the
dependency data the cut rests on, the verdict, and the surprises the
line-range brief did not anticipate. The companion retro is
`docs/lane-experience-issue-677-phase1m.md`.

## TL;DR

- The brief scoped the cut as lines ~9920–14070 of `stage2/main.kai`
  (6 sub-sections: A=positional record sugar, B=`var` rewriter + const/
  axiom/refinement/contract lowering, C=`use Effect` desugar, D=cap-
  binding nursery rewrite, E=brand-mismatch check, F=string-interp
  lift). ~300 top-level decls.
- The line range is a *starting hypothesis*, not the cut (the same
  lesson as resolve / modules). The callee graph + the type-in-
  signature graph + the emit-coupling of one sub-pass decided the
  boundary.
- **Sub-pass F (string-interpolation lift) STAYS in main.kai.** It
  consumes `IpLit(_, raw_for_c)` parts whose escaped-for-C field is
  produced only by the emit-aware interp scanner (`iscan_collect` →
  `raw_lit_for_c` → `escape_str_body_for_c`). Lifting F would drag the
  emit layer into desugar; the scanner is also used by `emit_interp_
  parts`, so it is a transverse service, not a desugar sub-pass.
- **The record-table type `RecInfo` (+ its pure lookups) MOVED to
  `compiler.ast`.** It appears in 16 positional-record-sugar signatures
  AND is threaded through the typer (49 uses). ast.kai is the only home
  both can import without a lateral arrow — the shared-AST-adjacent-
  types rule the modules retro recommended for `ModuleEntry`.
- **A cluster of FFI/proto-dispatch/string helpers STAYS in main**
  (out of the extraction range) — they fall in the line range by
  adjacency but their consumers are emit / perceus / typer.
- Net: `main.kai` 51604 → 47441 LOC (−4163). `desugar.kai` 4384 LOC.
  Public surface: desugar.kai 24 (18 fn + 6 type); ast.kai +9
  (RecInfo cluster).

## Method: comment-free callee graph + type-in-signature graph

Per the modules-lane lesson, the closure was computed over
**comment-stripped** bodies (cut each line at the first `#` outside a
`#{…}` interpolation). The scan ran in two directions:

- **Outbound** (range → outside): which symbols defined outside the
  range does the range reference? These must be imported, mirrored, or
  the range shrinks.
- **Inbound** (outside → range): which range symbols are referenced
  from outside? These must become `pub`.

And — critically — the type-in-signature graph was built separately
from the call graph, because the modules lane learned that a shared
type in a public signature is invisible to a call-only scan.

## The partition

### STAYS in `main.kai`

1. **Sub-pass F (string-interp lift):** `desugar_interp_decls` and the
   ~25 `desugar_interp_*` walkers + `lift_interp_string` /
   `interp_part_to_expr`. F is genuinely a desugar pass, but it sits on
   top of the emit-aware interp scanner: `interp_part_to_expr` reads
   `IpLit(_, raw_quoted)` where `raw_quoted` is the C-escaped literal
   the scanner precomputes via `escape_str_body_for_c` (emit layer).
   Extracting F would require either importing emit upward into desugar
   (inversion) or refactoring the `IPart` type to defer the escape to
   emit (a 7-call-site, 5-layer change — its own lane). F extracts
   cleanly once a shared scanner/string-lift layer exists; until then
   it stays beside the scanner.

2. **The FFI/proto-dispatch/string cluster** (in the line range by
   adjacency, not by domain): `body_is_ffi_extern`,
   `ffi_extern_symbol_of`, `ffi_extern_tag(_prefix)`,
   `body_is_proto_dispatch`, `proto_dispatch_marker_in`,
   `proto_dispatch_tag(_prefix)`, `mk_proto_dispatch_body`,
   `proto_dispatch_parts`, `ProtoDispatchParts`, `find_colon`,
   `string_starts_with_q`, `starts_with_loop`. Their consumers are
   `emit_fn_body` / `llvm_emit_fn` / `perceus_decl` / `classify_unbox_
   sig` / `synth_todo` — all later pipeline layers. Moving them to
   desugar.kai would mint spurious emit→desugar arrows. They stay in
   main and will migrate to emit (or a shared tag/util module) with
   that lane. The axiom lowering (which moved) produces the FFI tag;
   the FFI-shim emitter (which stayed) consumes it — see the local
   `dsg_ffi_extern_tag` mirror below.

3. **The typer-side record helpers** `rec_find_arity` (the arity-only
   wrapper, used only by the typer at the inference call site) and
   `rerank_recs_*` / `option_str_eq` / `module_slot_compat` (per-module
   rerank + Option comparison, used by unify). These stay; they import
   `rec_find_arity_loop` from ast.

### MOVES to `compiler.ast` (the shared record table, 9 pub)

`RecInfo` (ctor `RI`) + the pure lookups `rec_find` / `rec_find_loop` /
`rec_find_arity_loop` / `rec_find_with_field` / `rec_find_with_field_
loop` / `field_decl_list_has` / `collect_records` / `collect_records_
loop`. The positional-record sugar consumes `[RecInfo]` in 16 pass
signatures; the typer threads the same table through inference (49
uses). Leaving it in desugar.kai would force the future infer lane to
`import compiler.desugar` laterally. ast.kai is the only sink both
import cleanly — the shared-AST-adjacent-types rule (asu consult).

### MOVES to `compiler.desugar` (sub-passes A–E, 267 decls, 24 pub)

- **A. Positional-record sugar** (issue #266): `desugar_pos_records_
  decls` + the `dpr_*` walkers, `rename_pos_fields`, `dpr_record_lit`
  / `dpr_record_spread` + the spread machinery.
- **B. The `var` rewriter + lowering helpers**: `desugar_var_decls` /
  `validate_var_uses_decls` + the `validate_var_*` / `desugar_var_*` /
  `*_escapes` / `*_contains` families; `lower_consts` /
  `lower_const_one`, `lower_axioms` / `lower_axiom_one`;
  `desugar_const_refs_decls` + the `desugar_const_refs_*` / `rcr_*`
  span-rewriters; `synthesize_refine_pred_fns` / `build_refine_pred_fn`,
  `collect_refine_alias_names`, `lower_pattern_narrow_decls`;
  `extract_pure_names_decls`, `validate_contract_predicates_decls` +
  the `validate_contract_*` family.
- **C. `use Effect` desugar** (m7e §25): `desugar_use_decls` + the
  `use_*` / `use_rewrite_*` families.
- **D. Cap-binding nursery rewrite** (m7b #4): `rewrite_nursery_caps_
  decls` + the `rewrite_nursery_caps_*` / `ncs_*` / `bw_*` families,
  `split_last_arg`.
- **E. Brand-mismatch check** (issue #71(b)): `check_nursery_brand_
  mismatch_decls` + the `check_brand_*` / `benv_*` / `expr_brand` /
  `emit_brand_mismatch` families, `dump_brands`.

## Local mirrors (downward-glue, `dsg_` prefix)

The passes call AST-generic + trivial helpers that live in main.kai and
are used ubiquitously there. Per the downward-only rule they were
mirrored locally with a `dsg_` prefix (avoids duplicate C symbols in
the flat bundle):

- The `map_*` AST-functor family (`map_expr_kind` + 8 siblings; 25
  callers outside the range) → `dsg_map_*`.
- `pat_bindings` / `pat_bindings_loop` / `pfield_bindings_loop` (19
  callers) → `dsg_pat_bindings*`.
- `rename_param_names` / `rename_pat_names` / `rename_pats_names` /
  `rename_pfields_names` (binder-name walkers) → `dsg_rename_*`.
- `string_starts_with_q` / `starts_with_loop` (7 callers) →
  `dsg_string_starts_with_q*`.
- `ffi_extern_tag` / `ffi_extern_tag_prefix` (string-literal constants
  the FFI-shim emitter also reads) → `dsg_ffi_extern_tag*`. These are
  the *contract* between the axiom lowering (producer, moved here) and
  the FFI-shim emitter (consumer, stayed in main); the mirror is a
  string literal and cannot diverge.

The passes also call three helpers the modules lane already promoted to
`compiler.modules` — `pat_binders`, `fn_param_names`, `validate_pub_
access` — and consume them through `import compiler.modules`, the
correct downward direction. (Note `pat_binders` from modules and the
mirrored `dsg_pat_bindings` are distinct functions that coexist.)

## Dependency-arrow verification (the load-bearing data)

With F + the FFI/proto cluster pinned to main and RecInfo in ast:

- **UPWARD (extracted → main): NONE.** No symbol moving to desugar.kai
  calls back into main. The arrow never inverts. (Verified by the
  comment-free callee graph + the type-in-signature graph over all
  267 extracted decls — the latter caught 6 result types
  (`DPRDecls`, `PureExtract`, `UseRwProgram`, `NurseryRwProgram`,
  `BrandRec`, `BrandCall`) that the call-only scan missed and that
  kaic2's own pub-enforcement flagged at compile time.)
- **DOWNWARD (main → desugar): the 18 pass entry points** the driver
  invokes from `compile_source`, plus `lower_axiom_one` /
  `lower_const_one` which `tag_decl_module_origin` invokes.
- **main → ast: the RecInfo cluster** (`rec_find`, `rec_find_with_
  field`, `collect_records`, `rec_find_arity_loop`) consumed by the
  typer through the existing `import compiler.ast`.

## Surprises the line-range brief did not anticipate

1. **F is emit-coupled and stays.** The brief listed F (interp lift)
   as a desugar sub-pass to extract. It is — but it reads the
   precomputed C-escape field of the interp scanner's `IpLit`, which
   ties it to the emit layer. The honest cut leaves it in main with a
   one-line pointer to the future scanner-layer lane.

2. **`RecInfo` is the `ModuleEntry` of this lane.** A shared
   AST-adjacent table type in 16 pass signatures and 49 typer uses.
   Promoted to ast.kai (not parked in desugar.kai) so the future infer
   lane imports it from the sink, not laterally. `OpAr` would have been
   a second instance, but it only appears in F's signatures — with F
   left in main, `OpAr` did not need to move at all.

3. **The result-type pub gap recurs.** Six record result types
   (`DPRDecls` etc.) are returned by pub entry points but were not
   `pub` themselves. The call-only outbound scan does not see them
   (they are types, not calls); kaic2's pub-enforcement caught all six
   on the first modular compile. The type-in-signature graph predicted
   them; budget a pub-enforcement round regardless.

4. **An FFI tag is a producer/consumer contract split across the
   cut.** `lower_axiom_one` (moved) writes the `__kai_ffi__` tag;
   `body_is_ffi_extern` (stayed) reads it. The tag string is the
   contract. Mirroring `ffi_extern_tag` locally (string literal) keeps
   the producer self-contained without exporting upward.

## Verification commands

```sh
# section boundaries (A start .. F start .. typer start)
grep -nE "^# (issue #266|m7b #5b: pre-resolve|m7e §25|Cap-binding nursery|Brand-mismatch|m12\.8\.x phase 2|semantic types)" stage2/main.kai

# F stays: desugar_interp_decls must still be in main.kai
grep -nE "^fn desugar_interp_decls" stage2/main.kai

# RecInfo moved: must be pub in ast.kai, gone from main.kai
grep -nE "^pub type RecInfo" stage2/compiler/ast.kai
grep -cE "^type RecInfo" stage2/main.kai   # 0

# no extracted symbol calls back into main (UPWARD = none)
# the critical gate
cd stage2 && make selfhost   # kaic2b.c == kaic2c.c
```
