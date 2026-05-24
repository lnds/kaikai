# Parse cross-section analysis — issue #677 phase 1h pre-flight

**Date:** 2026-05-24
**Branch:** `parse-analysis` (research-only)
**Base:** `main` HEAD `2e4f02d`
**Author:** lane research agent
**Goal:** decide what should and should not move into `compiler/parse.kai`.

## TL;DR

The naive "extract lines 115–7821" plan from phase 1f/1g rolls up four
unrelated machines into one module. Section A (true parser, lines
115–6052) is genuinely parse-shaped; section B (lines 6053–7821) is
*call-site refinement substitution analysis*, an entirely different
phase that happens to live in the same source range. Inside section A
there are still four sub-clusters that don't belong to parse:

| Bucket                 | Decls | LOC   | Home               |
| ---------------------- | ----- | ----- | ------------------ |
| **A** parse (true)     |   217 | 4 761 | `compiler/parse.kai` |
| **B-refinement-regex** |    44 |   467 | `compiler/refinements.kai` (new) — regex-class semantic engine |
| **B-refinement-engine**|    21 |   399 | `compiler/refinements.kai` (new) — interval + partial-eval engine |
| **B-contracts-desugar**|     9 |   151 | `compiler/refinements.kai` (new) — parse-time AST surgery |
| **B-ffi**              |     7 |   149 | `compiler/ffi_validate.kai` or inside `parse.kai` (judgment call) |
| **C** issue #83 substitution |  98 | 1 769 | `compiler/refinements.kai` (new) — DIFFERENT module than refinements engine, but adjacent |
| program entry          |     3 |    32 | `compiler/parse.kai` (entry points) |

The path forward is **not** "extract parse first." It is **extract
`refinements.kai` first**, pulling buckets B-refinement-* + section C
(roughly 2 800 LOC, well-separated callees), and **then** parse what
remains. The current `main.kai` after Phase 1g already has clean
extracted neighbours (`compiler/chars.kai`, `compiler/diag.kai`,
`compiler/lex.kai`, `compiler/ast.kai`, `compiler/intervals.kai`). A
new `compiler/refinements.kai` slots in naturally next to
`compiler/intervals.kai`, before parse.

There is also a **real reverse coupling** to plan for: the emitter and
several typer passes call `parser_new` + `parse_expr` (8 sites,
re-parsing string-interpolation bodies). Both must be exported `pub
fn` when parse.kai is born.

---

## Boundaries (current line numbers in `stage2/main.kai`)

`stage2/main.kai` is 68 079 lines as of `2e4f02d`. The pre-Phase-1g
"115–7820" rough range now maps to:

```
115        # parser state      (start of section A)
349        # literal decoders
509        # precedence climbing fwd-decls (commentary)
535        # expression parsing
1411       # primary expressions placeholders
2492       # list / range
2579       # block, if, match
3327       # patterns
3633       # type expressions
4061       # declarations
6053       # Issue #83 sub-case 3 — call-site substitution analysis (SECTION C)
7822       # program (entry points)
7854       # AST dumper
8446       # checker: name resolution
```

Two natural cut-points exist inside the rough range:

- **6052 / 6053 — section A / section C divide.** Above is "parser
  state + everything the parser invokes during parse." Below is
  "call-site refinement substitution analysis": a post-parse static
  pass over the produced AST, completely separate from parsing.
- **7821 / 7822 — section C / program divide.** Below the program-
  loop entry points `parse_program_loop`, `parse_program`, `PParser`.
  These are the public surface of the parser and should travel WITH
  parse.kai, not as a co-located stub.

For Phase 1h we should treat 115–6052 + 7822–7853 as "parse range"
and 6053–7821 as "refinement-engine extra range."

---

## Section A — true parse range (115–6052)

328 top-level decls total in this range, 5 938 LOC. Bucketing by
callee analysis:

### A-parse (217 decls, 4 761 LOC) — safe to extract

These are called only from inside the parse range or from the
program-entry section (parse_program → parse_decl → …). Sub-grouped
for context (a future parse.kai-internal section layout):

