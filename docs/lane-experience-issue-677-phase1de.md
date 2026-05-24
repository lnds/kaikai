# Lane experience — issue #677 Phase 1d/1e + module tests (2026-05-24)

## Scope as planned

Continue the per-module extraction of `stage2/main.kai` started in
Phase 1a/b/c. The handoff named two next modules in dependency
order: `diag` (Phase 1d) and `lex` (Phase 1e). The lane was also
asked to add per-module tests for every extracted module, not just
the `chars` pilot — the user flagged this gap when reviewing
Phase 1d mid-flight.

## Scope as shipped

Three commits over one overnight session:

1. **Phase 1d (`da8177a`)** — `compiler/diag.kai`. Extracts the
   source-snippet rendering machinery, the structured diagnostic
   surface (Severity / RelatedInfo / Diagnostic plus
   accumulators), and the Levenshtein-based "did you mean?"
   helper. Narrower than the original "diagnostics" section in
   `compiler.kai`: `LeakLoc`, `MLeakRecord`, and
   `report_mono_leaks` stay near the monomorphiser code that
   produces and consumes them — they only lived in the
   diagnostics block for stage-1 type-resolution ordering, not
   semantic kinship.

2. **Phase 1d follow-up (`0f6eade`)** — `#[doc(...)]` pass on the
   21 pub symbols of `diag.kai`. Single-line form only — kaic1's
   lexer still rejects triple-quoted strings while the bundle
   path is the bootstrap fall-through.

3. **Phase 1e (`42a3497`)** — `compiler/lex.kai` (~900 LOC) +
   module tests for both diag and lex. Moves TokKind / Token /
   Lexer / LexStep / StrResult plus the 36 `lex_*` helpers and
   the `tokenize` entry point. The token-dump and error-scanning
   bridges (tok_text, dump_token, report_one_error, …) stay in
   main.kai for now — they compose lex with diag and the driver,
   and they'll move into a sibling module when the driver
   extracts.

## Design decisions and alternatives considered

### Extraction order discipline

`asu` and `linus` had flagged "cycles between modules" as the
single biggest Phase 1 risk. The plan handed off was:

```
chars → diag → lex → ast → intervals → parse → resolve →
modules → desugar → infer → protocols → emit_c / emit_llvm →
fmt → driver
```

The actual order shipped so far is `chars → diag → lex` — exactly
what the plan named. None of the three have surfaced cross-module
cycles. lex depends only on chars and stdlib core; diag depends
only on stdlib core. The next module (`ast`) is the first that
will hit a real cycle, between `Ty` (in the infer block) and
`Expr` (in the ast block) — flagged in the handoff for this
session as a stop-and-decide point.

### Extract-without-pub vs mark-everything-pub

Phase 1d started with the "extract bodies first, mark `pub`
when the typer complains" probe. That worked for symbols the
typer flagged one round at a time, but it cost two iterations:
first the 20 fns / types main.kai explicitly named, then
`abs_int` (a helper called from main.kai that the first scan
missed). For Phase 1e (`lex.kai`, 42 top-level decls) the lane
flipped to mass-marking everything `pub` up front because the
cross-module call graph from lex is dense enough that the
per-iteration probe would have needed ~4 rounds. The follow-up
audit that un-pubs lex_* helpers that turn out to be lexer-
internal is left for a later cleanup lane.

The lesson: extract-then-mark works for ~10 pub names; ~40+
pub names is more friction than value.

### `#[doc("...")]` shape — single-line only

The Phase 1c precedent applied multi-paragraph `#[doc(""" ...
""")]` to chars.kai, then had to back off to plain `#` comments
when the kaic1 lexer rejected triple-quoted strings inside the
bundle path. Phase 1d/1e applied the lesson up front: only
single-line `#[doc("...")]` for the per-symbol docs; module-
level multi-paragraph context stays in the `#` header comments
above each section. The lexer can ingest single-line attribute
strings cleanly, backticks and all (verified with a minimal
repro before applying).

### Test orchestration shape

`kai info testing` documents the sibling `tests/` directory
convention. Each test file imports the module under test and
exercises its public surface. The runner is `kai test stage2/.`.

