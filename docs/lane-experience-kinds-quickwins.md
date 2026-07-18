# Lane retro — kinds quick-wins (#1250, #1254, #1257)

Three small, disjoint fixes to the kind system, shipped as one PR.

## Scope as planned vs as shipped

Planned and shipped match: one commit per fix, no scope creep. The
brief's diagnosis was accurate in all three cases and each fix landed
where the brief predicted.

## #1250 — `unit` as a contextual introducer

Six kind introducers exist (`unit`, `currency`, `region`, `layout`, plus
user-declared ones); five were already contextual, `unit` alone was a
hard keyword, so `let unit = 42` failed to parse.

The fix turned out smaller than the brief's "route it through the
contextual path" framing suggested, because `unit` was *already*
registered in `base_intros()` — the contextual recogniser
`decl_head_is_user_introducer` had an explicit `if word == "unit" { false }`
guard specifically to keep it out. So the whole change was:

1. Stop the lexer minting `TkUnitKw` (delete one branch in `keyword_of`).
2. Delete the `word == "unit"` guard.
3. Delete the two now-unreachable `TkUnitKw` branches in the decl
   dispatcher and in `parse_kind_introducer`.

**Decision: keep the `TkUnitKw` variant declared, just unreachable.**
Removing a sum variant from the middle of `TokenKind` renumbers tags,
which this repo has been bitten by before (native heap corruption
visible only in CI). The variant costs nothing dead; deleting it buys a
tag-renumber risk for zero benefit. If it is ever removed it belongs in
a lane that can afford a full native run, not a quick-win.

The `kinds.kai` pre-scan needed no change: `introducer_word` special-cased
the keyword but its fallback (`ksrc`) yields the same `"unit"` string.

## #1254 — Functorial comment

Pure doc fix, verified before writing: `pipe_fusion.kai` has zero
mentions of `Functorial`, while `law_checks.kai` reads the theory to
synthesise `check` blocks per impl. The comment claimed the optimiser
consumed the laws; it does not. Rewrote to say law-checking, keeping the
confluence observation (which is still true of the laws read as rewrite
rules, independent of whether anything rewrites with them).

## #1257 — closed property menu

`parse_theory_props` accepted any lowercase ident. Properties are stored
on `DTheory`, dumped, and then discarded without validation, so
`theory Foo = { banana }` was accepted end to end.

Validation went in `parse_theory_props` itself rather than a post-parse
pass: the parser has the token in hand, so the diagnostic gets an exact
span for free, and `closest_name` (already in `diag.kai`) supplies the
did-you-mean.

**Structural surprise:** no generic list helper was usable. `list_any`
in `protos.kai` is monomorphised to `[Param]`, and there is no
`str_join` in the compiler bundle. Rather than add generic helpers for
one call site, membership is a flat `or` chain and the menu is spelled
out in the message string. `theory_prop_menu()` survives only as the
`closest_name` candidate list — the mild duplication between it and the
`or` chain is the cheaper trade at this size.

Menu confirmed against `docs/kind-system-design.md:90` and the theories
actually declared in `stdlib/core/kinds.kai` (which use six of the eight;
`idempotent` and `rows` are declared-but-unused, kept per the design doc).

Scope held: only membership. The decidable-*combination* check (which
property sets map to an engine) stays with the engines issue.

## Fixtures

- `examples/sugars/kinds_unit_contextual.kai` (+ `.out.expected`) —
  `unit m` declares while `let unit`/`let currency`/`let region`/
  `let layout` all bind normally, in one file, with unit arithmetic
  still typing.
- `examples/sugars/kinds_theory_bad_prop_err.kai` (+ `.err.expected`) —
  `theory Bad = { assoc, banana }` rejected.

`test-sugars` globs the directory, so neither needed Makefile wiring.

## Gates

Selfhost byte-identity closed (`kaic2b.c == kaic2c.c`), `tier0` green,
full `test-sugars` green. As expected for parser/comment changes with no
runtime semantics touched.

## Follow-ups

- The dead `TkUnitKw` variant (and its `tk_name` row) can be dropped by a
  lane that runs the native suite.
- Connecting the law-check to fusion — i.e. letting a `Functorial` proof
  actually license the rewrite — remains open design, deliberately out of
  scope here.