| Sub-cluster              | Decls | LOC | Lines (representative) |
| ------------------------ | ----- | --- | ---------------------- |
| parser-primitives        |    19 | 201 | 115–349 (`Parser`, `p_*`, `tk_is`, `tok_source`) |
| literal-decoders         |     9 | 172 | 349–509 (`decode_int`, `decode_real`, `decode_char`) |
| result-helpers           |    32 |  37 | 537–566 (`ok_e/err_e/...`) |
| workspace-types          |    24 |  75 | scattered (`PExpr`, `PStmt`, `PPat`, …) |
| expr-parsing             |    20 | 556 | 535–2491 (incl. precedence chain) |
| placeholder-desugar      |    19 | 259 | 584–874 (pipe-`_` substitution) |
| match-desugar            |    19 | 172 | 3086–3326 (multi-scrutinee, arm columns) |
| block-if-match           |     8 | 277 | 2579–3326 |
| pattern-parsing          |     2 |  69 | 3327–3633 |
| type-row-parsing         |    12 | 277 | 3633–4061 |
| list-record-tuple        |    12 | 446 | 2492–2950 |
| stmt-parsing             |     2 |  79 | scattered |
| decl-parsing             |     9 | 491 | 4061–4972 |
| lambda-parsing           |     8 | 119 | 1294–2492 |
| ast-shape-helpers        |    11 | 104 | scattered |
| other parse_*            |    38 | 1391 | (rest — see below) |
| misc / type-helpers      |     3 |  36 | |

The "other parse_*" cluster contains 38 fns that didn't pattern-match
my heuristic but are clearly parse-internal: `parse_concat`,
`parse_pow`, `parse_trailing_lambda`, `parse_call_args*`,
`parse_todo_bang`, `parse_intrinsic`, `parse_handle*`,
`parse_clause_params`, `parse_ident_primary`, `parse_variants_of`,
`parse_ensure_primary`, `parse_regex_sigil` (the parser of `r"..."`
sigils, distinct from the regex-semantic engine — see B-refinement-
regex), `parse_paren_or_lambda`, `parse_first_list_elem`,
`parse_brace_primary`, `parse_index_assign_rest`, `parse_var`,
`parse_more_scruts`, `parse_n_patterns`, `parse_ident_pattern`,
`parse_variant_sub_patterns`, `parse_optional_refinement`,
`parse_refinement_predicate`, `parse_refinement_matches_with`,
`parse_refinement_cmp_with`, `parse_named_type`, `parse_const_decl`,
`parse_at_legacy_extern_c_attribute`, `parse_optional_extern_c_override`,
`parse_clause_block`, `parse_case_arms`, `parse_case_arm_tail`,
`parse_case_multi_arms`, `parse_case_multi_arm`,
`parse_optional_pure_attribute`. All called only from other parse
fns.

### B-refinement-regex (44 decls, 467 LOC) — outsider, regex semantic engine

These live inside section A but compute *whether a refinement
predicate `matches(x, regex)` subsumes another regex predicate*. The
calling site is `regex_pred_subsumed` (line 5349), itself called by
`pred_entailed_by_params` (5306) inside `preds_to_asserts` inside
`wrap_with_contracts`. The CHAIN sits in parse range; the LOGIC is
analysis engine.

| Group          | Decls | Range         | Notes |
| -------------- | ----- | ------------- | ----- |
| sigil encoder  |   2   | 2066–2092     | `regex_pattern_to_kai_str_body`, `regex_pattern_has_hash`. These are called from `parse_regex_sigil` (line 2039) during parsing of `r"…"` literals — they *encode* the pattern as a string body. Borderline: stay with parse? Or move? They use only string primitives, no AST, but they exist to support a parser construct. **Recommendation: keep with parse.kai** (sigil encoding is part of how the parser produces the AST for regex literals). |
| subsume        |   3   | 5349–5392     | `regex_pred_subsumed`, `pred_regex_pattern_named`, `extract_regex_literal` |
| string decode  |   2   | 5416–5436     | `decode_regex_str_span`, `decode_regex_loop` |
| pattern contains |  1  | 5437          | `regex_pattern_contains` — top-level entry into subsume |
| anchored class | 12    | 5456–5658     | `parse_anchored_class`, `parse_class_atom`, `parse_bracket_*`, `parse_class_escape`, `parse_escape_atom`, `parse_quant`, `parse_repeat_quant`, `parse_pat_int*`, `is_regex_meta`. NB: these `parse_*` prefixes are misleading — they parse *regex patterns inside strings*, not kaikai source. |
| char masks     | 20    | 5680–5775     | `mask_*` 0/1 bitmap family for character-class subset checks |
| class compare  |  1    | 5776          | `ac_contains` |
| AST aux types  |  4    | 5453–5612     | `AnchoredClass`, `ParsedClassAtom`, `BracketItem`, `BracketFirst`, `QuantParse`, `IntParse` |

