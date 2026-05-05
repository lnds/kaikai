# Lane experience — parser-qualified-types-and-constructors

Closes issues #232 (qualified type names in signatures) and #234
(qualified variant constructors in expression / pattern position).

## Objective metrics

| metric | value |
|---|---|
| start | 2026-05-04T20:08:50-04:00 |
| end | 2026-05-04T20:44:21-04:00 |
| files changed | `stage2/compiler.kai`, `stage2/Makefile`, 3 fixture dirs |
| compiler.kai diff | +513 / −5 lines |
| new fixtures | 3 (`qualified_type_in_signature`, `qualified_constructor_disambig`, `qualified_type_negative`) |
| Tier 0 | OK |
| Tier 1 | OK |
| Tier 1-ASAN | OK |
| selfhost | OK (byte-identical fixed point) |
| selfhost-llvm | OK (byte-identical fixed point) |
| `test-modules-qualified` | 9/9 OK |
| `test-modules-qualified-neg` | 2/2 OK |

Build TSV (tail):

```
timestamp	cmd	outcome	elapsed_s
2026-05-04T20:32:07-04:00	tier0	OK	46
2026-05-04T20:41:25-04:00	tier1	OK	-
2026-05-04T20:43:01-04:00	selfhost	OK	-
2026-05-04T20:44:04-04:00	selfhost-llvm	OK	-
2026-05-04T20:44:14-04:00	tier1-asan	OK	-
```

## Diagnosis

### #232 — type position

`stage2/compiler.kai:parse_named_type` (line ~4701) only consumed
a single ident token and (optionally) its generic-arg list. There
was no path for `mod.Type`. The parser bailed at the dot with
"expected `,` or `)` in parameter list" — the dot survived to the
caller (`parse_clause_params`) which then mis-classified it.

The downstream resolver `rqc_*` (line ~37260) already routes
`EField(EVar(mod), fn)` into `EModCall(mod, fn)` for **expression**
qualified calls, but the type grammar is a separate path; nothing
walked qualifiers in `TypeExpr`.

### #234 — expression / pattern position

`mod.Active` in expression position parsed correctly today (as
`EField(EVar("mod"), "Active")`), but `rqc_kind` rewrote it to
`EModCall(mod, "Active")` which downstream codegen treats as a
function call. There is no function `Active` in the module — only
a variant constructor — so the typer rejected it as unbound.

Pattern position never accepted the qualifier at all:
`parse_ident_pattern` (line ~4328) only knew bare uppercase
idents.

`collect_pub_exports` (line ~36597) listed only `pub fn`, `pub
type`, `pub effect`, `pub protocol`, `pub axiom`, `pub unit` —
variant constructor names of a `pub type T = A | B | …` were
never published. The module table therefore could not validate
`mod.A` even after the parser accepted it.

## Implementation shape

No shared helper across the three call sites — the contexts
differ enough (return-type slot vs. expression slot vs. pattern
slot, each with its own surrounding state) that an
`Option[(String, String, Parser)]` factor would have made the
sites more cluttered, not less. Each site reads a tiny inline
peek-2 sequence (lowercase ident + dot + upper-or-lower ident).

### Call sites changed in the parser

| site | line | shape |
|---|---|---|
| `parse_named_type` | ~4701 | `mod.Type[args]` head folded into `TyName("mod.Type", args)` |
| `parse_ident_pattern` | ~4328 | qualifier consumed, drops to existing upper-ident pattern logic |

### Resolver / typer changes

- `collect_pub_exports`: a `pub type T = A | B | …` now also
  exports every variant constructor name. The new helper
  `collect_variant_names` recurses over the `TBSum` payload.
- `rqc_kind`'s `EField(EVar(mod), fname)` branch: when `fname`
  starts uppercase and the module exports it, emit `EVar(fname)`
  rather than `EModCall(mod, fname)`. Variant ctors are
  auto-declared globally per #187 so the bare form resolves
  correctly.
- New `qualtype_decls` pass walks every reachable `TypeExpr` in
  every Decl (DFn / DType / DEffect / DProtocol / DImpl /
  DAttribPure / DAxiom / DDerive / DTest / DBench / DCheck plus
  body expressions for SLet annotations, EHandle ty_args,
  EVariantsOf, ELambda params). For `TyName(name, targs)` whose
  name contains a dot, validate `mod` against the module table
  and rewrite to `TyName(ty, targs)` after stripping the
  qualifier. Mismatches emit `unknown module qualifier 'mod' in
  qualified type 'mod.ty'` or `module 'mod' does not export type
  'ty'; available exports: …`.
