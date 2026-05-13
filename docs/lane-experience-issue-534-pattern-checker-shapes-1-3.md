# Lane experience — issue #534 shapes 1-3 (pattern checker)

## Scope as planned

The #520 audit phase 2 catalogued four pattern/type-check gaps under
issue #534. This lane closes the three that are co-located in
`stage2/compiler.kai`'s pattern-pass and exhaustiveness walker:

1. **Duplicate binding in one pattern** — `match p { MkPair(x, x) -> x }`
   accepted silently, second `x` shadowed the first.
2. **Duplicate variant arm** — `match m { Yes(x) -> 1; Yes(y) -> 2 }`
   accepted, second arm unreachable.
3. **Duplicate literal arm** — `match x { 1 -> 10; 1 -> 20; _ -> 0 }`
   accepted, second literal unreachable.

Shape 4 (unbound type variable in a signature like `fn f(x: a) : b = x`)
was explicitly out of scope: it lives in `fn_scheme_of_decl` /
`infer_decl`, not in the pattern pass, and the blast radius differs.
A follow-up issue continues #534 for shape 4 alone.

## Scope as shipped

All three shapes ship in this lane. Implementation lives in
`stage2/compiler.kai`:

- **Shape 1** — `check_pattern_no_dup_binds` walks a pattern, gathers
  every `PBind` / `PAs` / `PNarrow` name (also list-rest binders), and
  reports the first repeat via `diag_error` + `st_bump_err`. Wired into
  the three pattern entry points: the `SLet` arm of `infer_stmt`,
  `check_arms`, and `synth_arms`.
- **Shapes 2 + 3** — `check_arms_redundancy` runs once per `match`
  before arm typing. It fingerprints each arm's head (`AhVariant`,
  `AhLitInt/Bool/Char/Str`, `AhCatchAll`, `AhUnclassified`) and reports
  either:
  - `unreachable match arm: previous arm matched every value` when an
    unguarded catch-all precedes another unguarded arm;
  - `duplicate match arm: <head> is already matched above` when two
    unguarded arms share the same classified head.
  Both errors fire once per offender. Called from `synth_match` and
  `check_match`.

Three fixtures promoted out of `examples/negative/silent_contract/`
into `examples/negative/patterns/`:

- `pattern_duplicate_binding.kai`
- `pattern_duplicate_variant_arm.kai`
- `pattern_duplicate_literal_arm.kai`

Each carries a single-line `.err.expected` golden matching the existing
patterns/ style. `silent_contract/README.md` strikes the three rows
from the #534 entry and notes shape 4 remains open. `tools/test-negative.sh`
reports 87 PASS (up from 84 before promotion).

## Design decisions

### Variant duplicates require "totally open" subpatterns

`Some(true)` and `Some(false)` are legitimately distinct arms — they
match disjoint values despite sharing the `Some` constructor name.
The naive "two arms with the same ctor name = duplicate" rule
mis-fired on the very first selfhost attempt (four sites in
`compiler.kai` plus a handful in stdlib protocols).

The shipped rule flags two arms as duplicates only when both have the
same ctor name AND every subpattern is a catch-all (`PWild`, `PBind`,
`PHole`, `PAs` wrapping a catch-all). `Some(x)` followed by `Some(y)`
fires; `Some(true)` followed by `Some(false)` does not. This matches
exactly what shape 2's fixture exercises; richer overlap analysis (e.g.
`Some(true)` after `Some(_)`) is left for a future lane that wants a
real reachability analyser.

This rule keeps the implementation syntactic and avoids writing a
general `pat_eq` — explicitly flagged in the briefing as out of scope.

### Filter synthetic fallthrough arms from `build_marm_columns`

The multi-arg-match desugar (`match a, b { p1, p2 -> ... }`) lowers
into a nested chain of two-arm matches where each outer arm has the
shape `[user_arm, Arm(PWild, None, fall)]` and the synthetic `PWild`
inherits the outer match's `(line, col)` via `mk_pat(PWild, line, col)`
in `build_marm_columns` (≈ line 4692). On the first attempt the
redundancy pass tripped on those synthetic catch-alls when the user
wrote a multi-arg match whose first column pattern was `_`
(demo `toquefama`: `[], _ -> 0; _, [] -> 0; ...`).