**Suggested home:** `compiler/refinements.kai`, or a sub-module
`compiler/refinements/regex.kai` if the refinement file gets large.
Subgroup `sigil encoder` (the 2-decl 26-LOC one at lines 2066–2091)
is the only contestable item — see note above.

### B-refinement-engine (21 decls, 399 LOC) — outsider, partial-eval + interval engine

These compute "given a predicate expression, can we decide it at
compile time / prove it from another predicate?" Mostly called from
*within* parse range (during contracts desugar) but also have
genuine downstream callers in the typer and the emitter.

| Decl                              | Line | LOC | Downstream caller? |
| --------------------------------- | ---- | --- | ------------------ |
| `pred_canon`                      | 5809 |  24 | yes (5×) — sub-case-3 (6055+) |
| `subst_self`                      | 5790 |   8 | no |
| `subst_self_list`                 | 5798 |  11 | no |
| `cmp_lt`                          | 5833 |  19 | no |
| `pred_entailed_by_refinement`    | 5852 |  14 | no |
| `interval_of_pred`                | 5866 |  16 | no |
| `interval_from_cmp`               | 5882 |  13 | no |
| `ival_int_max_const`              | 5895 |   1 | no |
| `ival_int_min_const`              | 5896 |   8 | no |
| `check_pred_with_interval`        | 5904 |  28 | no |
| `expr_struct_eq`                  | 5932 |  29 | yes (2×) — sub-case-3 |
| `expr_list_struct_eq`             | 5961 |  16 | no |
| `try_eval_pred`                   | 5977 |  37 | yes (3×) — sub-case-3 |
| **`try_eval_int`**                | 6014 |  20 | **yes — emit_expr (14299), unbox (42023), llvm codegen (48062)** |
| `eval_int_op`                     | 6034 |   9 | no |
| `eval_int_cmp`                    | 6043 |  10 | no |
| `pred_entailed_by_params`        | 5306 |  43 | no |
| `refinement_pure_names`           | 5085 |  18 | yes (1×) at 20034 |
| `find_impure_call`                | 5103 |  53 | yes (2×) at 20135-ish |
| `find_impure_call_args`           | 5156 |  15 | no |
| `synth_refine_pred_fn_name`       | 3295 |   7 | yes (2×) at 19903, 19982 |

**Suggested home:** `compiler/refinements.kai`. `try_eval_int` is a
general partial evaluator used by codegen even when no refinement is
in play — if `refinements.kai` feels wrong for it, `compiler/eval.kai`
is a plausible sibling. The right call is probably: put it in
`refinements.kai` first (the file is where it originated), and split
later only if codegen evolves to need richer partial-eval.

### B-contracts-desugar (9 decls, 151 LOC) — outsider, AST surgery at parse time

These wrap a parsed `fn` body with `assert` statements that enforce
`requires`/`ensures` clauses. The lane is invoked from parse_decl
(`wrap_with_contracts` is called at lines 4742, 4760, 4770) but the
WORK is AST surgery downstream of pure parsing.

| Decl                       | Line | LOC | Callers          |
| -------------------------- | ---- | --- | ---------------- |
| `check_pred_not_false`     | 5171 |  20 | parse range only |
| `wrap_with_contracts`      | 5191 |  26 | parse range only |
| `preds_to_asserts`         | 5217 |  43 | parse range only |
| `build_violation_msg_span` | 5260 |  19 | parse range only |
| `collect_param_preds`      | 5282 |  24 | parse range only |
| `maybe_wrap_pure`          | 5019 |   7 | parse range only |
| type `Contracts`           | 5026 |   7 | parse range only |
| type `PureAttr`            | 4969 |   2 | parse range only |
| type `ParamPred`           | 5279 |   3 | parse range only |