Wall-clock cost: ~75 s per `kai test stage2/.` invocation
because the package compile is dominated by the 70 k-line
main.kai. tier 1 wiring deferred until the cache layer (#452)
or interface persistence (#677 Phase 2) drops that to <1 s
per test file.

Test count after this lane: 7 (chars) + 17 (diag) + 19 (lex)
= 43 module tests, all green.

## Structural surprises the brief did not anticipate

### Two kaikai syntax sharp edges the tests exposed

The new tests hit two `kai info`-documented-but-easy-to-forget
constraints that the chars pilot did not exercise:

1. **`match` arm bodies cannot be bare statements like `assert
   x`.** The arm body must be an expression; a statement-shaped
   form needs `{ ... }` wrapping. The parser error is the
   generic `expected expression`. Fixed by sed wrapping every
   `-> assert ...` to `-> { assert ... }`. 14 sites in
   tests/test_diag.kai.

2. **Record patterns inside `match` use field-position syntax
   (`{ kind, ... }`) — NOT type-prefixed (`lex.Token { kind }`).**
   The latter is record-literal syntax (per `kai info match`:
   "A record LITERAL in expression position needs the type
   prefix … Only patterns and `let`-destructure allow the
   prefix-less form."). Using the prefix form in pattern
   position compiles but panics at runtime with
   `non-exhaustive match`. Fixed by switching to field access
   `t.kind` for the projection cases, which is cleaner than
   record-pattern destructuring anyway.

Both shape constraints are documented; neither is intuitive
the first time you write a test file. Captured in this retro
so the next module extraction can skip the round-trip.

### `lex.kai` needs its own `import compiler.chars`

When `kai test` compiles a `tests/test_lex.kai` file together
with `lex.kai`, the resolver looks at the modules touched by
the test (not just main.kai's module set). `lex.kai` calls
`ch_is_*` from compiler.chars but had no `import compiler.chars`
line of its own — main.kai's import was sufficient for the full
package compile but not for the test-driven compile of lex.kai
in isolation. Fixed by adding the import line to lex.kai
directly. selfhost stays green because kaic1 parses the import
and ignores it, kaic2 resolves it.

The general rule going forward: every per-module file imports
exactly the modules its body calls, not relying on main.kai's
import set. This matches Phase G of the issue's source-of-
truth flip and makes each module independently compilable.

### `$` is `TkDollar`, not `TkError`

A first-draft lex test asserted "an unexpected ASCII character
`$` lexes as TkError". Empirically `$` is `TkDollar` — reserved
in 2026-05-13 for compiler intrinsics like
`$extern_handler("c_symbol")`. The test was wrong, the lexer
was right. Fixed by replacing the case with `dollar lexes as
TkDollar` (positive coverage) plus a real error case
(`0x` with no following hex digits, which is the documented
TkError shape). Generally a good reminder that the tests
shouldn't bake in assumptions about which inputs are
"obviously" errors — derive them from the lexer's actual
error catalogue.

## Fixtures added and coverage gaps

- `stage2/tests/test_diag.kai` (17 tests): spaces / nth_line /
  severity_to_string / mk_diag / ri_note / ri_help /
  push_diag / finalize_diags / levenshtein / closest_name /
  abs_int.
- `stage2/tests/test_lex.kai` (19 tests): tokenize over single-
  token fixtures covering each TokKind family — integers,
  reals, imaginaries, strings, chars, idents, underscore,
  keywords, true/false, hash-open, dollar, malformed-hex,
  line/col bookkeeping.

Coverage gaps left intentionally:

- **diag effect-heavy helpers** (`emit_*`, `diag_error`,
  `diag_note`, `diag_help`, `diag_error_from_src`) — would
  need a stderr-capture harness. The pure helpers carry the
  algorithmic load; the stderr-emitters are thin wrappers.
- **lex error-path tokens beyond malformed-hex** — the
  negative-fixture harness wired into tier1 (`examples/
  negative/`) carries the systematic coverage for those.
- **No property checks (`check "..." with x: T { ... }`) yet**
  on any module. `kai info testing` confirms the syntax
  exists; natural candidates for follow-up: `check
  "ch_is_digit ⇒ ch_is_alnum" with c: Char`, `check
  "levenshtein(a, a) == 0" with s: String`.
- **No benches**. `kai info testing` documents `bench
  "name" { body }` for regression-of-performance probes. User
  flagged this as a future direction for compiler perf
  baselines (parse / lex / monomorph timing) — a separate
  lane.

## Real cost vs estimate

The handoff coming into this session estimated Phase 1d at
"a few hours; lex is more". Real cost:

- Phase 1d (extraction + pub markers + doc pass): ~2 hours
  across two commits, mostly waiting on selfhost + tier1.
- Phase 1e (extraction + mass-pub + import-add + tests): ~3
  hours, half spent on the two syntax sharp edges discovered
  while writing the test files.

The estimate was right. The test-writing piece was new in this
lane and cost roughly 60-90 minutes of the Phase 1e time —
mostly the two syntax round-trips, which the next module
extraction will not pay.

## Follow-ups for the next lanes

- **Stop point for next session**: extracting `ast.kai` hits
  the `Ty ↔ Expr` cycle. `Ty` (line ~24030 of main.kai,
  inside the infer block) carries `TyDimT(Ty, UnitExprT)`
  where `UnitExprT` is in the AST section, and `TyRefineT(Ty,
  Expr)` where `Expr` is in the AST section — and `Expr`
  carries `ty: Option[Ty]`. The handoff explicitly stopped
  the autonomous loop here because a one-module-fits-all
  `compiler/types.kai` or a forward-declaration trick needs
  a human decision.
- **Un-pub audit for `lex_*` helpers**. The mass-pub on
  extraction was a velocity choice. A later lane should
  un-pub every `lex_*` that only `tokenize` calls — the
  public-surface principle is "expose what callers need, not
  what was easy to ship."
- **Property checks for the pure helpers** in chars / diag /
  lex. `kai info testing` documents `check "..." with x: T {
  ... }`; small lane, high signal.
- **Bench harness for compiler perf baselines** (parse / lex /
  monomorph timing). User flagged the direction; separate
  lane.
- **Tier 1 wiring for `kai test stage2/.`** once #452 or #677
  Phase 2 land. Today the 75 s package-compile cost makes
  per-test-file tier integration expensive; that drops to
  <1 s once incremental rebuild ships.
- **`stdlib/core/char.kai` migration**, deferred from Phase
  1c. compiler/chars.kai has its own copies of is_digit /
  is_lower / etc. because (a) `ch_is_space` includes `\r`
  for CRLF and stdlib's doesn't, (b) `ch_is_hex_digit` has
  no stdlib equivalent, (c) the stage-1 bootstrap predates
  the module resolver. None of those reasons changed; the
  migration is conceivable but not yet motivated.
- **Issue #677 status update on GitHub**. The issue body lists
  the per-file plan; commenting "Phase 1a–1e shipped: chars +
  diag + lex (+ tests) — see commits 5a1ba77 / cd1312e /
  35e5987 / da8177a / 0f6eade / 42a3497" would help future
  readers locate the lane. Requires user authorisation
  (gh write action) — flagged here, not run autonomously.
