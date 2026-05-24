# Lane experience — issue #677 Phase 1h: parser extraction (2026-05-24)

The successful re-attempt at extracting the parser, after the Phase 1f/1g
lane rolled back its first naive try. This lane is the follow-through on
that retro's headline follow-up: *"Parse needs an up-front cross-section
analysis."* It split into four sub-lanes (H0, H1a, H1, H2) plus a docs
fix, each shipped as its own commit behind a green selfhost + tier0.

## Scope as planned

The Phase 1f/1g retro left parse extraction as the open work, with one
hard lesson: the naive "cut source range 115–7820 into parse.kai" failed
because that range bundles glue that does not belong to the parser
(typer `tp_*` helpers, the regex compiler, refinement discharge, the AST
dumper). The plan for 1h:

1. Produce a cross-section dependency analysis first.
2. Extract `compiler/parse.kai` surgically based on it.

## Scope as shipped

Five commits on the `parse-analysis` branch:

- **`f8dab2c` H0** — cross-section analysis doc + surface cleanup.
- **`a6968f9` H1a** — `compiler/util.kai` (shared string/list utils).
- **`e2769bd` H1** — `compiler/refinements.kai` (refinement engine).
- **`f96b2e9`** — backfill `#[doc]` on util/refinements pub fns.
- **`92fd1a3` H2** — `compiler/parse.kai` (the parser proper).

main.kai dropped from ~68.0k to ~60.2k LOC. The module suite grew from
74 to 134 tests (chars 7, diag 17, lex 19, ast 10, intervals 21, util
28, refinements 20, parse 12) plus 16 property checks.

### H0 (`f8dab2c`) — analysis + surface cleanup

`docs/lane-parse-cross-section-analysis.md` buckets every decl in the
rough parse range into: pure parse, co-located outsider (refinement /
regex / ffi), and glue. It recommended the load-bearing reorder:
**extract refinements.kai before parse.kai**, because the refinement
engine lives inside the parse line range but is a separate phase.

Two code changes shrank parse's eventual public surface:
- Moved the four field-name marker helpers (`pos_field_marker` etc.) to
  `compiler/ast.kai` — they are an AST-shape convention shared with
  codegen, not parser-internal.
- Added `pub fn parse_interp_expr(src) : Option[Expr]` wrapping
  `tokenize`/`parser_new`/`parse_expr`, and routed 7 of 8 string-interp
  re-parse sites through it, hiding the `PExpr` workspace type.

### H1a (`a6968f9`) — `compiler/util.kai`

The cross-section analysis surfaced a problem the original report
under-weighted: the refinement engine and parser both depend on ~10
low-level string/list helpers (`cat2`/`concat_all`, `char_at_or_zero`,
`list_has`, `string_lt`, `escape_str_body_for_c`, …) that lived in
main.kai with no module home. Since kaikai forbids import cycles,
refinements.kai could not import them from main.kai. Pulling them down
into a leaf module both sides import broke the cycle. 13 pub fns,
util.kai goes first in BUNDLE_SRCS.

### H1 (`e2769bd`) — `compiler/refinements.kai`

~109 decls, ~1490 LOC: contract-purity checking, the regex-predicate
subsumption engine (anchored-class parser + char-class mask family),
the compile-time partial evaluator (`try_eval_int`/`try_eval_pred`),
interval entailment, and the whole issue #83 call-site substitution
analysis. Split at the Parser boundary: `Contracts`,
`parse_contracts_loop`, `check_pred_not_false`, `maybe_wrap_pure` stay
in main.kai (they thread `Parser`) and travel with parse.kai in H2.
8 pub fns.

### H2 (`92fd1a3`) — `compiler/parse.kai`

~6260 LOC: the entire parser. Three pre-moves broke remaining cycles —
`has_interp`→util.kai, `tp_unit_suffix`→ast.kai, `tp_make` travels into
parse.kai. Public surface kept to `parse_program`, `parse_interp_expr`,
and the `tp_*_bounds` decoders the typer reads. The one Perceus
interp-rewrite site that called `parser_new`+`parse_expr` directly was
routed through `parse_interp_expr`, so `Parser`/`PExpr` no longer leak
(Parser is pub only because PParser carries it).

## Design decisions and alternatives considered

### Refinements-before-parse vs parse-first

The decisive call. The phase-1f/1g rollback proved parse-first hits the
refinement engine embedded in the parse range. H1 pulls that engine out
first, leaving parse.kai with only its own callees. Confirmed correct:
H2's cycle check came back empty on the first try.

### Contract cluster: split at the Parser boundary vs move-all / keep-all

The contract-desugar helpers are entangled with the parser: `Contracts`
carries `p: Parser`, and `parse_contracts_loop` (parser) calls
`check_pred_not_false` (which calls `refinements.try_eval_pred`). The
clean cut was by *what touches Parser*: the pure helpers
(`wrap_with_contracts`, `preds_to_asserts`, …) moved to refinements.kai;
the four Parser-touching ones stayed and went to parse.kai in H2. This
kept every cross-module edge pointing main→parse→refinements, never back.

### util.kai as a new module vs duplicating helpers vs dumping into ast.kai

Considered all three (asked the integrator). A dedicated leaf module won:
`concat_all` (961 uses), `list_has` (176), `cat2` (71) are heavily-used
basics that earn their own module; duplicating them would rot, and
ast.kai is the AST data model, not a util grab-bag.

