# Lane retro — qualified record literal `mod.Type { ... }` (issue #456)

## Branch / scope

Branch: `lane-issue-456-qualified-record`.

Closes: #456. Parser + qualtype rewriter accept the qualified surface
`mod.Type { ... }` after a plain `import mod`, closing the asymmetry
with `mod.fn(...)` calls (#218 / m6.2) and `: mod.Type` type
ascriptions (#232). The user-visible diff is one less "expected
expression" cliff when constructing record values from a module the
caller imported without selective destructuring.

## Scope as planned

Issue body laid out four positions where the qualified form is
recognised today (call, type ascription, variant pattern, ctor
pattern) versus one that fails (record literal). The brief asked for
a four-step lane:

- Parser: accept `mod.Type {` in expression position and emit the
  same `ERecordLit` shape used by the unqualified form.
- Typer / resolver: wire `mod.Type` to the record type declared in
  `mod` via the existing qualified-lookup machinery.
- Fixtures: basic repro, two-module case showing the surface, and
  the spread sugar composed with the qualified head.
- Gates: tier0 green, tier1 green, selfhost byte-identical,
  fixtures pass.

Estimate: 1 day.

## Scope as shipped

Real cost: ~half a day; the parser site and the qualtype hook were
both small and reused the issue-#232 (type-head) precedent more or
less verbatim.

Three changes in `stage2/compiler.kai`:

1. **Parser** (`parse_ident_primary`, ~line 3346): after the existing
   "lowercase ident + `.UpperIdent`" peek for qualified types, add a
   sibling branch that also requires the next token to be `{`. The
   commitment to record-lit interpretation only fires on that third
   token, so `mod.fn(...)` calls, `mod.field` postfix access, and
   `mod.UpperIdent` (without a brace) all continue to route through
   `parse_postfix_rest` as before. On a match, fold the qualifier
   into a dotted name `"mod.Type"` (identical encoding to issue #232
   in `parse_named_type`) and dispatch to `parse_record_lit`.

2. **Qualtype rewriter** (`qualtype_expr_kind_at`, new ~line 44595):
   intercept `ERecordLit(name, fs)` before the generic
   `map_expr_kind` fallthrough. When the name carries a dotted
   qualifier, run it through `split_dotted_qualifier` + `mt_lookup` +
   `me_has_export` — the exact same trio `qualtype_te` uses for
   TyName — and rewrite to `ERecordLit(ty, fs)` (bare). The
   downstream typer cascade sees the bare form and proceeds
   identically to today's selective-import surface. Unknown module
   → `qt_diag_unknown_mod`; known module, missing export →
   `qt_diag_missing_type` with the export list. Both diagnostics
   substitute `"__qualtype_bad__"` as the type name to force a
   downstream cascade failure (same sentinel TyName uses).

3. **Position threading**: the existing `qualtype_expr_kind` matches
   on `ExprKind` directly and has no line/col available for the
   diagnostic emitters. Rather than thread positions through every
   arm, route the kind-level rewrite through a thin
   `qualtype_expr_kind_at(k, mt, file, line, col)` wrapper that
   handles the one position-sensitive arm (the new `ERecordLit`)
   and delegates everything else to the legacy `qualtype_expr_kind`.
   `qualtype_expr` calls the `_at` form so all wrapped exprs pay no
   structural cost.

Fixtures (Part C):

- `examples/imports/qualified_record_basic/` — the issue body
  reproducer (geometria + Punto) with the qualified form on the RHS
  and a sibling qualified type annotation on the LHS. Round-trips
  through `bin/kai run`.
- `examples/imports/qualified_record_ambiguous/` — two modules
  (`shapes2d` + `shapes3d`) both participating in a record-vocab.
  Names differ on purpose (`Point2` / `Point3`); see "Out of scope"
  below for the actual ambiguity case.
- `examples/imports/qualified_record_with_spread/` — the spread
  sugar (issue #326) composed with the qualified head:
  `mod.Point { ...src, x: override }`. Required no extra parser
  work — the spread parser sits inside `parse_record_lit` which
  the new arm already feeds.

Makefile wiring: new `test-import-qualified-record` target in the
top-level Makefile, mounted on the `test` aggregator next to the
existing `test-import-stdlib` / `test-import-prelude-dedup`
fixtures. Same shape: a `for case in …; do bin/kai run …; diff … ; done`.

## Design decisions

### Why an `_at` wrapper instead of expanding `qualtype_expr_kind`

The kind-level match for the rewriter walks ~10 arms and is invoked
from `map_expr_kind` recursively. The new `ERecordLit` arm is the
only one that needs source position for a diagnostic — everything
else is structural recursion. Adding a position-carrying `kind_at`
shim keeps the existing call sites unchanged. Cost: one extra
function (~25 lines incl. recursing helper).

Alternative considered: thread `(line, col)` through every
`qualtype_*` helper. Rejected — invasive, touches every recursive
call, and pollutes signatures with arguments that all-but-one arm
ignores.

Alternative considered: drop the position from the diagnostic and
emit at `(0, 0)`. Rejected — issue #232's TyName diagnostic ships
real positions, and users hitting the same diagnostic on the new
surface deserve the same quality.

### Why fold the qualifier into the name string

`ERecordLit` is `ERecordLit(String, [FieldInit])` today. The
qualifier could be carried as a separate `Option[String]` field on a
new variant, or as a sibling enum case. Both options would touch
every match on `ERecordLit` (and `map_expr_kind`, and the codegen).
The dotted-name encoding (#232's choice for types) keeps the AST
shape stable and pushes all the qualifier reasoning into one pass
(`qualtype_*`). Once the pass runs, downstream sees the bare name
and never has to know about qualifiers.

If a later lane needs to keep the qualifier alive past resolution
(e.g. for module-aware error messages or for the LSP), the encoding
can be unfolded into a structured form at that point — no caller
besides `qualtype_expr_kind_at` knows about the dotted string.

### Why no recursion into field exprs from `map_expr_kind`

`map_expr_kind` already recurses into `ERecordLit` fields via the
default arm. But the new `ERecordLit` rewrite needs to walk the
fields itself (the qualifier rewrite must compose with nested
qualified record-lits inside a field expr like
`outer.Type { inner: nested.Sub { ... } }`). The new helper
`qualtype_fields_init` walks the fields explicitly and recurses
through `qualtype_expr` so the rewriter sees every sub-expression
in source order. Verified by the spread fixture, where the
`__spread__` source itself can be (and is) a recursive expression.

## Out of scope (not closed by this lane)

- **True name collision across modules**: the issue body's
  motivating case (two modules both exporting `Point`) is *not*
  resolved by this lane. Today's resolver flattens both modules'
  top-level types into a single scope via concat semantics; the
  first import wins for the bare name, and the qualified form still
  routes through that flattened table. A real fix needs a
  qualified-namespace resolver — the qualified surface this lane
  ships is a *necessary* precondition (you cannot disambiguate
  what you cannot write) but it is not *sufficient*. The ambiguous
  fixture uses two distinct type names to demonstrate the surface
  without depending on the collision fix.

- **Qualified record-pattern in `match`**: patterns already accept
  the qualified ctor surface (issue #234) but drop the qualifier
  rather than validate it against the module table. That asymmetry
  exists across both the existing patterns and this new record
  literal — both rely on type narrowing for correctness. Tightening
  the pattern qualifier into the same validating path the literal
  uses is a separate cleanup, not gated by this lane.

## Gates

- `make tier0` — green. Selfhost byte-identical (the parser
  change does not touch any token kaic2 emits when compiling
  itself, since compiler.kai uses no qualified record literals).
- `make tier1` — green locally.
- Reproducer from issue body — compiles and runs.
- 3 fixtures — pass.

## Follow-ups (parked, not gating)

- True qualified-namespace resolver (closes the issue's motivating
  collision case; file a fresh issue when the user prioritises it).
- Tighten qualified-pattern handling in `parse_ident_pattern` to
  validate via `mt_lookup` instead of silently dropping the
  qualifier (so a typo'd `wronng.Ctor` in pattern position
  surfaces as a clear diagnostic rather than a confusing later
  "no such variant" error).

## Real cost vs estimate

Estimate: 1 day. Real: ~half a day, including writing the three
fixtures and this retro. Most of the time went into reading
`qualtype_*` and the `parse_ident_primary` neighbourhood to
verify the precedent (#232 for types, #234 for ctor patterns).
The actual code change is small (~40 lines added across the
two sites, plus the fixtures and Makefile target).