`arm_is_synthetic_fallthrough` keys off the `(pat.line, pat.col) ==
(match_line, match_col)` invariant — user-written wildcards always
carry their own pattern span. The synthetic arm is skipped in
redundancy bookkeeping but still typed normally. This is a
narrowly-scoped recogniser tied to the only AST shape that emits a
co-located wildcard; if a future desugar produces a different shape,
the redundancy pass will need to learn it.

### Diagnostic shape is single-line + help, no caret-rich notes

The `examples/negative/patterns/*.err.expected` goldens already use
single-line first-line matching. The new diagnostics follow that
convention: a one-line `error:` header, an optional `note:` line for
context, and a `help:` line offering the canonical fix (rename, add
guard, or move the arm). No multi-line carets — those would have
required adding `.diag.expected` body assertions and the issue body
did not call for them.

## Structural surprises

- **Selfhost found four real false-positive sites the first time
  around.** Before the catch-all-subpat refinement, `match opt {
  Some(true) -> ...; Some(false) -> ... }` patterns in
  `try_eval_pred` (lines 7510, 7517), `expr_show_pred` (lines 8154,
  8339), and AST-pretty-print helpers (`RL`, `TyName`, `EFn` matches
  on a struct-shaped option payload) triggered. None were actual
  duplicates; all distinguished arms by literal payload. The fix was
  the design call documented above — not a code change in
  `compiler.kai`.
- **The multi-arg `match` desugar emitted catch-alls the new pass
  cannot ignore by structure alone.** The `(line, col)` equality
  shortcut works because no other site emits a co-located PWild.
- **`Expr` record field is `kind`, not `ekind`.** First draft used
  `ekind`; build failed with a parse-time "no such field" pointing
  far from the source of the typo. Memo: when adding new field
  accessors against the AST record types, grep for an existing access
  before writing.

## Fixtures and coverage

- 3 fixtures promoted; goldens added; `tools/test-negative.sh`
  count rises from 84 PASS to 87 PASS.
- Existing pattern fixtures (`arm_type_mismatch`, `constructor_wrong_arity`,
  `guard_not_bool`, `non_exhaustive_match`) unaffected — the new pass
  fires before they would, but on disjoint inputs.
- The new diagnostics do NOT have a `.diag.expected` body golden.
  The issue body specifies error class + offending name; that's all
  the first-line golden enforces today. A follow-up lane could add
  caret/note body assertions if the diagnostic shape needs to harden.

## Cost vs estimate

Briefing estimated ~1 day (3 shapes × 4–6h). Actual elapsed for the
implementation passes was under that — the longest sub-task was
chasing the multi-arg-match desugar false-positive, which took two
iterations to recognise as a synthetic-AST issue rather than a real
pattern bug. The lesson was not new (#187 phase 3 wrestled with the
same `(line, col)` invariant for `PNarrow` rewrites) but it cost a
round-trip.

## Follow-ups left for next lanes

- **#534 shape 4 (unbound type variable in signature)** stays open
  after this PR merges. The closing PR for that lane should refile or
  rename the issue to scope it to shape 4 only, then close.
- **Richer arm-overlap analysis.** A real reachability checker would
  catch `Some(_)` followed by `Some(true)`, list patterns whose
  fixed-length prefixes overlap, record patterns with subsuming field
  shapes, etc. Today's pass is intentionally syntactic. If a future
  lane wants to ship this, the natural site is
  `check_arms_redundancy` — extend `ArmHead` and the
  `arm_head_classified` filter.
- **Stage 1 mirror not added.** kaic1 still accepts duplicate
  patterns; the pattern-pass walker in `stage1/compiler.kai` does
  not include this discipline. The negative fixtures gate stage 2
  only. Stage 1 is the bootstrap surface; user-facing kaikai is
  stage 2. If a future lane wants the discipline to apply to the
  stage 1 surface as well (e.g. for portability fixtures), the
  walker is small and can be mirrored.
