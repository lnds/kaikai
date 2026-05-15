# Lane experience report — issue #608 (`#[...]` attribute syntax migration)

## Scope as planned

Migrate kaikai's annotation form from hardcoded reserved words
(`#derive(...)`, `#unstable`) to bracket-delimited form
(`#[derive(...)]`, `#[unstable]`). Reserve `//` and `/*` as
`TkError`-with-guidance so they cannot accidentally be repurposed by
future features. Single comment form (`#`) preserved. Forward-compat
for future `#[doc(...)]`, `#[deprecated]`, `#[cfg]`, custom attributes
without compiler edits. Selfhost byte-identical. Adelantado pre-Anga
Roa per asu recommendation (2026-05-15) — `#unstable` had shipped that
same day in v0.64.0 with no external consumers, so the migration
window was zero-cost public.

## Scope as shipped

Matches the plan exactly. The lane delivered:

- **Lexer**: removed `TkDerive` / `TkUnstable` and the
  `lex_match_word_at(..., "derive"/"unstable")` reserved-word scan.
  Added `TkHashOpen` for `#[`. `//` now produces a sharper `TkError`
  (the message previously cited #315 only — now points at `#` as the
  kaikai comment form). New `/*` `TkError` branch.
- **Parser**: replaced two `TkDerive` / `TkUnstable` arms in
  `parse_decl` with one `TkHashOpen` arm dispatching to
  `parse_attribute_decl`, which reads the attribute identifier and
  routes to `parse_attr_derive_body`, `parse_attr_unstable_body`, or
  the forward-compat `parse_attr_unknown_body`.
- **Transition aid**: bare `#derive` and bare `#unstable` (legacy
  pre-#608 surface) lex to `TkError` with a message pointing at the
  new `#[...]` form. No silent parse failure.
- **Sed migration**: 54 `.kai` fixtures across `stdlib/`, `examples/`,
  `tools/`, `kaikai-book` (none — book absent in repo), 3 user-facing
  docs (`docs/editions.md`, `docs/protocols.md`, `docs/grammar.md`,
  `docs/unions-design.md`).
- **6 new fixtures** in `examples/attributes/` plus a `test-attributes`
  Makefile target wired into `test:`.
- **AST unchanged**: `DDerive(names, inner, line, col)` and
  `DUnstable(inner, line, col)` survive untouched. The forward-compat
  case for unknown attributes does *not* introduce a new `DAttribute`
  node — unknown attributes parse and are dropped from the AST.

## Design decisions and alternatives considered

### 1. Drop unknown attributes vs synthesise a generic `DAttribute(name, args)` node

**Chose**: drop. Forward-compat means *the syntax parses cleanly* —
not that the AST carries the metadata yet. Adding a `DAttribute`
variant would force every downstream walker (typer, monomorphiser,
emitters, fmt) to handle it; that's churn proportional to the
walker count for zero current consumer. When `#[doc(...)]` or
`#[deprecated]` actually need AST visibility, that lane introduces the
shape it needs.

**Alternative**: keep the parsed args as a list on the wrapped decl
so a future lane reads them without re-parsing. Rejected — there's no
"wrapped decl" mechanism today for unknown attributes; building one
would mean either a new `DAttribute(name, args, inner)` wrapper or
attaching args to every existing decl variant. Both are speculative
abstraction.

### 2. Conserve legacy bare `#derive` / `#unstable` detection in `lex_skip_ws`

Considered ripping the legacy detection out entirely — without it, a
bare `#derive(Show)` line is just a comment (the `#` would start the
comment, the rest would be skipped). Rejected because: the user who
typed `#derive(Show)` then `type Foo = ...` would see a confusing
"unknown type Foo" or "expected fn" error far from the actual
mistake. The TkError transition aid keeps the diagnostic at the point
of the typo.

The detection is local to `lex_skip_ws` + `lex_punct` (a few lines
each); it can be removed in Orongo (~6 months out) when the migration
is unambiguous historical.

### 3. AST shape

The original brief specified AST unchanged. Confirmed: `DDerive` and
`DUnstable` keep their pre-#608 signatures. Only the parser path that
populates them changed.

### 4. `lex_skip_ws` forwarding rule

The pre-#608 rule: `#` keeps its position (for `lex_punct` to tokenize)
only when followed by `derive` or `unstable`. New rule: `#` keeps its
position when followed by `[`, OR by legacy `derive`/`unstable` (for
the TkError transition aid). Everything else is a comment.

The forwarding asymmetry between the lexer's pre-scan and `lex_punct`'s
post-scan is the cleanest path inside the existing single-pass
architecture. No new lex mode needed.

### 5. Selfhost gate

`stage2/compiler.kai` doesn't use `#derive` or `#unstable` on itself,
so the migration didn't have to touch the compiler's own annotation
usage. The selfhost byte-identical gate became: does the rebuilt
`kaic2b` (compiled by the migrated `kaic2`) compile `compiler.kai` to
the same C output as `kaic2c`? Yes — passed on first try after the
lexer + parser changes landed, with no AST shape divergence.

## Structural surprises the brief did not anticipate

### 1. `print` is already `println`

The brief assumed `print(s)` would not add a newline. Stage 2 stdlib's
`print` is the `Console` effect operation and **does** add a newline
(stdlib/core/io.kai shows `println` is just `print` aliased). First
attempt at `attr_derive_basic.kai` produced doubled newlines that
diverged from the golden. Fixed by dropping the explicit `\n` literals
in fixture source.

### 2. Sed multi-line eat-up

The perl substitution `s/^#unstable\s*$/#[unstable]/` — intended to
match a line that is exactly `#unstable` — silently consumed the
following newline in a number of files (perl `-pe` with `\s*$` was
matching greedily across lines on some files, leaving lines like
`#[unstable]pub fn foo`). Fixed with a follow-up substitution
`s/(#\[unstable\])(pub|type|fn)/$1\n$2/g`. Lesson: when running
multi-line-sensitive perl across many files, always grep-verify the
output shape before declaring the migration done.

### 3. `.err.expected` files are fixed-string `grep -F` needles, not full matches

`stage2/Makefile`'s `test-stdlib` target uses `grep -qF "$$line"` —
substring match. When my first migration pass changed `#derive(...)` to
`#[derive(...)` (no closing `]`), the test silently still passed for
files whose needle had no closing backtick — but failed for the ones
that did (the needle was `` cannot `#derive(BinSerialize)` for
`Palette` `` with a `` ` `` immediately after `)`, and the new output
inserted a `]` between `)` and `` ` ``, breaking the substring match).
Had to do a second sweep to insert `]` into the needle.

