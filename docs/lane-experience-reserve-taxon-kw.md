# Lane experience — reserve `taxon` / `taxology` keywords

Outcome: **pure lexical reservation.** `taxon` and `taxology` are now
hard keywords: no program can spell either as an identifier. The kind
system they will eventually introduce (`docs/kind-system-design.md`) is
NOT implemented here — this lane only pins the two words so a future
Orongo edition owns them and no Hanga Roa program spends them first.

## Scope as planned

Reserve `taxon` and `taxology` as hard (lexical) keywords. Add the two
token kinds, classify the words in the lexer, and give them a spelling
for diagnostics — mirroring `unit` / `with`. No declaration parsing, no
taxology/taxa/property/unification machinery. A negative fixture proves
the words are rejected as identifiers; selfhost must stay byte-identical.

## Scope as shipped

- `stage2/compiler/lex.kai` — `TkTaxon` / `TkTaxology` added to the
  `TokKind` enum, to `keyword_kind` (`else if text == "taxon" { … }`),
  and to `tk_name` for `--tokens` dumps.
- `stage2/compiler/parse.kai` — `tk_is` gains the two exhaustive arms
  (the token equality is enumerated per case, no wildcard), the
  top-level declaration dispatch gains two stub arms that emit
  `` `taxon` is a reserved keyword; taxon declarations are not yet
  supported `` (and the taxology sibling) instead of the generic
  "expected fn/type/…" error, and both words join the `closest_name`
  typo-suggestion list.
- Three negative fixtures in `examples/negative/parser_syntax/`:
  identifier-position (`let taxon = 1` → "expected pattern"), and
  declaration-position for each word → the "not yet supported" stub.

## Design decisions

- **Two failure surfaces, two error qualities.** A reserved word can
  appear where an *identifier* is expected (`let taxon = 1`) or where a
  *declaration* is expected (`taxon Foo {}`). The identifier case is
  already handled well by the existing parser — it reports "expected
  pattern" anchored at the word, identical to `let unit = 1`, so no new
  code is needed there. The declaration case, without a stub, would fall
  through to the generic "expected fn/type/effect/…" list, which is
  cryptic for a word that IS reserved. The two-line stub buys a clear
  message ("reserved keyword; not yet supported") for the cost of one
  `else if` per word — a good trade the brief anticipated.

- **No edition gate.** Reservation is available immediately; it is not
  gated on Orongo. The `EDITION` file is untouched. Reserving a word
  that has zero identifier uses across the whole tree breaks nothing, so
  there is no migration path to protect.

## Structural surprises

- **`tk_is` is exhaustive per-case, not wildcard-terminated.** Adding a
  variant to `TokKind` is not free: `tk_is(a, b)` enumerates every
  token kind as `a` with no `_ ->` fallback, so the two new kinds each
  need their own arm or the match is non-exhaustive. `tk_name` (lex.kai)
  is the only other exhaustive `TokKind` consumer; both were updated.
  Grepping for a representative kind (`TkUnitKw`, `TkComplex`) is the
  fast way to enumerate every exhaustive match site before rebuilding.

## Fixtures

- `taxon_reserved_ident` — `let taxon = 1` → `error: expected pattern`.
  This is the load-bearing one: it proves the word no longer lexes as
  `TkIdent`. Mirrors the `let_keyword_pattern` precedent for `fn`.
- `taxon_decl_unsupported` / `taxology_decl_unsupported` — declaration
  position → the reserved-keyword stub message. These pin the stub arms
  so a future kind-system lane that replaces them with real parsing must
  consciously update the goldens.

All three are auto-discovered by `tools/test-negative.sh` (glob over
`examples/negative/**/*.err.expected`), which runs under tier1. No
harness wiring needed beyond dropping the file pairs in place.

## Verification

- Selfhost byte-identical (`make tier0`: `kaic2b.c == kaic2c.c`).
- The three new negative fixtures PASS.
- Zero identifier uses of either word remained across
  `stage0/1/2 · stdlib · examples · demos` (only the new string
  literals and classification arms match a word-boundary grep).

## Follow-ups

- The kind-system implementation (`docs/kind-system-design.md`) is the
  successor lane. When it lands `parse_taxon_decl` /
  `parse_taxology_decl`, it replaces the two stub arms and retires the
  two declaration-position goldens.
