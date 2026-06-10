# Lane experience: `kai doc` command + `kai --help` fix (2026-06-10)

Goal: build the human consumer for the `#[doc(...)]` documentation that
already lives across stdlib (issue #681 / PR #768, coverage audit #774),
plus fix the `bin/kai --help` regression that printed
`line 587: c: command not found`. CLAUDE.md listed `doc` among the
binary's subcommands — aspirational drift; it had never been built.

## Scope as planned vs as shipped

Planned: three views (`kai doc`, `kai doc <module>`,
`kai doc <module>.<symbol>`) built on the existing extractor, plus the
`--help` fix and a wrapper smoke test.

Shipped exactly that, plus one JSON extension the brief flagged as
legitimate-if-needed:

- **`bin/kai`**: `cmd_doc` (three views), `doc_resolve_module`
  (name/path → stdlib file), a small CommonMark→TTY renderer
  (`doc_render_body`), and three awk field extractors
  (`doc_field` / `doc_module_doc` / `doc_names`) that parse the
  single-line `--doc-json` stream honouring `\"` escapes.
- **`stage2/compiler/doc_attr.kai`**: extended `DocItem` with a `sig`
  field and a leaf-local signature renderer (`doc_sig` + `doc_te` /
  `doc_row` / `doc_label`). For a `fn` it renders
  `(p: T, ...) -> R / Eff`; other kinds carry `sig: null`.
- **`tools/test-doc.sh`** wired into `tier1` next to `test-info`.
- **`examples/doc/doc_json_basic.doc.expected`** regenerated for the
  new `sig` field (the byte-exact `test-doc-attr` golden).

## The `--help` bug

`./bin/kai --help` printed:

```
kai: error: missing input file (see 'kai build --help')
kai: error: missing input file (see 'kai run --help')
./bin/kai: line 587: c: command not found
./bin/kai: line 587: native: command not found
./bin/kai: line 587: llvm: command not found
```

Root cause: the `KAI_BACKEND` paragraph added to the `usage()` heredoc
during the #807 wiring used **unescaped** Markdown backticks inside an
unquoted `cat <<EOF`. The shell evaluated `` `kai build` ``,
`` `kai run` ``, and `` `c|native|llvm` `` as command substitutions —
so the heredoc ran three subcommands while expanding. The reported line
(587) is the `cat <<EOF` line, not the offending text, which is what
made it read like a `resolve_backend` problem. The sibling `usage_*`
heredocs already escape their backticks (`` \` ``); `usage()` was the
lone straggler. Fix: escape the six backticks. The smoke test now
asserts `kai --help` leaks neither `command not found` nor
`missing input file`.

## Design decisions

- **Build on `--doc-json`, not a new extractor.** The `MDocJson` mode
  (issue #681) already dumps a module's `module_doc` + per-item docs as
  one JSON object. `kai doc <module>` is `kaic2 --doc-json
  <stdlib-file>` + render; no parser duplicated. `kai info builtins`
  was the precedent for invoking the extractor from the wrapper and
  parsing with awk.

- **Extend the JSON with `sig` rather than re-deriving the signature in
  the wrapper.** The doc JSON carried `kind/name/line/pub/doc` but not
  the parameter/return types — and `kai doc string.split` is far more
  useful showing `(s: String, sep: String) -> [String]`. The renderer
  mirrors `te_to_string` / `fn_detail` (driver.kai) but lives in
  `doc_attr.kai` so the module stays a **leaf** (parse.kai + infer.kai
  import it; it must not import driver.kai). ~45 LOC, kept the file at
  A (km 92.8, cogcom avg 2.4 / max 7).

- **awk JSON parsing, zero new deps.** `bin/kai` is `#!/bin/sh` with
  `set -eu`; the extractors split the single-line stream on the stable
  `{"kind": ` prefix and scan string values respecting `\"` escapes.
  CommonMark rendering is a deliberate TTY subset (ATX headers, fenced
  code, bullet lists) — full CommonMark is out of scope.

- **Module resolution by basename, top-level wins.** `string` →
  `core/string.kai`, `stream` → `stream.kai`, `map` →
  `collections/map.kai`. Exact relative paths (`core/string`, `fs/path`)
  are honoured first so the ambiguous `path` (top-level `path.kai` vs
  `fs/path.kai`) stays addressable; bare `path` resolves to the
  top-level file.

## Structural surprises

- **awk `index` returns the position of the prefix's first char**, so
  the value starts at `index(s, "\"name\": \"") + 9` (the prefix is 9
  bytes), not `+ 8`. The off-by-one made `scan_str` hit the opening
  quote and return empty — every symbol looked "not found" until fixed.
  Cheap lesson, but it cost the first end-to-end run.

- **`args == []` does not compare empty `[TypeExpr]` lists** the way
  list patterns do. `te_to_string` in driver.kai uses
  `list_length(args) == 0`; my first cut used `args == []` and rendered
  `String[]` for the bare `String`. Switched to `list_length`. (List
  *patterns* like `RClosed([])` work fine — it's the `==` operator on
  lists that doesn't.)

## Fixtures + coverage

- `tools/test-doc.sh` (tier1): `--help` cleanliness (the #807
  regression), all three doc views, basename + relative-path
  resolution, the `sig` field in `--doc-json`, and both error paths
  (bad symbol / bad module exit non-zero with an actionable hint).
- `examples/doc/doc_json_basic.doc.expected` regenerated; the existing
  byte-exact `test-doc-attr` golden + `holes-json` + `builtins-doc-json`
  sub-checks all still pass with the wider schema.

Coverage gap: the near-miss hint is substring-based (`spl` → `split`,
`to` → `to_int to_upper to_lower`), so a transposition/substitution typo
(`splid`) gets only the "run `kai doc <module>`" fallback, not a
suggestion. Edit-distance was out of the cheap budget; documented here.

## Follow-ups

- `kai doc` with no args spawns one `kaic2 --doc-json` per stdlib
  module (~3.6 s for ~57 modules). Fine for an exploratory command; a
  single batched dump mode (`--doc-json-all`) would cut it to one
  process if it ever feels slow.
- Some modules (`collections/hashset`, `crypto/mac`, `math/complex`,
  `math/numeric`) show an empty module synopsis — they carry no file-top
  `#[doc]` yet. That is real coverage data, tracked in the #774 audit,
  surfaced faithfully rather than papered over.
- Local-package symbols are not yet resolvable (`kai doc` reads stdlib
  only). The wrapper already knows the cwd package layout via
  `kai.toml`; pointing `--doc-json` at a package's modules is a small
  follow-up once a use-case appears.
