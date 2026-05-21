# Lane experience — `kai info` Capa 1 (man/info-style topic pages)

**Lane shipped:** 2026-05-20. Single feat(cli) commit; no compiler edits.
**Trigger:** Mid-session, the assistant alucinated kaikai surface forms
(operator sections `(+ 32.0<F>)` and Haskell-style `\x -> body`) when
suggesting a more elegant rewrite of a unit-of-measure demo. The user
identified the pattern as a systemic gap in the LLM-friendliness
strategic bet (Tier 3 #8): the language has no canonical, version-
correct, machine-discoverable reference that an agent can consult
before claiming a form exists.

## Scope-as-planned vs scope-as-shipped

**Planned:** `kai syntax` cheat-sheet subcommand (single page). Source
in `docs/syntax-quickref.md`. Distributed via release tarball so brew
ships it automatically.

**Shipped:** `kai info` umbrella subcommand modelled on Python's
`pydoc` / GNU `info`:

- `kai info` — list 12 curated topics.
- `kai info <topic>` — print the topic page.
- `kai info <topic> --json` — JSON-structured output (sections as keys).
- `kai info --list` — names only, one per line (for shell automation).
- `kai info -k <keyword>` — search topic names + NAME blurbs.

Topics shipped: `syntax`, `effects`, `fibers`, `actors`, `match`,
`loop`, `pipes`, `protocols`, `units`, `packages`, `testing`, `holes`.

The cheat-sheet is one topic among many (`kai info syntax`) instead of
a standalone command. This matches the `info` Unix lineage the user
explicitly named and leaves room for layered topics without a second
top-level command.

## Decisions and alternatives

### Format: man-page-style markdown, ALL-CAPS section headers

Considered:
- Plain markdown with `##` headers — would require a markdown parser
  in shell for `--json`.
- A custom YAML / TOML schema — overhead for both authors and readers.
- Embedded asciidoc — too heavy a dep.

Chose **convention-over-format**: each page is plain text whose
section headers are ALL-CAPS lines in column 0 (NAME, SYNOPSIS,
DESCRIPTION, EXAMPLES, NOT IN KAIKAI, SEE ALSO, plus topic-specific
ones). awk reads them with a one-line regex (`/^[A-Z][A-Z ]*[A-Z]$/`).
JSON conversion is ~30 lines of awk with manual control-char escaping
(newlines must become `\n` for valid JSON). No dependencies beyond
POSIX shell + awk; python3 only required for the smoke test's JSON
validation.

### Layout for installed mode

Reused the existing dev-vs-installed branch in `bin/kai`. New env
override `KAIKAI_INFO_PATH` mirrors `KAI_STDLIB` / `KAI_INCLUDE`. Dev
reads `docs/info/`; installed reads `share/kaikai/info/`.
`scripts/build-release.sh` copies the directory wholesale at packaging
time. The Homebrew tap formula bumps only version+url+sha, so the
tarball delivery means brew users get info pages automatically with
zero formula churn.

### Required Tier 1 surface only — defer Capa 2 / Capa 3

The user expanded the design to three layers during the session:

- **Capa 1 (this lane):** curated topics, hand-authored markdown.
- **Capa 2 (deferred):** stdlib + user-package introspection via
  doc-comments extracted from `.kai` sources (`kai info list.map`).
- **Capa 3 (deferred):** package help for the current `kai.toml`
  package and its declared deps.

Capa 2 was deferred because it requires a load-bearing decision: doc-
comment surface syntax (`///` vs `#|` vs `#:` etc.) is a Tier 1 #4
stability commitment; shipping it without a written spec quietly
constrains every future Hanga Roa surface change. Deferring also lets
Capa 1 land immediately and prove the discovery shape works.

## Surprises

### The audit-against-reality pass caught alucinations in my own info pages

Most consequential discovery. After writing the 12 topic pages, the
user said "revisa la coherencia de los info que sean reales y no
alucinaciones". A grep pass against `stdlib/`, `demos/`, `examples/`,
and the compiler tokens turned up several mistakes I had written into
the very document meant to prevent that class of mistake:

| Page | Bug | Reality |
|---|---|---|
| syntax.md | claimed `for x in xs { body }` is valid | `for` is reserved only for `impl Proto for T` |
| syntax.md | NOT IN KAIKAI: "no `foo.bar()` method calls" | UFCS is real (`r.f(x).g(y)` chains left-assoc, issue #205) |
| syntax.md | omitted n-tuples `(a, b, c)` | sugar for Pair/Triple/Quad (issue #154) |
| syntax.md | omitted `expr!` postfix | real Option/Result propagation (m7e §13) |
| syntax.md | omitted record punning `{x, y}` | real (m7d §10) |
| syntax.md | omitted pipe placeholder `|> f(_, b)` | real (m7d §21) |
| syntax.md | omitted as-pattern `name @ pat` | real (m7d §14) |
| syntax.md | omitted `++` operator | TkPlusPlus, used in `demos/quicksort/main.kai` |
| fibers.md | called `Cancel.check()` | the only op is `Cancel.raise()`; cleanup runs via `handle ... with Cancel { raise(_) -> ... }` |
| actors.md | `Actor[Msg].send(...)` call syntax | real code writes `Actor.send(...)`; the `[Msg]` parameter lives in the handler binding |
| protocols.md | `fn show(self: Self)` op decl | op decls in `protocol { ... }` omit `fn`; stdlib uses `x: Self` not `self: Self` |
| protocols.md | listed Show/Eq/Ord/Hash/Serialize/BinSerialize only | also Default and Add/Sub/Mul/Div/Rem[a] |
| packages.md | omitted `import name.{a, b}` | real (`examples/fmt/imports.input.kai`) |

The auditing methodology (grep stdlib + demos + examples + stage2/compiler.kai
tokens; cross-check against `docs/grammar.md` and `docs/design.md`) is
the same one CLAUDE.md now instructs LLM agents to perform before
claiming a form exists. If an LLM uses the cheat-sheet to verify
*before writing code*, the cheat-sheet must itself be verified
*against the running compiler* — not against the LLM's prior beliefs.

### grep-against-docs is not enough; compile a real program

After the doc-grep audit landed corrections, the user added "prueba
con código no solo leyendo docs". I wrote `/tmp/info_audit.kai`
exercising every form `kai info syntax` claims is real, and compiled
it with kaic2. The compiler caught one bug the doc-grep had missed:

| Bug | Origin | Where it was wrong |
|---|---|---|
| `var x` reads need `@x`; bare `x` is rejected with a typer error | `loop.md` example `while { i < n } { ... i := i + 1; ... }` had bare reads | Fixed both `loop.md` and added the rule to `syntax.md` |

Three other compile errors in the audit file itself turned out to be
audit bugs (`char_to_string` doesn't exist; placeholder `.` only works
in arg position, not as a let-RHS lambda; my `range_step` fn collided
with the runtime's `kai_range_step`). The info pages did not affirm
any of these, so no further corrections were needed.

Lesson: **a doc whose audit method is "grep" can be self-consistent
but still wrong about semantics the grammar permits but the typer
forbids.** For Capa 2 (doc-comments + stdlib introspection), the
audit pipeline should compile a generated sample for every claim.

### The pre-existing `examples/sugars/` directory was a richer ground-truth source than `docs/`

Once I discovered `examples/sugars/*.kai`, the audit accelerated.
Each sugar / surface feature has a focused fixture (`m7e_13_bang_*`,
`m7d_10_record_pun_*`, `n_tuples_basic.kai`, etc.). These are stronger
evidence than docs because they must compile clean to pass tier1.
Future Capa 1 amendments should grep `examples/sugars/` first; primary
docs second; design proposals last.

### CHANGELOG / VERSION / cz are downstream

This lane adds a user-visible surface command (`kai info`) and ships
new docs that travel with the binary. By the project's commitizen
conventions, that is a `feat(cli)` commit and will mechanically trigger
a MINOR bump on the next `cz bump --yes`. The agent does not write
VERSION or CHANGELOG.

## Fixtures and coverage

`tools/test-info.sh` covers:
- `kai info` exits 0 with non-empty topic list.
- `kai info --list` matches `docs/info/*.md` on disk.
- Every topic produces non-empty output with a `NAME` section.
- Every topic produces valid JSON under `--json` (python3 validation).
- The `syntax` topic must exist (CLAUDE.md cites it).

Wired into `tier1` (target `test-info`) — fast (<1s), no kaic2 needed,
catches deletions, dispatcher regressions, and awk-escape drift.

## Cost — planned vs real

Estimated 1 session. Real: ~1 session (this one), with the audit pass
adding ~30 minutes of `grep`-and-corrections at the end. The audit was
not in the original plan but turned out to be load-bearing — without
it, the cheat-sheet itself would have shipped with at least 8
documented alucinations.

## Follow-ups

1. **Issue: Capa 2 — doc-comments + stdlib introspection.** Spec doc-
   comment syntax first (Tier 1 #4 edition-level decision), then
   compiler-side doc-extraction, then `kai info <module>` /
   `kai info <fn>` shell wrapper. Pre-1.0 target.

2. **Issue: Capa 3 — `kai info` for the current package.** Depends on
   Capa 2. `kai info` invoked in a directory with `kai.toml` shows
   the package description, entry point, and lists public symbols.
   `kai info <dep>` for any declared dep. Post-Capa-2.

3. **Issue: Topic seed expansion.** 12 topics shipped. Likely
   omissions for Hanga Roa surface: `refinements` (m12.6, real),
   `holes` could be split into `holes` (term) + `effect-holes`
   (`row_hole_basic.kai` fixture), `ffi` (`extern "C" fn`), `main`
   (effect inference + auto-installed effects).

4. **Issue: `kai info -k` should also search SYNOPSIS bodies, not
   just NAME blurbs.** Currently matches only topic names + NAME
   lines. Useful for "find the topic that mentions `nursery`".

5. **Convention to enforce: every new surface feature in a lane MUST
   either update `docs/info/syntax.md` (and any other affected topic)
   or open an issue noting the omission.** Add to CLAUDE.md "Doc
   discipline" section. Without this, `kai info syntax` drifts from
   reality the same way `docs/stdlib-layout.md` did (issue #367).

These follow-ups are listed for next sessions; opening any of them
requires Eduardo's explicit authorization (per memory
`feedback_kaikai_never_open_issues_unauthorised.md`).
