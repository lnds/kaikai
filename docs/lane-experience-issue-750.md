# Lane experience — issue #750 (piece-3 of #86): contract-violation `help: narrow` line

## Scope as planned vs as shipped

**Planned.** Emit the third diagnostic line of the #86 spec — `= help: narrow
`b` to `Real where != 0.0`` — for a contract-violation panic whose `requires`
predicate has the single-ident `<ident> <cmp> <literal>` shape that piece-2
already covers. Source the refined type's *base* (`Real` vs `Int`) properly,
not by guessing off the literal. Restrict to that shape; everything else falls
through to piece-1 with no regression.

**Shipped.** Exactly that. The base type is read from the offending
parameter's *declared* annotation (`Param.ptype`), the help renders only for
the single-ident scalar shape, and multi-conjunct / non-scalar / `ensures`
predicates fall through unchanged. Both backends (C and LLVM) emit identical
output — confirmed by `make -C stage2 test-violations`, which compiles and
runs every fixture under both paths and diffs the panic byte-for-byte.

## The design decision the issue got wrong

The issue's framing — "the help needs the base type from the **typer**; thread
it into the refinement-discharge path" — is the reason this piece was deferred
in 2026-05-31. That framing is wrong, and recognising why is the whole lane.

`wrap_with_contracts` (where the assert message is built) runs in `parse_decl`,
**during the parse**, and already receives `params: [Param]`. Each `Param`
carries `ptype: Option[TypeExpr]` — the type annotation the user *wrote*. For
`fn divide(a: Real, b: Real) requires b != 0.0`, the param `b` has
`ptype = Some(TyName("Real", []))` right there, before the typer ever runs.

asu's call (consulted once, as the brief allowed): for an **edit suggestion**,
the declared type *is* the authoritative source — it is the spelling the user
will rewrite, not a typer-normalised type they never typed (which might surface
an expanded alias, a resolved row cell, a `<dimensionless>` unit, etc.).
Threading a resolved `Ty` from the typer back to a parse-time site would also
break the single-pass discipline (Tier 1.3) for zero benefit. So: no typer
thread. Read `Param.ptype`, unwrap a `TyRefine` to its base, render the
`TyName` head. An unannotated param (`ptype = None`) or a non-atomic base
yields no help — piece-1, no regression. The acceptance gate #2 (`Int` for an
`Int` param, `Real` for a `Real` param, regardless of the literal `0`) is
satisfied precisely *because* the source is the declared type.

## The line-order decision (linus consult)

The #86 spec shows the help **last**, after the runtime `argument b was:`
line. But `argument` is appended by the runtime (`kai_assert_check_with_value`
concatenates it onto the static `base_msg`), while the help is static text that
naturally lives in the parse-time C string literal — so it comes *before*
`argument`. Putting it last would mean a 5th `const char *help` parameter on
the runtime helper, threaded through **both** emitters and **both** `runtime.h`
copies plus the LLVM forwarder — a real blast radius across pre-existing F-grade
files, purely to permute two lines of a stderr panic. linus's verdict: the help
content is what matters, not its position; this is a UX convention, not a parse
protocol; adapt the doc to the implementation, not the reverse. The doc's v3
status sidebar now states the real order and explains why.

## Structural surprises

- **kaic1 rejects `#[doc("""...""")]` and backticks inside attribute strings.**
  The first draft of `refine_help.kai` used a triple-quoted module `#[doc]` with
  backticks; the stage1 bootstrap lexer choked (`unexpected character '\``,
  `unterminated interpolation`) when it consumed the concatenated bundle. The
  fix is the same constraint `refinements.kai`'s header already documents:
  single-line `#[doc(...)]` for `pub` symbols, `#` comments for the module doc,
  no backticks inside attribute strings. (Backticks inside ordinary `"..."`
  string *literals* in the body are fine — only attribute strings choke.)
- **The build ran in the wrong checkout once.** An early `cd stage2 && make` had
  its cwd resolved against the main repo, not the lane worktree, so it built
  stale sources and produced a confusing "no such file" on the worktree's
  `build/`. All edits had landed in the worktree correctly (absolute
  `file_path`); only the *build* leaked. Pinning every build command to the
  absolute worktree path fixed it. (Same hazard as the worktree-edit-leak note.)

## Fixtures added

- `requires_help_narrow.kai` — the canonical #86-spec shape (`b != 0.0`,
  `b: Real`); pins the help line itself. Golden carries `Real where != 0`.
- `requires_help_narrow_int.kai` — the *base-type provenance* pin: the same
  `!= 0` predicate over an `Int` param must read `Int where != 0`, not `Real`.
  This is the fixture that would fail a literal-guessing implementation and
  pass a declared-type one — it *is* acceptance gate #2.

Both wire into the existing `test-violations` harness (auto-globs
`examples/refinements/*.kai`, diffs `.run.err.expected` under C **and** LLVM).
Six pre-existing single-ident goldens were regenerated to carry the new help
line — that is the feature landing, not a regression; the non-matching shapes
(`requires_complex_predicate`, `ensures_violation_diagnostic`,
`requires_violation_string`) were left untouched and still pass, which is the
negative coverage for gate #3.

## Coverage gaps / follow-ups

- The `or wrap in ensure(b) where != 0.0` half of the help is still out of
  scope (a return-type refinement, a different channel) — tracked under #86.
- The call-site `--> file:line:col` caret is still aspirational (needs the
  call-site span threaded into the assert) — tracked under #86.
- No fixture exercises an unannotated-param `requires` (the `ptype = None` →
  no-help path) because a top-level fn param is conventionally annotated;
  `requires_complex_predicate` covers the "no help" outcome via a different
  shape, and the `None` arm is covered by reading.

## Cost vs estimate

One new A++ file (`refine_help.kai`, 67 LOC, cogcom avg 3.4 / max 7), ~25
additive LOC in `refinements.kai` (threading `params` + a trivial
`violation_help_suffix`), one `BUNDLE_SRCS` insertion, two new fixtures, six
regenerated goldens, one doc sidebar. No runtime, no emitter, no AST-shape
change — the line-order decision is what kept the blast radius this small.
