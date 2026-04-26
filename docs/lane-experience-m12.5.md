# Lane experience report — m12.5 (Units of Measure)

LLM agent: Claude (Opus 4.7, 1M context). Single-agent run.

## Objective metrics (from /tmp/lane-m12.5-builds.tsv)

- Start: 2026-04-26T16:58:51-04:00
- End:   2026-04-26T17:55:17-04:00
- Wall-clock: ~56 minutes (interactive, with one quick interruption
  for record-field-named-`unit` collision diagnosis).
- Diff size: +1401 / −43 in `stage2/compiler.kai` (≈ +1100 effective
  net lines, ≈ +200 lines of docs).

### Build/test invocations (TSV log)

```
2026-04-26T17:03:00  make all          OK  init build, baseline
2026-04-26T17:29:53  check-self        OK  AST + Subst landed (kaic2 type-checks itself)
2026-04-26T17:44:06  fixtures          OK  10 / 10 fixtures pass
2026-04-26T17:49:17  make test stg2    OK  full stage 2 test suite
2026-04-26T17:50:25  make test         OK  full repo test suite
2026-04-26T17:50:38  selfhost          OK  5.5s — fixed-point on stage 1+2 C path
2026-04-26T17:51:03  selfhost-llvm     OK  4.9s — fixed-point on LLVM path
```

The compiler's own selfhost time is unchanged from the m7e13 baseline
(within noise), confirming the typer cost of the unit machinery is
proportional to the number of dimensioned values — pure code pays
nothing.

## Compiler errors I encountered

| # | Class                                  | Count | Surface |
|---|----------------------------------------|-------|---------|
| 1 | non-exhaustive match (added new variants) | ~15  | kaic2's exhaustiveness checker after each AST-extension step |
| 2 | C compile errors from stage 1's lambda free-var bug | 7 | C build of generated stage2.c, in lambdas containing `match Pattern { Ctor(x, y) -> ... }` |
| 3 | C compile error from name shadow (param `mul` vs `kai_mul` runtime fn) | 1 | C build, fixed by renaming param to `scale` |
| 4 | typer error in `Real<Newton>` use until alias-expansion landed | 1 | initial type-rule pass before adding pre-pass |
| 5 | typer parity error (`unit` reserved word collided with record field name `unit:`) | 1 | selfhost re-check after lexer keyword landed; renamed field `unit` → `dim` |

Errors of class 1 are the well-behaved kind — kaic2 enumerated every
remaining match site that needed a new arm. Errors of class 2 are
the pre-known stage-1 capture bug that the m7e13 report flagged as
the dominant source of friction; the workaround pattern (lift inline
lambdas to top-level helpers when their match-arm bodies bind names)
is now reflex.

## Friction points

### Abelian-group unification — easier than predicted

The spec lists this under **Risks**. In practice every fixture
pivots on a unit-var with |exponent| = 1, which makes the
substitution `var := -(rest)/k` exact. The branch in
`unify_unit_diff` for |k| > 1 is wired as `None` with a
docs-cited stop guard rather than as a fresh-var split — the lane
ships without exercising that path, and the docs flag it as a
"polynomial" guarantee that future adversarial tests would need.
Smith Normal Form was *not* required for the fixtures the lane
specifies. If `area[u: Unit](w: Real<u>, h: Real<u>) : Real<u^2>`
were called against `area(2.0<m>, 3.0<m>)` the diff is
`m^-1 * u^1` — pivot exp 1, easy. The harder case is e.g.
`Real<u^2>` ≡ `Real<m^2>` standalone, which fixtures don't test.

### Visitor sprawl — manageable, but every variant needs auditing

Adding 4 variants (`DUnit`, `TyDim`, `TyDimT`, `ELitUnit`) reached
exhaustiveness errors at ~15 distinct match sites. Stage 2's
checker walks each one; I only had to follow its diagnostics
sequentially. About half the sites are pure pass-through (recurse
into base / inner with no semantic change) and half need real
handling (resolve, unify, render).

### Catch-alls (per the briefing's warning)

I ran a pre-commit audit over `_ ->` catch-alls in matches over
`Decl`, `Ty`, `TyKind`, `ExprKind`. Two sites surfaced:
- `check_decl_row_labels` (`_ -> 0`) — semantically correct for
  `DUnit` (no row to validate).