**Suggested home:** `compiler/refinements.kai`. They sit at the
boundary between parser and refinement engine; either could host
them. They couple TO the engine (call `pred_entailed_by_params` etc.),
not to parser primitives, so they belong with the engine. Leaving
them in parse.kai would force parse.kai to depend on the engine
module, which is precisely what we want to avoid.

### B-ffi (7 decls, 149 LOC) — outsider, C-ABI surface validation

Validates that an `extern "C" fn` declaration has C-safe types and a
valid C identifier symbol. Called from `parse_optional_extern_c_override`
(line 4269) and `parse_decl`.

| Decl                          | Line | LOC | Callers |
| ----------------------------- | ---- | --- | ------- |
| `extern_row_has_ffi`          | 4308 |   8 | parse only |
| `row_label_names_has`         | 4316 |  21 | parse only |
| `is_c_abi_safe_type`          | 4337 |  19 | parse only |
| `first_non_c_abi_param`       | 4356 |  16 | parse only |
| `validate_extern_c_decl`      | 4401 |  51 | parse only |
| `is_valid_c_identifier`       | 4452 |  10 | parse only |
| `is_valid_c_identifier_rest`  | 4462 |  24 | parse only |

**Judgment call.** These are called only from the parser, but they
represent a separate concern (FFI ABI conformance, not grammar). Two
defensible homes:

1. **Keep in parse.kai** — they're invoked only from parse, never
   re-used, and have no incoming dependencies from other modules.
   Simplest path; cost is parse.kai owns FFI validation.
2. **Move to `compiler/ffi_validate.kai`** — single-purpose tiny
   module, cleaner separation. Cost is 7 more decls of module
   ceremony for a one-caller module.

**Recommendation:** keep in parse.kai for now. The FFI rules are
likely to grow (issue tracker mentions ABI work) and a dedicated
module can spin out cleanly when it earns its weight. ~150 LOC of
co-located validation is below the "extract" threshold.

---

## Section C — call-site substitution analysis (lines 6053–7821, 98 decls, 1 769 LOC)

This is a post-parse static analysis pass that walks the produced AST
and rewrites or annotates calls based on whether `requires` clauses
can be discharged at compile time. The header comment (line 6055)
labels it "Issue #83 sub-case 3 — call-site substitution analysis."
It is **not parser code at all** — the original "extract lines
115–7820" plan that failed in Phase 1f/1g failed largely because of
this region.

Key decls:

| Group          | Decls | Lines (approx) |
| -------------- | ----- | -------------- |
| signature collection | 11 | 6087–6210 (`CalleeSig`, `CalleeReq`, `collect_callee_sigs*`, `extract_callee_reqs`, `extract_*_loop`, `assert_msg_is_requires`, `string_starts_with_requires`, `starts_with_after_quote`, `pred_starts_with_loop`) |
| refinement bindings  |  4 | 6212–6238 (`RefBinding`, `collect_caller_refinements`, `lookup_caller_refinement`) |
| status reporting     |  2 | 6239–6249 (`CallSiteStatus`, `CallSiteReport`) |
| classification       |  3 | 6251–6305 (`classify_arg_vs_pred`, `subst_var`, `subst_var_list`) |
| AST aux              |  2 | 6284–6288 (`mk_int`, `mk_var`) |
| pretty               |  1 | 6306–6318 (`expr_show_pred`) |
| recursive walker     |  6 | 6319–6450 (`analyze_calls`, `analyze_calls_in_args`, `analyze_calls_in_stmts`, `analyze_calls_in_arms`, `analyze_calls_in_list_elems`, `lookup_callee_sig`) |
| remainder            | ~70 | 6451–7821 (continued analysis, reporting, summarisation, helpers) |

This section depends on `pred_canon`, `try_eval_pred`, `try_eval_int`,
`expr_struct_eq` from B-refinement-engine. Both should travel
together.

**Suggested home:** `compiler/refinements.kai`. Section C is the
**caller** of the engine in B-refinement-engine; both modules form a
single conceptual unit ("refinement discharge"). Splitting them
would create a circular dependency on either side. Pack everything
into one `refinements.kai` for Phase 1h-pre; if size becomes a
concern, split into `refinements/engine.kai` (B-engine + B-regex) and
`refinements/discharge.kai` (section C + B-contracts-desugar) once
the file exists and we can measure.

