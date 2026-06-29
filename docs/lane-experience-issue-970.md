# Lane experience — issue #970: block-lambda pattern binder

## Scope as planned vs. as shipped

**Planned.** Accept a pattern in the parameter binder of a block lambda
(`{ (a, b) -> ... }`) so a `Pair`/tuple callback argument destructures
inline, dropping the `let (a, b) = p` boilerplate on `foreach` /
trailing-lambda callbacks over `enumerate()` and friends. Block-lambda
only; the arrow form `(a, b) =>` stays a two-parameter lambda.

**Shipped.** Exactly that, with the binder generalised for free to any
parenthesised pattern `parse_pattern` accepts — tuple arities 2–4
(`Pair`/`Triple`/`Quad`), and grouping `(pat)` over a single inner
pattern. No new pattern machinery: the binder desugars to a single-arm
`match` over a fresh binder and reuses the existing pattern lowering and
exhaustiveness check end to end.

## Design decisions

- **Discriminant = `(` then `->` after the matching `)`.** A brace block
  may legitimately open with a parenthesised expression (`{ (a + b) }`),
  so `(` alone cannot mean "pattern binder". The disambiguator is a
  balanced-paren lookahead (`skip_balanced_parens`, already in the
  parser): a pattern binder only when `->` immediately follows the
  matching `)`. The scan reports no errors, so a misfire falls through
  to the ordinary block path with zero risk to existing code. Considered
  and rejected: committing to a pattern parse first and backtracking on
  failure — heavier, and `parse_pattern` carries `Console` (it reports
  errors), so a speculative call that fails would have to suppress
  diagnostics. The pre-scan avoids that entirely.

- **Desugar to `match`, not a bespoke bind.** `{ (a,b) -> body }` lowers
  to `{ __lam_pat_L_C__ -> match __lam_pat_L_C__ { (a,b) -> body } }`.
  This buys the refutability check for free: kaikai already rejects a
  non-exhaustive `match` at compile time (`non-exhaustive match on T:
  missing V`), so a refutable lambda binder (`(Some(n))`) is rejected
  with the standard diagnostic — no new error path, no new wording to
  maintain. The fresh binder name is `__lam_pat_<line>_<col>__`, unique
  per call site by construction.

- **Block-only, arrow untouched.** The arrow path
  (`try_parse_lambda_params_loop`) discriminates on `=>` and treats
  `(a, b) =>` as two parameters; touching it would break every binary
  arrow lambda. The block path (`try_parse_lambda_block_params`) has no
  `=>` and is where the pattern binder was added. The asymmetry (block
  destructures, arrow is N-params) is the accepted price and is the form
  where destructure actually hurts (trailing/`foreach` callbacks).

## Structural surprises

- **The diagnostic is not lambda-aware.** Because the desugar routes
  through `match`, a refutable binder reports `non-exhaustive match on
  Option: missing None` rather than something like "lambda binder must
  be irrefutable". This is correct and clear, and reusing the check is
  the right trade (one source of truth, no drift), but it does not name
  the lambda. Left as is deliberately; a lambda-specific message would
  duplicate the exhaustiveness logic for cosmetic gain.

- **A bare variant binder (`{ Some(n) -> }`, no parens) is not a pattern
  binder.** It starts with an identifier, so it enters the N-name loop,
  which bails at the `(`, and the whole thing falls to `parse_block` →
  "expected expression". Pattern binders are introduced by `(`. This is
  in scope per the brief ("at least `(a, b)`") and keeps the lookahead
  cheap; widening to bare constructor binders would need a different
  discriminant and is not motivated by the `enumerate` pain point.

## Fixtures

- `examples/sugars/block_lambda_pattern_binder.kai` (+ `.out.expected`)
  — positive: tuple-pattern binder destructuring a `Pair`, plus an arrow
  `(a, b) =>` two-parameter lambda in the same file as the
  non-regression pin.
- `examples/sugars/block_lambda_pattern_refutable.kai` (+
  `.err.expected`) — negative: `(Some(n))` binder rejected with
  `non-exhaustive match on Option: missing None`.

Both ride the existing `test-sugars` gate (auto-globs
`examples/sugars/*.kai`; positives diff stdout, negatives substring-match
stderr). No harness change needed — the fixtures are self-contained
(local `Pair` type, `Stdout` effect), so they need no `--path ../stdlib`.

## Coverage gaps

- The acceptance case (`foreach(rows.enumerate()) { (row, line) -> }`,
  which needs the stdlib `enumerate`/`foreach`) is verified by hand on
  the native backend but is not a `test-sugars` fixture, because that
  harness only passes `--path ../stdlib` for `complex_literal_*`. The
  self-contained fixtures exercise the same desugar path; the stdlib
  case adds no parser coverage.

## Verification

- Native backend: acceptance case prints `0:a / 1:b / 2:c`; tuple
  arities 2 and 3 both destructure.
- `test-sugars`: 155/155 (153 pre-existing + 2 new), C backend.
- selfhost byte-id: `kaic2b.c == kaic2c.c`.
- tier0: green.
- `km score` on the five new functions (isolated): A+ (95.8); cogcom avg
  4.0, max 8 — within the avg<5 / max<25 bar.

## Cost vs. estimate

Single-file parser change (~60 lines added), two fixtures, one doc
edit. The only real time sink was a tooling slip: early `Edit` calls
used the bare `…/kaikai/` path from the briefing instead of the
worktree path `…/kaikai.block-lambda-destructure/`, landing the change
in the main checkout. Caught when `make` reported "kaic2 up to date" and
a grep for the new functions came back empty in the worktree; reverted
the main checkout and re-applied via `git apply`. Lesson reinforced:
every path in a worktree lane carries the worktree suffix.

## Follow-ups

- None required. A lambda-specific irrefutability message and bare
  constructor binders are possible future polish but are not motivated
  by the pain point this lane closed.
