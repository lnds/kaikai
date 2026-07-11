# Lane experience — issue #1178: Type and Effect as builtin-engine catalog kinds

## Scope as planned vs shipped

Planned (issue #1178): declare `Type` and `Effect` in the kind catalog over
`builtin` theories, closed and not redefinable; parser support for the
`builtin` theory body; validator rejection of user builtin theories and of
builtin-kind redeclaration; docs. Explicitly out of scope: any change to HM,
row unification, or the unification dispatcher.

Shipped exactly that, plus one guard the issue implied but did not spell out:
a user kind classified by a builtin theory (`kind Foo : EffectRow with foo`)
is also rejected — without it the parser would admit the theory name and the
abelian engine would then treat its habitants as unit-like, which is
meaningless for a compiler-core engine.

## Design decisions

- **Theory names**: `HindleyMilner` and `EffectRow` (the issue's placeholders
  were `Hindley`/`EffectRow`). `HindleyMilner` is the honest full name of the
  type engine; `EffectRow` names what the row unifier actually unifies.
- **AST encoding of `builtin`**: stored as a single sentinel property
  `"#builtin"` on the existing `DTheory(String, [String], Int, Int)` shape
  rather than a new field or variant. `#` cannot start a parsed identifier,
  so the sentinel is disjoint from every property list by construction.
  Alternative considered: adding a `Bool` field — rejected because it changes
  the constructor arity and forces a sweep over every `DTheory` match arm
  (cache, emit, fmt, modules, resolve, unit_walk, …) for a flag only three
  printers and one validator care about.
- **Where the closure guard lives**: `doc_attr.kai`'s doc-strip pass, beside
  the existing catalog-only `theory` gate. It is the sole pass that sees
  `DTheory`/`DKind` with the source file path in hand, which is exactly the
  discriminator needed ("is this the catalog file?"). No new pass, no typer
  involvement.
- **`with type` / `with effect`**: `type` and `effect` lex as keywords, so
  `parse_kind_introducer` accepts `TkType`/`TkEffect` alongside `TkUnitKw`
  and stores the surface word verbatim — symmetric to how `unit` is handled.
  The introducer registry entries these create are inert in habitant
  dispatch, because a keyword token can never appear where the parser checks
  `kind_reg_is_introducer` (that path requires an IDENT head).

## Structural surprises

- The closure was cheaper than briefed: `doc_attr.kai` already rejected every
  user `theory` outside the catalog (issue #1108's lane), so "user cannot
  declare `theory X = builtin`" held before this lane — what was missing was
  the dedicated diagnostic and the `DKind` side (redeclaration + builtin
  theory reference), which had no guard at all.
- The catalog file is *not* a prelude: `core/kinds.kai` is absent from
  `core_module_files()`, so normal compilations never parse it. The closed
  names therefore had to be hardcoded in `ast.kai` (`is_builtin_kind`,
  `is_builtin_theory`) rather than derived from the catalog decls.

## Fixtures

Four negative fixtures in `examples/negative/modules/` (auto-discovered by
`tools/test-negative.sh`): `theory_builtin_user`, `kind_type_redeclared`,
`kind_effect_redeclared`, `kind_over_builtin_theory`. Positive surface is
covered by the catalog itself (`kaic2 --check stdlib/core/kinds.kai` passes)
and the pre-existing `examples/sugars/kind_theory_surface.kai`.

Coverage gap left open: no harness compiles `stdlib/core/kinds.kai` in CI, so
a regression that breaks only the catalog file would surface first in `kai
doc` / downstream use, not in a tier. Follow-up candidate: a one-line
`--check` of the catalog in a stdlib harness.

## Cost vs estimate

Small lane, single session: parser + validator + catalog + docs + fixtures,
no rework. The typer was never touched (catalog + parser + validator only),
which the selfhost gate confirms.

## Follow-ups

- Wire a catalog `--check` into a tier harness (gap above).
- `docs/units-of-measure.md` still describes `Measure` as "the second kind";
  harmless today, worth aligning next time that doc is touched.
- Sibling catalog work: `Module` (#1174), `Structural`/regions (#1123).