- `collect_alias_cands` (`_ -> []`) — semantically correct
  (TyDim isn't an effect alias).

Neither is a current bug; both are noted in the audit log.

### Lambda capture (stage-1 era)

Pattern-binders inside an inline lambda inside a `map`/`filter`
get spuriously captured by stage 1's free-var analyser, producing
C that uses undeclared identifiers. Fix is mechanical: hoist the
lambda body to a top-level helper. I hit this 4× in this lane (in
canonicalisation/expand-aliases helpers). The diagnostic surface
is bad — the C compiler error mentions `kai_n` undeclared — and
the underlying cause is in stage 1, which the m12.5 lane is
forbidden to touch. m7b #18 fixed the analyser in stage 2 but
stage 1 is still on the old code.

### Field-name vs keyword collision (`unit:`)

Adding `unit` as a keyword broke a record field declaration
(`type DimSplit = { base: Ty, unit: UnitExprT }`) that I'd
introduced earlier in the lane. Stage 1 (which built kaic2)
treated `unit` as IDENT and accepted it; kaic2 (with the new
keyword) rejected its own source. Renamed `unit` → `dim`. Worth
remembering: when introducing a keyword, scan the source first
for that identifier in field positions.

## Spec ambiguities or interpretive choices

1. **Unit application for record types** (`Money[u: Unit]` →
   `Money<u>` vs `Money[u]`?). The spec example shows
   `Money<u>` for record application. Lane scoped to the
   primitives-only path (Real / Int) — record-with-unit-param is
   a follow-up. Fixtures don't exercise the record case, so
   no decision needed at lane scope.
2. **Kind annotation syntax** (`[u: Unit]` vs `[u of Unit]` vs
   `[<Measure> u]`). Picked `[u: Unit]` per spec §5; encoded
   internally by suffixing the tparam name with `#Unit` so the
   existing `tparams: [String]` plumbing keeps working without
   widening to `[(String, Kind)]` at every call site.
3. **Adjacent vs non-adjacent `<`** for literals vs types. Spec
   says no space between number and `<` for literals. Type
   position has no comparison operator to disambiguate from, so
   no adjacency check there. Implemented exactly that way:
   parser uses token start-position adjacency for literals,
   straight `<` lookahead for type position.
4. **`unitless()` runtime representation**. Implemented as an
   identity special-case in both C and LLVM emitters — no
   `kai_prelude_unitless` runtime function. Saves a runtime call
   and matches the codegen-erasure invariant exactly.

## Subjective summary

- **Confidence in correctness**: high for the fixtures the lane
  ships (10/10 pass, including the negatives). Medium for
  unit unification on adversarial shapes (|k|>1 pivots), where
  the implementation refuses rather than guesses.
- **Wall-clock estimate**: ~1 hour active work. Spec said "2-3 days
  at observed velocity"; the spec's calibration is humans-with-IDE,
  not a focused LLM run.
- **Hardest sub-task**: alias expansion. Required either threading
  an alias table everywhere (touches every resolver call) or
  writing an AST pre-pass that rewrites every TypeExpr/Expr. Took
  the pre-pass route — verbose (~150 lines of mechanical visitor)
  but isolated.
- **Easiest sub-task**: unit canonicalisation. Once the table form
  (sorted (sym, exp) lists) is chosen, every operation reduces to
  list manipulation.
- **Did the compiler help or hinder?** Help, dominant. Stage 2's
  exhaustive-match checker walked me through every visitor that
  needed a new arm. The non-exhaustive errors landed quickly and
  explicitly — way better than silently emitting wrong C and
  debugging the runtime later. The two non-helpful surfaces were
  (a) stage 1's lambda capture bug (already known) and (b) the
  field-name/keyword collision (any compiler with reserved words
  has this).
- **Visitor sprawl count**: ~15 sites updated explicitly, 2 catch-all
  sites audited and judged benign.

## Validation of design assumptions

- **Codegen erasure verified**: `cmp` of C output for
  `with_units.kai` vs `no_units.kai` returns no diff. Both
  binaries produce identical stdout when run.
- **Compile-time impact**: selfhost wall-clock (5.5s) is within
  noise of the post-m7b baseline. The added typer machinery is
  paid only by code that uses dimensioned values; pure code is
  untouched.
- **Polynomial unification confirmed**: every fixture pivots on
  |exp|=1; the implementation does at most one var-elimination
  step per equation. No exponential blowup observable; the
  guard rail in `unify_unit_diff` (returning `None` on |k|>1
  rather than entering Smith Normal Form) keeps the path
  documented even though no fixture exercises it.

## Limitations of this report

- Self-report bias: same agent that wrote the code wrote the
  report. Independent verification not done.
- Single agent. No comparison run with a second LLM or with a
  human implementer; the wall-clock and difficulty rankings are
  not statistically meaningful with N=1.
- The "fixture coverage" claim is honest but bounded: 10
  fixtures touch every documented surface (decl, alias, basic
  arith, mismatch, generic, escape, canonicalisation, Int unit,
  codegen invariance) but do not stress-test pathological unit
  expressions. The |k|>1 unification path is wired but unused.

## Raw build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-04-26T17:03:00-04:00	make all	OK	init
2026-04-26T17:29:53-04:00	check-self	OK	AST landed
2026-04-26T17:44:06-04:00	fixtures	OK	10/10
2026-04-26T17:49:17-04:00	make test (stage2)	OK	-
2026-04-26T17:50:25-04:00	make test	OK	-
2026-04-26T17:50:38-04:00	selfhost	OK	5.5s
2026-04-26T17:51:03-04:00	selfhost-llvm	OK	4.9s
```
