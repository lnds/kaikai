# Lane experience — issue #745: text.display_width

## Scope as planned vs. as shipped

**Planned (path 2, the #744 follow-up):** add `display_width(s) : Int`,
a wcwidth-style terminal column count over Unicode codepoints, built on
the #744 codepoint iterator (`string.chars`). East Asian Width table +
zero-width handling; codepoint-level, not grapheme (UAX #29).

**Shipped:** exactly that, as a new opt-in module `stdlib/text.kai`
(not folded into `core/string.kai`):
- `pub fn char_width(cp: Int) : Int` — 0 / 1 / 2 columns for one codepoint.
- `pub fn display_width(s: String) : Int` — sum of `char_width` over
  `string.chars(s)`.

All four issue acceptance cases pass on both backends:
`display_width("hello")==5`, `("你好")==4`, `("▸ Edit")==6`, `("🎉")==2`.

## Design decisions and alternatives

- **New module `stdlib/text.kai`, not `core/string.kai`.** The issue
  offered both. `core/string.kai` is part of the bootstrap *prelude*
  (loaded into every compilation); an ~80-interval East Asian Width
  table would bloat the prelude every build pays for. `text` is opt-in
  (`import text`), like `uuid`/`regexp`/`http` — the EAW table loads
  only when a consumer actually needs column width. The issue itself
  frames this as "the shared text layer" that manutara / ahu.cli /
  i18n would import, which argues for a dedicated module over inflating
  core. Catalogued in `docs/stdlib-layout.md`.
- **Curated interval tables, not the full Unicode database.** The wide
  set (CJK, Hangul, Hiragana/Katakana, fullwidth forms, the common
  emoji blocks) and the zero-width set (combining marks, ZWJ/ZWSP,
  bidi/format controls, variation selectors) are the core ranges every
  wcwidth implementation ships (Go x/text/width, Rust unicode-width,
  Python wcwidth). Full per-codepoint coverage would bloat the module
  for codepoints no terminal app exercises. Documented as a deliberate
  95%-case subset in the module header and the issue's scope note.
- **Codepoint-level, grapheme deferred.** Emoji ZWJ sequences, flags,
  skin-tone modifiers that render as one wide glyph need UAX #29
  segmentation and are explicitly out of scope (the issue says so).
  `display_width` measures codepoints; a grapheme layer can follow.
- **ANSI-escape skipping left to the caller** (issue's first-cut
  guidance). A `display_width_ansi` sibling that strips `ESC [ … m`
  was not added — a TUI knows whether its strings carry color codes,
  and the core need is the column count.

## Structural surprise

**`or` cannot lead a continuation line.** The first cut wrote the
interval predicates as a short-body `fn is_wide(cp) : Bool =` with each
`or in_range(...)` on its own line. The parser rejects a binary
operator at the *start* of a continuation line (the newline terminates
the expression): `error: expected fn/type/... ` at the first `or`.
Parenthesising the multi-line expression did not help either. The form
that parses is a **block body with the `or` trailing each line**
(`cp == 1 or` ↵ `cp == 2`). Rewrote both predicates that way. Worth
knowing for any future long-disjunction helper — `kai info syntax`
does not call this out.

## Fixtures added

- `examples/stdlib/text_display_width.kai` (+ `.out.expected`) — the
  issue's acceptance cases plus a mixed `a你🎉b`==6 string and
  `char_width` of single codepoints (narrow/wide/combining/ZWJ). Picked
  up by the `examples/stdlib/*.kai` glob in `test-stdlib`; passes on C
  and LLVM (parity verified).
- `stdlib/text.kai` intrinsic test blocks: `char_width` and
  `display_width` acceptance cases.

## Verification

- All acceptance cases pass; `test-stdlib` survey green (148 OK, 0 FAIL).
- C/LLVM backend parity on the fixture.
- Selfhost byte-identical (the new module doesn't perturb the
  compiler's own bootstrap — it's opt-in, the compiler never imports it).

## Follow-ups

- Grapheme-cluster width (UAX #29) for ZWJ emoji / flags, if demand
  appears — a separate, harder problem the issue scopes out.
- `display_width_ansi` (strip `ESC [ … m`) if a consumer wants the
  framework to own escape-skipping rather than doing it itself.
- The EAW table is a curated subset; if a real consumer hits a wide
  codepoint outside the shipped ranges, widen the table (cheap, additive).
