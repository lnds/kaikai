# Lane experience — issue #1137: range patterns

## Scope as planned vs as shipped

**Planned:** `case lo..hi ->` matches an inclusive Int/Char interval in
`match` arms and clause-block `case` bodies. Both bounds literal, no
step, no expressions. Desugar to a comparison pair, no new runtime.
Participate in the exhaustiveness/reachability checker. Negative cases
for non-literal bound and non-numeric scrutinee.

**Shipped:** exactly that. New `PRange(Expr, Expr)` variant in `PatKind`;
parser accepts `<lit> .. <lit>` after any Int/Char/`-Int` literal in
pattern position; a desugar pass rewrites `Arm(PRange(lo, hi), g, body)`
into `Arm(PBind(fresh), (fresh >= lo and fresh <= hi) [and g], body)`.
No changes to typer, KIR, or codegen beyond defensive `PRange` arms —
the node never survives desugar.

## Design decisions

**Desugar to `PBind` + comparison guard, not a live `PRange` through the
typer.** The precedent is exact: `PNarrow` (the `name : T` refinement
narrow) already rewrites to `PBind(name)` + a synthesised predicate
guard in `desugar.kai`, and `desugar_const_patterns` rewrites a const
pattern to `PBind` + an equality guard. A range pattern is the same
shape — a refutable binder with a predicate — so it reuses the entire
downstream pipeline (typer, exhaustiveness-via-guard, reachability,
lowering, emit) with zero new handling. asu confirmed: keeping `PRange`
live to the typer would buy a marginally better exhaustiveness message
at the cost of touching N passes with a new node, for a cover the issue
explicitly does not require. The guard shape also makes the acceptance
criterion ("guard-style equivalent produces identical output") true by
construction — the two lower to the same AST.

**Non-literal bound rejected in the parser, not the typer.** Keeping
both bounds literal is what keeps exhaustiveness decidable, so the
parser is the right gate: `parse_range_bound` accepts only Int / Char /
`-Int` and returns `None` otherwise, surfacing "range pattern bounds
must be Int or Char literals". A non-numeric scrutinee needs no special
handling — the desugared `>=`/`<=` simply fails to type-check against a
String, giving "type mismatch in comparison" for free.

**Pass placement.** The rewrite must run *after* name resolution and the
const-pattern desugar but *before* the typer — the same slot
`desugar_const_patterns_decls` occupies (driver.kai, just before the
nursery rewrite). An earlier attempt to hook it next to
`lower_pattern_narrow_decls` (pre-resolution) produced a runtime panic:
the synthesised `EVar(fresh)` guard had no resolved binder yet.

## Structural surprise the brief did not anticipate

**kaic1 does not check match exhaustiveness, so a missing `PatKind` arm
is a runtime panic, not a compile error.** Adding `PRange` to the enum
compiled cleanly through `make kaic2` (kaic1 built the bundle without
complaint), which gave false confidence. The first `1..9` program then
panicked with "non-exhaustive match" — *inside the compiler*, from a
`match ...pkind` in `resolve.kai` that enumerated every variant with no
catch-all. An Explore sweep found 13 such exhaustive-without-catch-all
matches across resolve, emit_c, emit_shared, infer, modules, protos,
perceus. Each needed a `PRange` arm even though most run post-desugar
and never see the node. Lesson for the next lane adding a `PatKind` /
`ExprKind` variant: **the clean kaic2 build is not the oracle — grep
every `match …pkind` and add the arm, or the panic surfaces only when a
program exercises the new shape.** (This is the "new variant = walker
sweep trap" already in the memory index, confirmed again here.)

## Fixtures added

- `examples/match/range_pattern_match.kai` (+ golden) — Int and Char
  ranges in `match`, each paired with its `when`-guard twin so the
  golden proves identical output; includes a negative range `-9..-1`.
- `examples/match/range_pattern_clause_block.kai` (+ golden) — ranges in
  a clause-block `case` body, Int and Char.
- `examples/match/range_pattern_nonliteral_bound.kai` (+ `.err.expected`)
  — variable upper bound, parse error.
- `examples/match/range_pattern_non_numeric.kai` (+ `.err.expected`) —
  String scrutinee, type error.
- `docs/info/match.md` — Range patterns section, one compilable
  `kaikai` example + two `kaikai-neg` blocks (verified by
  `make test-info`).

All wired into the existing `test-match` gate; no new harness needed.

## Coverage gaps / follow-ups

- Exhaustiveness over `Int` never forces a catch-all (the checker skips
  non-enumerable scrutinee types — `check_exhaustive` returns early for
  `Int`/lists). A range pattern inherits this exactly, matching the
  guard equivalent. If a future lane wants "a range arm without a
  catch-all is a warning", that is a checker enhancement orthogonal to
  this feature, and would apply equally to hand-written guard arms.
- Char ranges rely on Char comparing as its code point; this is the
  existing `>=`/`<=` behaviour on Char, untouched here.