### Minimising parse.kai's public surface

`parse_expr`/`parser_new` had exactly one external caller (the Perceus
interp rewriter, which also reused the token stream). Rather than expose
`Parser`+`PExpr`, the site re-tokenises once for its span mapping and
calls `parse_interp_expr` for the AST. The redundant tokenise is cheap
and keeps two workspace types module-private.

## Structural surprises the brief did not anticipate

### The cross-section report over-estimated section C

The H0 analysis assumed the issue #83 call-site-substitution section ran
6053→7821. In reality it ends ~6549; between it and `parse_program` sit
~1250 LOC of *more* parse declarations (effect/protocol/impl/attribute/
type/test/import parsers + `tp_*` bounds helpers). Re-mapping against the
live file rather than the report's line numbers caught this. Lesson: the
section-header comments are the stable anchors, not the line numbers.

### The selfhost (bundle) hides import + pub-leak errors that the test
### path (separate compilation) catches

The biggest process lesson. `make selfhost` builds kaic2 from the
*concatenated bundle* (kaic1), where every symbol is flat — a missing
`import` or a private-type leak resolves fine. But `kai test
tests/test_<mod>.kai` runs kaic2 in *package mode* (resolving imports),
which catches them. H1 shipped missing `import compiler.chars`; H2 hit
three pub-leaks (`Parser`/`PExpr`/`PParser`). The per-module tests are
not optional polish — they are part of the merge gate, exactly the
discipline lex/ast/intervals established.

### `#{` inside a `#[doc("...")]` string is lexed as interpolation

The phantom bug of the lane. H2's selfhost reported `cannot find
concat_all` in two cache-layer functions — far from any parser code, and
`concat_all` resolved fine in 925 other sites. Root cause: the `#[doc]`
on `has_interp` contained a literal `` `#{` ``, which the lexer read as
an unterminated string interpolation. That corrupted util.kai's lexing
inside the bundle, producing a kaic2 that mis-resolved downstream. The
error location (cache layer) was nowhere near the cause (util.kai doc
string). Rule for future docs: never put `#{` in a string literal,
including attribute strings.

### pub-count by grep counts comment mentions as callers

Both the H1 and H2 mapping initially over-counted pub candidates because
a function name appearing in a `# comment` matched the caller grep.
`preds_to_asserts` (H1) and several others looked like they needed pub
until comment lines were filtered out. Filter `grep -vE '^\s*[0-9]+:\s*#'`
before counting external callers.

## Fixtures added and coverage gaps

- `stage2/tests/test_util.kai` — 28 unit tests + 6 property checks over
  every util.kai pub fn.
- `stage2/tests/test_refinements.kai` — 20 unit tests + 4 property
  checks over the pure AST-only surface (`try_eval_int`/`try_eval_pred`,
  `find_impure_call`, `synth_refine_pred_fn_name`, `refinement_pure_names`).
- `stage2/tests/test_parse.kai` — 12 unit tests + 2 property checks over
  `parse_interp_expr` (precedence, calls, unary) and `parse_program`
  (decl counting, error surfacing).

Coverage gaps: the regex-subsumption, interval-entailment, and
call-site-substitution paths in refinements.kai are exercised only
implicitly (selfhost + examples/ fixtures), not by direct unit tests —
they need richer AST or full-Decl fixtures. The parser's pattern,
type-expression, effect-row, and sugar-desugar productions are covered
by `parse_program` over representative sources rather than per-production
assertions.

## Real cost vs estimate

Larger than 1f/1g because of the three import cycles and the `#{`-in-doc
phantom. The cross-section analysis (H0) paid for itself: H1 and H2 each
needed only one cycle-check pass, and the only surprises were the
pub-leaks (mechanical) and the doc-string lexing bug (one-line fix once
located). The doc-string bug cost the most debugging time precisely
because the error surfaced far from its cause.

## Follow-ups for the next lanes

### Contract cluster + AST dumper still in main.kai

`Contracts`/`parse_contracts_loop`/`check_pred_not_false`/`maybe_wrap_pure`
went to parse.kai, but the AST dumper (`dump_*`, --ast) stayed in
main.kai: its display helpers (`type_expr_display`, `row_label_names`,
`unit_expr_display`) are reused by typer/emitter diagnostics. It extracts
once those consumers do, or once the display helpers move to a shared
module (diag.kai is the natural home).

### Next extraction target: resolver / typer

main.kai is now ~60.2k LOC, still dominated by the resolver (`# checker:
name resolution`), the inference engine, the monomorphiser, the Perceus
pass, and the two emitters (C + LLVM). `compiler/resolve.kai` or
`compiler/infer.kai` is the natural next cut. The resolver's `Env` type
and the typer's `tp_kind_of`/`tp_strip_kind` decoders are the
already-identified shared symbols to watch.

### Un-pub audit for the new modules

util.kai, refinements.kai, and parse.kai were extracted with the minimal
pub surface justified by real external callers, so a separate un-pub
audit (as ran on chars/diag/lex/ast/intervals in `2e4f02d`) should be
light — but worth a pass to confirm no pub fn lacks an external caller.

### CI runner offline

As in 1f/1g, the macOS m2 CI runner was offline; validation was local
(selfhost byte-identical + tier0 demos baseline 34 + module suite
134/134). Re-confirm under tier1 CI before merge.