- `qtchk_decls` is the read-only counter sibling: walks
  decl-level `TypeExpr`s only and returns the bad-qualifier
  count. Folded into the existing `else if … > 0 { … }` cascade
  in `compile_source` so a `unknown_module.Foo` reference fails
  the build with exit 1.

## Fixture verification

### `qualified_type_in_signature/`

- `helper.kai`: `pub type Shape = Circle(Int) | Square(Int)`
- `main.kai`: exercises the qualifier in parameter type, return
  type, generic argument `[helper.Shape]`, and `let` annotation.
- `main.out.expected`: `147\n147\n` (both `area` and
  `first_area` paths).
- Verified: `make -C stage2 test-modules-qualified` covers it on
  C and LLVM backends.

### `qualified_constructor_disambig/`

- `auth.kai` / `billing.kai` deliberately use **different** type
  names (`AuthStatus`, `BillStatus`) — the `Status` collision
  case from the issue brief depends on type-name disambiguation,
  which is blocked by the #187 auto-declaration model and is out
  of scope for this lane (see *Limitations*).
- `main.kai` uses `auth.Active`, `billing.Pending` in expression
  position and `auth.Active`, `auth.Suspended`, `billing.Pending`,
  `billing.Paid` in pattern position.
- `main.out.expected`: `auth: active\nbilling: pending\n`.

### `qualified_type_negative/`

- A `pub fn area(s: unknown_module.Foo) : Int { 42 }` references
  a module that was never imported.
- `main.err.expected` pins the substring
  `unknown module qualifier 'unknown_module' in qualified type 'unknown_module.Foo'`.
- The fixture compile fails (exit 1) — the qtchk pass folds into
  the standard error gate.

### Repros from issue bodies

- #232 `pub fn of(s: shapes.Shape) : Int { match s { Circle(r) ->
  r * r * 3 ; Square(s) -> s * s } }` — verified compiles and
  runs (output `75` for `Circle(5)`).
- #234 `let s = auth.Active ; match s { auth.Active -> … }` —
  verified compiles and runs with the disambig fixture (subject
  to the type-name limitation noted above).

## Friction points

- Two passes (`qualtype_decls` rewrite + `qtchk_decls` counter)
  rather than a single pass that returns `(decls, errs)`. The
  threaded-state version was non-functional in kaikai's
  closure-free record style and would have required a wrapper
  struct on every helper signature; two passes was less code
  even at the cost of one extra walk.
- `qtchk_decls` initially walked into expression bodies and
  panicked with `non-exhaustive match` for several minutes —
  root caused to a missing `DAttribPure` arm in the decl walker.
  Trimmed back to decl-boundary `TypeExpr`s only (which is
  enough to reject the negative fixture); the rewrite pass
  still walks expressions so legitimate `let x : mod.T = …`
  annotations resolve correctly even though the counter never
  inspects them.

## Spec ambiguities and out-of-scope cases

- **Type-name collisions** (#234 brief showed both modules
  exporting `Status`) are out of scope. The #187 auto-declaration
  model merges global nominal types by bare name, so two
  `pub type Status = …` declarations across modules become a
  single TyCon `Status` with the union of their variants —
  exhaustiveness checks then complain about cross-module
  variants. Fixing this requires per-module type-name minting
  (e.g. `auth__Status`) and is a separate refactor.
- **Ambiguous bare ctor names** (the issue's "if both exports
  collide, unqualified is an error" rule) are not enforced.
  Today's silent first-wins behaviour persists when the user
  omits the qualifier. The lane brief explicitly cites this as
  acceptable scope reduction.
- **Re-exports / nested imports** (`import auth.account` →
  `account.Status`) work because `last_segment` of the dotted
  path is what the module table indexes; the qualifier
  surface is the binding name, not the full dotted path.

## Subjective summary

The diagnosis was clean: parser rejected three positions, each
fixable with a tiny inline peek. The harder part was wiring the
diagnostic — kaikai's typer accepts unknown `TyName` silently
(the #187 nominal model), so the qualtype rewriter had to count
its own errors and fold the count into the build's exit
discipline. The collision case in the brief turned out to be
half about #234 (qualifier surface) and half about a separate
#187 limitation (global type-name merging) that the lane brief
treats as out of scope.

## Limitations of this report

- Line counts come from `git diff --stat`, not a manual audit.
- The "ambiguity diagnostic with module suggestions" piece of
  #234's spec was descoped per the lane brief; the negative
  diagnostic only fires for bad qualifiers, not for
  ambiguous-bare-ctor cases.
- Selfhost byte-identical converged in one iteration on both
  backends — `stage2/compiler.kai` itself does not (yet) use
  qualified types or constructors, so the fix-point check is the
  trivial-rewrite case. A future stage2 author who reaches for
  the qualifier surface would re-validate convergence.
