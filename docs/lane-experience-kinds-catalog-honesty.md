# Lane retro — the theory catalog stops claiming engines it does not have (#1270, #1265)

## Scope as planned vs. as shipped

Planned: reclassify `Functorial` out of the theory catalog (#1270), dissolve
`Structural` (#1265), delete the dead `structural_kind_names` pair. Explicitly a
catalog/surface lane — **no unification semantics touched**, so the selfhost gate
would be the cheap kind.

Shipped exactly that, plus two things the brief did not name but the code forced:

- **`fusion` left the theory property menu.** It is a functor law, not a unification
  property; leaving it in the menu after `Functorial` left the catalog would have kept
  a law name in the theory namespace — the same category error one level down. #1270's
  "audit whether they leak into the menu" was the pointer. `identity` stays: it is a
  genuine `AbelianGroup` property that the functor laws merely happen to share a name
  with.
- **Two theory names had to be invented**, since dissolving a label means naming what
  replaces it (below).

## Design decisions

### `Functorial` keeps its surface; only its namespace changes

The issue floated `#[laws(functor)]` as an alternative spelling. Rejected: the surface
`protocol P[s: Shape] : Functorial` is already shipped and used, and Tier 1 #4 says a
surface a user wrote against is preserved within an edition. Changing the spelling
would have made a catalog-honesty lane into a migration lane for no honesty gain — the
dishonesty was never in the *spelling*, it was in `Functorial` sitting in
`is_known_theory` next to real engines.

So the fix is a **namespace split in the parser**, not a syntax change:

- `is_known_theory` — what a `kind K : T` may name. Lost `Functorial`.
- `is_protocol_law` — what a `protocol P : L` header and its `axiom L` exemption may
  name. Gained `Functorial`, and only it.

The two are now disjoint, and both directions are errors with their own diagnostics:
`kind Bad : Functorial` says *unknown theory*, `protocol P : AbelianGroup` says
*unknown protocol law `AbelianGroup` — the law sets are: Functorial*. Before this lane
both were accepted, which is precisely how the mirage stayed invisible.

Pleasant surprise: the AST was **already honest**. The wrapper node is `DProtoLaws`,
not `DProtoTheory` — a previous lane named it for what it is. Only the parser gate and
the catalog were lying. That kept the change to a handful of lines.

### Naming the two theories that replace `Structural`

`Structural` grouped `Region` and `Shape`, which share a formation guard and nothing
else. Dissolving it needs two names, and a name is a public commitment:

- **`ConstructorApp`** (`Shape`) — names the engine's actual mechanism: it decomposes
  an application `s[A]` into head and argument and binds the constructor witness
  (`shape_bind` / `shape_witness_of` / `TyShapeApp`). Not "identity"; identity is what
  it does *after* binding.
- **`Nominal`** (`Region`) — identity by declaration rather than structure, the
  standard type-theory word, and it stays accurate once the real branding engine lands.

Both are `builtin`: their engine is the compiler core, so no user kind may be declared
over either. That is unchanged behaviour — `Structural` was already builtin — just
split in two.

**`Nominal` still has no engine of its own.** Region identity continues to ride
`TyDim` into `unify_abelian` behind a formation guard. This lane does not fix that and
does not claim to: it makes the catalog *name* the gap instead of hiding it under a
label shared with a kind that does have an engine. The honesty map now says so in the
row. Building the engine is the next lane's work.

### The dead code was genuinely dead

`structural_kind_names` and `base_structural_kind_names` had **zero callers** —
verified against the whole `stage2/` tree, not just by eye. The consequence is worth
recording because it is easy to misread the deletion as a behaviour change: `Region`
was never in `atomic_kind_names` (which is Module + Composition only), so Region
habitants get **no formation guard today**. The functions claimed to supply one and
supplied nothing. Deleting them removes a false claim, not a check.

## Structural surprises

- **`kai info kinds` is a third catalog.** Beyond `stdlib/core/kinds.kai` (the
  declarative catalog) and the parser's hardcoded membership tests, `docs/info/kinds.md`
  ships with the binary as the user-facing authority. Three places name every theory;
  all three had to move together. A future lane adding or removing a theory should
  expect the same three-way edit.
- **`docs/shape-kind-design.md` carried the argument that created the facade.** Its
  §1.1 justified `Shape : Structural` with "the theory already exists — zero new
  engine", the exact reasoning the audit later rejected. A blind rename would have left
  the sentence "`ConstructorApp` is the theory of Region", newly false. It is rewritten,
  with a sidebar recording why the shared label was wrong — the argument is worth
  keeping visible as a caught mistake rather than silently overwritten.
- **The design doc contradicted itself on `Dim`.** An older passage classified
  `Dim : Structural`; the newer §"`Dim` — the shape-index kind" says the engine is the
  HM core. Since `Structural` no longer exists the stale passage would have named a
  dissolved theory, so it is reconciled to the HM-core position. No `Dim` implementation
  here — that is a later lane; this is only removing a dangling reference.

## Fixtures

Three negatives in `examples/sugars/` (auto-discovered by `test-sugars`, tier 1):

- `kinds_functorial_not_a_theory_err` — `kind Bad : Functorial with bad` ⇒ *unknown
  theory `Functorial`*. The #1270 acceptance criterion, executable.
- `kinds_theory_not_a_law_err` — `protocol Sequence[s: Shape] : AbelianGroup` ⇒
  *unknown protocol law `AbelianGroup`*. The reverse direction, which nothing tested
  before because nothing rejected it.
- `kinds_structural_dissolved_err` — `kind Bad : Structural with bad` ⇒ *unknown theory
  `Structural`*. Pins the dissolution so the name cannot quietly return.

Positive coverage rides existing fixtures, deliberately: `shape_kind_laws`,
`shape_kind_laws_axiom`, `kind_theory_surface`, `kinds_layout_fields`, and
`region_tree_1123` all pass unchanged. That they needed **no** edit is the evidence
the lane wanted — a pure reclassification must not move behaviour, and a green
`shape_kind_laws` proves law-checking still runs after `Functorial` left the catalog
(#1270's "not in scope: do not weaken the law-checking").

## Coverage gaps left

- No fixture asserts a `Region` habitant is rejected in a product (`r1*r2`), because
  nothing rejects it — see the dead-code note. The fixture belongs with the engine that
  makes it pass, in the `Nominal` lane.
- `axiom AbelianGroup` after an impl body now falls through to the top-level `axiom`
  decl parser rather than being claimed as a law clause. Its diagnostic is whatever
  that parser says; not pinned by a fixture. Low value until a second law set exists.

## Follow-ups

- Build `Nominal`'s engine (real region identity / branding). The catalog now names the
  hole.
- `ConstructorApp` and `Nominal` are hardcoded in `is_builtin_theory` while the catalog
  file declares them too — the same mirror-the-list pattern as `base_*_kind_names`. A
  single source of truth for the theory list would collapse the three-way edit noted
  above; out of scope here, worth doing before the catalog grows again.
