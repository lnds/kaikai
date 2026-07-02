# Lane experience — issue #987 (stage0 multi-line attribute lexing)

## Scope as planned vs shipped

Planned: port stage1's `lex_skip_attribute` (#986, b8768612) to
`stage0/lexer.c` so a `#[...]` attribute spanning physical lines is
skipped whole, plus a regression fixture wired into the tier that
covers stage0. Shipped exactly that: a new `skip_attribute` /
`skip_attr_string` pair in `stage0/lexer.c`, dispatched from
`skip_whitespace_and_comments` only when `#` is immediately followed
by `[`; `#` followed by anything else stays a line comment, and
`#{...}` string interpolation is untouched (`skip_attr_string` reuses
`lex_interp_block` for interpolation inside attribute strings).

## Design decisions

- Mirrored the stage1 fix one-to-one (bracket depth counting +
  stepping over single/triple-quoted string bodies) rather than
  inventing a stricter attribute grammar in the lexer. Parity between
  the two lexers was the goal; the parser never sees attributes in
  kaikai-minimal.
- `lex_interp_block` is forward-declared instead of moving code:
  minimal churn in a file whose layout groups skippers before literal
  scanners.

## Structural surprise: `examples/minimal/` cannot hold attributes

The first fixture attempt lived in `examples/minimal/` (auto-globbed
by the stage0 harness). CI shard-1 failed immediately, for two
reasons the brief did not anticipate:

- **Token parity**: stage2 `test-tokens` diffs kaic1 vs kaic2 token
  streams over every `examples/minimal/*.kai`. Attributes diverge by
  design — stage0/stage1 skip them as trivia, stage2 tokenizes `#[`
  for its attribute parser — so any attribute in that directory
  breaks parity.
- **Full-kaikai semantics**: stage2 also compiles those examples, and
  `#[derive(...)]` on a `fn` is a hard error in full kaikai.

The fixture therefore moved to `examples/attributes/` (the same home
#986 chose), and the stage0 harness gained an explicit
`ATTR_LEX_EXAMPLES` list appended to `test-run` — same pattern as
stage1's `test-check` list.

## Fixtures

- `examples/attributes/attr_multiline_ascii.kai` (+ `.out.expected`):
  plain-ASCII multi-line `#[doc("""...""")]` whose body contains `]`,
  `[` and quotes — exercises both the multi-line span (ruling out
  UTF-8 as the trigger) and bracket/quote handling inside string
  bodies. Picked up by stage0 `test-run`, stage1 `test-check`, and
  stage2 `test-attributes`.
- `examples/attributes/attr_doc_multiline.kai` (existing, from #986):
  now also compiled and run by the stage0 harness.

## Verification

Repros (triple-string and ASCII bracketed) went from exit 1 to exit 0
under kaic0; built clean with `-std=c99 -pedantic-errors -Wall
-Wextra`; full bootstrap kaic0 → kaic1 → kaic2 green; tier0 green;
stage2 `test-tokens`/`test-ast`/`test-types`/`test-check`/
`test-attributes` green locally.

## Follow-ups

None. The bug was latent (stage1/compiler.kai uses no multi-line
attribute); the fix removes the bootstrap trap and keeps the three
lexers' attribute handling consistent.