### 4. `issue_315_no_double_slash.err.expected` was load-bearing

The pre-#608 error message was the full citation of the needle. After
the message rewrite, the test failed until the expected file was
updated. Caught in test-stdlib on first run; updated.

## Fixtures added and coverage gaps

Six fixtures in `examples/attributes/`:

| Fixture | Kind | Acceptance |
|---------|------|-----------|
| `attr_derive_basic.kai` | positive | `#[derive(Show, Eq)]` on a record; round-trips `show` + `eq` |
| `attr_unstable_basic.kai` | positive | `#[unstable] pub fn` parses, behaves identically to legacy |
| `attr_unknown_ignored.kai` | positive | `#[doc(...)]`, `#[deprecated]`, `#[cfg(...)]` all parse and are silently dropped |
| `attr_double_slash_error.kai` | negative | `//` produces TkError with the new guidance message |
| `attr_block_comment_error.kai` | negative | `/*` produces TkError citing Elixir precedent |
| `attr_legacy_deprecated.kai` | negative | bare `#derive(Show)` (pre-#608) produces the deprecation TkError |

Wired into `stage2/Makefile` as `test-attributes`, added to the `test:`
aggregator. Positive fixtures use `.out.expected` (diff vs run output);
negative fixtures use `.err.expected` (substring grep against stderr).

**Coverage gaps**:

- No fixture exercises `#[derive(Show, Eq)]` on a sum type — already
  covered by the migrated `examples/stdlib/binserialize_derive_sum_*`
  fixtures.
- No fixture exercises `#[unstable]` on `pub effect` or `pub protocol`
  — same gap as #602's lane retro called out; not regressed by #608.
- No fixture exercises `#[derive(...)]` AND `#[unstable]` stacked on
  the same decl. The two annotations cannot stack today (parser
  consumes one attribute per decl); future lane if/when stacking is
  needed.

## Real cost vs estimate

Brief estimated 1–2 sessions. Real cost: one session, roughly two
hours interactive, dominated by the sed verification sweep (three
iterations to converge on the right multi-line behaviour) rather than
the compiler change itself. The compiler diff is ~80 LOC across
lexer + parser; the migration touched 57 files across stdlib +
examples + docs. The selfhost gate passed on the first build of
`kaic2`.

## Follow-ups

### Anga Roa (next edition cutover)

- **Remove the legacy transition aid**. Once Anga Roa is the only
  edition with `#[...]` syntax, the `TkError` arms in `lex_punct` for
  bare `#derive` / `#unstable` become dead weight. Drop them and let
  the bare form lex as a comment again. Net: ~20 LOC removed,
  cleaner lexer, simpler `lex_skip_ws` (no `derive`/`unstable`
  word-match needed at all).

### Orongo or later

- **Real `#[doc(...)]`**. The forward-compat path means `#[doc(...)]`
  parses today and is dropped. The actual doc-attribute lane needs to
  carry the doc string through the AST (`DAttribute(name, args,
  inner)` or per-decl `doc: Option[String]`) and wire it into the LSP
  hover protocol + generated documentation. Out of scope here.
- **`#[deprecated]`**. Similar shape: parser already accepts the
  syntax; the lane attaches a deprecation flag to the wrapped decl and
  emits a warning at every call site. Crosses paths with the
  `#[unstable]` enforcement machinery — likely a shared substrate.
- **`#[cfg(target = "native", feature = "x")]`** — conditional
  compilation. Requires the compiler to read a configuration source
  (kai.toml + target triple) and prune decls at parse or resolve time.
  Much larger lane; the syntax is settled but the semantics aren't.
- **User-defined attributes**. If/when this lands, the
  `parse_attr_unknown_body` path becomes the user-attribute path —
  with validation against a registry of declared attributes.
