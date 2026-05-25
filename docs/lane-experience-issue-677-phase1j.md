# Lane experience — issue #677 Phase 1j (kai fmt extraction)

Second of the "easy three" follow-ups (resolve / fmt / driver) after
the parser extraction (Phase 1h, PR #684) and the resolver extraction
(Phase 1i, PR #685). This lane pulls the `kai fmt` canonical
pretty-printer out of `main.kai` into `compiler/fmt.kai`. A sibling
lane (driver-extract, Phase 1k) runs in parallel on the `cli + driver`
block immediately below — disjoint line range, trivial merge conflicts
on `main.kai`'s import list and the Makefile `BUNDLE_SRCS` only.

## Scope as planned

Move the `# kai fmt — pretty printer (Tongariki MVP)` section
(~1764 LOC) to `compiler/fmt.kai`, add `#[doc]` on every pub, add
`stage2/tests/test_fmt.kai` with ≥5 unit tests + ≥2 property checks,
wire the Makefile + main.kai import, and ship a retro. Constraint:
touch nothing outside `compiler/fmt.kai` + Makefile + main.kai imports
+ the extraction itself.

## Scope as shipped

As planned. The section was a near-perfectly cohesive contiguous block
(main.kai 50030–51793 after rebasing onto `origin/main` 7c8f893):
3 local types (`Cmt`, `FmtSt`, `CmtScan`, `Loc`), the `collect_comments`
re-scanner, and the full `fmt_*` AST walker. 107 top-level decls
total. Extracted body is 1722 LOC; the new module with its header +
imports is 1820 LOC. `main.kai` dropped from 59244 → 57480 LOC.

Tests: 15 unit + 3 property checks, all green. Selfhost determinism
held (kaic2b.c == kaic2c.c).

## Cross-section analysis (the load-bearing finding)

The callee-graph scan (the discipline Phase 1h/1i established) found
the fmt section is one of the cleanest extractions yet: of 107 decls,
**only 2 have external callers** — `collect_comments` and
`fmt_program`, both called from the `cli + driver` block (the sibling
lane's territory). Everything else is private to the formatter.

But the scan also surfaced **two glue dependencies pointing the wrong
way**: `fmt_tparam_decode` called `tp_kind_is_unit` and `tp_strip_kind`,
both of which live in `main.kai` (line ~18343) with *typer-side* call
sites (lines 16916, 17104, 18332, 29311 …). A module cannot import from
`main.kai` (modules import downward; `main.kai` imports them), so a
naive copy of the line range would have failed to compile in the
modular `kaic2` build.

Three options were weighed:

1. **Move the two `tp_*` into `compiler/parse.kai`** (where the rest of
   the tparam-kind family — `tp_strip_bounds`, `tp_bounds_of`,
   `tp_has_bounds` — already lives). Correct long-term home, but it
   touches `parse.kai` (outside this lane's allowed file set) and would
   orphan `tp_kind_of` / `tp_subst_lookup`, which also call them from
   `main.kai`. Rejected: scope creep + risk of colliding with the
   driver lane.
2. **Back-import from main.kai.** Impossible — would create an import
   cycle.
3. **Local mirrors.** Re-derive the two predicates inside `fmt.kai`
   from `tp_unit_suffix()` (which *is* `pub` in `compiler/ast.kai` and
   already imported). Chosen.

`fmt_tp_is_unit` / `fmt_tp_strip_unit` are the result — ~10 LOC of
duplicated logic, each carrying a comment explaining the duplication
and pointing at the consolidation follow-up. The `string_ends_with`
call in the original (also a `main.kai` typer-side helper) was inlined
with `string_slice` for the same reason. This is the right trade for a
lane whose mandate is "do not touch parse.kai / main.kai internals":
the duplication is visible, documented, and collapses to one import
the day someone moves the tparam-kind family into `parse.kai`.

Had I extracted the literal line range without the callee graph, the
modular build would have failed on three unresolved names
(`tp_kind_is_unit`, `tp_strip_kind`, `string_ends_with`) — exactly the
failure mode the cross-section discipline exists to catch.

## Design decisions

- **Public surface: 3 decls.** `collect_comments` + `fmt_program`
  (the two with external callers) and the `Cmt` *type* (flows through
  both pub signatures). `FmtSt`, `CmtScan`, `Loc` and all 100+ `fmt_*`
  walkers stay private. Before the un-pub audit nothing was marked;
  after, exactly these 3 are pub.

- **Imports.** `compiler.util` (`cat2`, `concat_all`, `list_is_empty`),
  `compiler.chars` (`ch_is_space`), `compiler.diag` (`spaces`),
  `compiler.lex` (`Token`), `compiler.ast` (every AST type the walker
  matches on + `tp_unit_suffix`), `compiler.parse` (`tp_strip_bounds`,
  `tp_has_bounds`, `tp_bounds_of`). All prelude string/list/char
  primitives (`string_slice`, `string_join`, `char_at`, `map`, …) need
  no import.

- **Test strategy: end-to-end through the real lexer + parser.** The
  formatter only makes sense on genuine AST, so every test goes
  `tokenize → parse_program → fmt_program` (and `collect_comments` off
  the same token stream). Two assertion shapes: **round-trip** (a
  canonical source formats back to itself — the strongest single check)
  and **idempotence** (`fmt(fmt(x)) == fmt(x)` — the property a
  formatter must satisfy on any input). Module-qualified variant
  patterns (`fmt.Cmt(_, _, _, is_tr)`) work in match arms, matching the
  `ast.EInt(n)` precedent in `test_parse.kai`.

## Structural surprises

- **`if`/`else` always renders multi-line.** My first round-trip test
  for `if b { 1 } else { 2 }` failed — the formatter expands every
  brace-body across lines (`fmt_brace_body` opens `{`, newline, indent).
  This is the canonical shape, not a bug; the test was corrected to
  expect the multi-line form. A good reminder that "canonical" is
  whatever `fmt_program` emits, not what a human would hand-write
  compactly.

- **`kai fmt` is in-place destructive.** Confirmed during debugging —
  `bin/kai fmt /tmp/x.kai` rewrote the file rather than printing to
  stdout. Used a throwaway file. (Already in the global memory.)

- **Branch was one merge behind.** The lane branch started at 9d5f36c
  (pre-resolve-merge); `origin/main` was already at 7c8f893. Rebased
  before touching anything so `compiler/resolve.kai` and the correct
  `BUNDLE_SRCS` order were in place. The fmt section shifted from
  ~51024 to ~50030 after the rebase (resolve extraction removed ~1000
  lines above it).

## Fixtures / coverage

`stage2/tests/test_fmt.kai` — 15 unit tests (10 round-trip across
fn/type/import/match/let/if/pipe/test forms, 5 comment-recovery) + 3
property checks (idempotence, leading-comment recovery over N decls,
short-form round-trip). Runner: `kai test stage2/.` / `kai check`.
Tier 1 wiring deferred until #452 / #677 Phase 2 lowers per-test-file
cost (same posture as every prior module-test file).

## Cost vs estimate

Roughly on estimate. The single real decision was the `tp_*` glue
(weighed three options, chose local mirrors); the rest was mechanical
cut + header + import + un-pub audit. The one debugging round (if/else
round-trip) cost one `kai fmt` invocation.

## Follow-ups for the driver lane (Phase 1k)

- The driver lane edits the same two files (`main.kai` import list,
  Makefile `BUNDLE_SRCS`). Conflicts are trivial; integrator rebases
  one PR over the other. Module order must read:
  `util, chars, diag, lex, ast, intervals, refinements, parse,
  resolve, fmt, driver, main` (driver adds itself after fmt).
- `collect_comments` / `fmt_program` are the only fmt symbols the
  driver block calls — both now `pub` in `compiler.fmt`. The driver
  module must `import compiler.fmt`.
- `tp_kind_is_unit` / `tp_strip_kind` / `string_ends_with` were
  intentionally **left in main.kai** (typer-side callers). The driver
  block does not use them, so no action needed there. A future lane may
  consolidate the tparam-kind family into `parse.kai`, at which point
  `fmt.kai`'s two local mirrors collapse into one import.