---

## Reverse coupling — downstream callers reaching back into parse

Two genuine couplings exist; the rest are stale comments.

### Coupling 1: emitter and typer re-parse string-interpolation bodies

`parser_new` and `parse_expr` are called from **eight** sites outside
the parse range, all in the emitter and adjacent typer passes:

```
12641, 12642   emit_expr (string interp re-parse)
13118, 13119   emit_expr lambda variant
13549, 13550   string-interp helper
22878          (commentary only)
23100, 23101   typer string-interp checking
42955, 42956   unboxing string-interp
43934, 43935   another emit path
44971, 44972   yet another
48469, 48470   llvm codegen
```

The interpolation body `#{expr}` is lexed and parsed lazily by the
phase that touches it, not by the main parser walk. This means:

- `parser_new` and `parse_expr` MUST be `pub fn` in the future
  `compiler/parse.kai`.
- `Parser` type must be `pub type`.
- `PExpr` workspace type need not be exported (callers consume `.r`
  directly inside the same module — *but they do today, by grepping
  the call sites:* every caller pattern-matches on the returned
  `PExpr`'s `.r` field). Either export `PExpr` too, or refactor the
  eight call sites to use a thin `parse_interp_expr(...) :
  Option[Expr]` exported helper that hides `PExpr`.

  **Recommendation:** expose a `pub fn parse_interp_expr(file, src,
  toks) : Option[Expr]` in parse.kai that wraps `parser_new` +
  `parse_expr` and returns just the AST. Eight call sites become
  one-liners and parse.kai stops leaking its workspace types. Land
  this BEFORE the parse extraction to reduce diff size at extraction
  time.

### Coupling 2: AST shape conventions used by codegen

```
18904   match {FI(nm0, _) -> is_spread_field_marker(nm0)}
19070   match {FI(nm, _) -> is_pos_field_marker(nm)}
```

Codegen branches on the synthetic marker names `__pos_<i>__` and
`__spread__` that the parser injects into anonymous-record/list
field-init lists. These four tiny fns (`pos_field_marker`,
`spread_field_marker`, `is_pos_field_marker`, `is_spread_field_marker`)
plus the constants are an AST-shape *convention*, not parser-internal.

**Recommendation:** move them to `compiler/ast.kai` (already
extracted). They will then be reachable from both parser and codegen
without cross-module dependency. ~30 LOC total.

### Non-couplings (stale references in comments only)

`decode_real` (line 64993), `desugar_placeholder_arg` (30640),
`parse_trailing_lambdas` (22191), `parse_record_pos_loop` (18710),
`build_marm_columns` (35178), `parse_optional_arm_refine` (19858,
64189), `parse_row_labels` (36931), `parse_unit_expr_rest` (59375).
All grep-positive but only inside `# ...` comments. No code coupling.

---

## Recommended sequencing

Three lanes, in order. Each is small enough to finish in one PR with
a clean retro. Do **not** try to do them together.

1. **Lane H0 — surface-area cleanup (one PR, ~80 LOC diff).**
   - Add `pub fn parse_interp_expr(file, src, toks) : Option[Expr]`
     thin wrapper in `main.kai` (still pre-extraction) that hides
     `PExpr`. Update the eight `parser_new` + `parse_expr` call
     sites in emitter/typer to use it. Net effect: parse's
     workspace `PExpr` type stops being a public surface.
   - Move `pos_field_marker`, `spread_field_marker`,
     `is_pos_field_marker`, `is_spread_field_marker` to
     `compiler/ast.kai`. ~30 LOC moving + 2 call-site updates in
     codegen.
   - Acceptance: tier1 green; no observable behaviour change.

2. **Lane H1 — extract `compiler/refinements.kai` (one PR, ~2 800 LOC moved).**
   - Move sections C (6053–7821) + B-refinement-regex +
     B-refinement-engine + B-contracts-desugar from `main.kai`
     into a new `compiler/refinements.kai`.
   - Total moved: ~98 + 44 + 21 + 9 = 172 decls, ~2 786 LOC.
   - Re-pub only `wrap_with_contracts` (called from parse_decl),
     `analyze_calls` (called from the sub-case-3 driver in
     main.kai), `synth_refine_pred_fn_name` (called from the
     refinement synthesiser at 19903/19982), `refinement_pure_names`
     (called at 20034), `find_impure_call` (called at 20135),
     `try_eval_int` (called from emit/unbox/llvm), `try_eval_pred`
     and `pred_canon` and `expr_struct_eq` if they have callers
     outside the moved range (verify with grep).
   - Subgroup `sigil encoder` (2066–2091) **stays** with parse —
     it's invoked from `parse_regex_sigil` during literal parsing.
   - Acceptance: tier1 green, byte-identical selfhost.

3. **Lane H2 — extract `compiler/parse.kai` (one PR, ~5 000 LOC moved).**
   - Move what's left in the parse range: 217 A-parse decls + 7
     B-ffi decls + `program` entry section. Total ~4 942 LOC.
   - Public surface: `pub fn parse_program`, `pub fn
     parse_interp_expr` (the H0 wrapper). All other parse_*
     and p_* fns stay module-private.
   - Acceptance: tier1 green, byte-identical selfhost. The retro
     for H2 should be ~50–80 lines (the work is now mechanical
     once H0 and H1 cleared the obstacles).

### Why this order

- **H0 first** removes the only cross-module type leak (`PExpr`) and
  the only AST-shape coupling to ast.kai. The diff is small enough
  that a regression bisects cleanly.
- **H1 before H2** is the load-bearing insight from this analysis.
  The original Phase 1f/1g plan tried to extract parse first and
  bumped into the refinement engine living inside the parse range.
  Pulling refinements out first leaves parse.kai with only its own
  callees.
- **H2 last** becomes the easiest of the three: once H0 and H1
  succeed, H2 is a near-mechanical cut.

### What NOT to do

- Do not try to bundle H1 + H2 into one PR. The combined diff
  would be ~7 800 LOC across two new modules with new public
  surfaces, and any selfhost-byte mismatch would be impossible to
  bisect.
- Do not split B-refinement-engine across two new modules (e.g.
  `eval.kai` + `refinements.kai`) on first extraction. Co-locate
  everything, then split later if the file grows past the typical
  ~3 000 LOC compiler-module threshold.
- Do not move B-ffi decls in H1 — they're called from parse_decl,
  and a separate `compiler/ffi_validate.kai` would be ~150 LOC of
  ceremony for a one-caller module. Either ship them with parse in
  H2 or in a follow-up lane.

---

## Verification commands a reviewer can run

```sh
# section boundaries
grep -nE "^# ====|^# parser state|^# program|^# Issue #83" stage2/main.kai

# decl counts per range
grep -nE "^(pub )?(fn|type) " stage2/main.kai | awk -F: '$1>=115 && $1<=6052' | wc -l   # → 328
grep -nE "^(pub )?(fn|type) " stage2/main.kai | awk -F: '$1>=6053 && $1<=7821' | wc -l  # → 98

# reverse coupling
grep -nE "\bparser_new\b" stage2/main.kai | awk -F: '$1>7853'   # → 8 hits
grep -nE "\bparse_expr\b" stage2/main.kai | awk -F: '$1>7853'   # → 8 hits + 1 comment
grep -nE "\b(is_pos_field_marker|is_spread_field_marker)\b" stage2/main.kai | awk -F: '$1>7853'   # → 2 hits

# real downstream uses of refinement-engine
grep -nE "\b(try_eval_int|try_eval_pred|pred_canon|expr_struct_eq|synth_refine_pred_fn_name)\b" stage2/main.kai | awk -F: '$1>7821'
```

## Notes for the integrator

- This analysis is a snapshot at HEAD `2e4f02d`. The line numbers
  will drift as soon as another lane lands; the **boundary headers**
  (`# parser state`, `# Issue #83 sub-case 3`, `# program`) are the
  stable anchors. Always re-grep before acting.
- The "B-refinement-regex sigil encoder" group is the one judgment
  call worth a second opinion: 26 LOC, called only from
  `parse_regex_sigil`, but conceptually part of the regex string
  encoding rather than parser primitives. Either home is defensible.
- I did not run `make selfhost` or `make tier1` — this lane is
  research-only and modifies zero `.kai` source.
