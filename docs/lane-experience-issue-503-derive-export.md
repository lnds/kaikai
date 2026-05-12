# Lane experience report — issue #503 (`#derive` breaks pub-type export)

## Goal

Fix #503: a module declaring `#derive(Show) pub type Nota = { ... }`
silently strips `Nota` from the module's export list. Importers see
`module 'thing' does not export type 'Nota'`, with `Nota` missing from
the *available exports* list — even though the same file with
`#derive` removed exports cleanly.

## Scope as planned vs as shipped

**Planned:** preserve `pub` through derive expansion, or register the
DType into the export table before derive runs.

**Shipped:** the second option, in a one-line shape. `collect_pub_exports`
was treating raw `DDerive(names, inner, ...)` nodes as opaque; adding a
single match arm that recurses into the wrapped `inner` decl is enough
for the export collector to see the underlying `DType(true, ...)` and
(if the body is a sum) the constructor names that go with it. No change
to `expand_derives`, the typer, or codegen.

## Diagnosis

Three call sites build module export lists from the *parser output*:

- `resolve_module` for every imported module (line 44685),
- `load_prelude` for every `--prelude` file (line 51064),
- the target file's own self-registration (line 53264).

All three call `collect_pub_exports` on the raw `[Decl]` stream, well
before the typer's `expand_derives` (line 47990) replaces each
`DDerive(names, inner, line, col)` with the bare inner type plus
synthesised `DImpl` decls. So the export collector never gets to see
the post-expansion shape; it has to handle the wrapper directly.

`collect_pub_exports` (line 44511) was a straight match against the
bare decl tags: `DFn`, `DType`, `DEffect`, `DProtocol`, `DAxiom`,
`DUnit`, fall-through `_`. The fall-through swallowed every `DDerive`,
which is how a `pub type` annotated with `#derive` disappeared from
the export list while the same type without the annotation showed up.

The bug was symmetric across protocols: `#derive(Show)`, `#derive(Eq)`,
`#derive(Hash)`, and multi-protocol `#derive(Show, Eq, Hash)` all
reproduced identically. Sum types were equally affected, with the extra
twist that their variant constructors (which `collect_pub_exports` also
emits, per #234) disappeared along with the type name.

## Root cause vs #502

Different mechanism, despite both being "pub type not exported"
diagnostics. #502 was about *constructor names colliding with prelude
ctors* (`Ok`, `Err`, `Some`, `None`) — the typer was qualifying ctor
lookups against the wrong owner and the env's bare-name shadow rules
hid the user's `pub type`. #503 is about the export collector running
before the typer's desugar pass, so the wrapper kept the type
invisible at module-boundary lookup time. The two bugs reach the user
through the same diagnostic ("module M does not export type T") but
share no code path.

## Design decisions and alternatives considered

**(A) Recurse into `DDerive` inside `collect_pub_exports`** (shipped).
Single arm, four lines including comment. Preserves the existing
`DType`/`TBSum` logic — calling `collect_pub_exports([inner], acc)`
runs the same record/sum dispatch, so constructor export for derived
sum types comes "for free". No risk of double-counting because a
`DDerive` cannot wrap another `DDerive` (the parser only accepts a
type decl after `#derive(...)`, enforced at `parse_derive_decl` line
8597).

**(B) Hoist `expand_derives` to run before module registration.**
Rejected. The expansion produces synthetic `DImpl` decls that the
typer's coherence/dispatcher pipeline expects to see after impls have
been resolved against the full prelude. Moving it earlier would force
us to thread the proto registry and core impl table through the
import resolver, which is multiple invasive changes to fix a
one-character mismatch in pattern coverage.

**(C) Preserve `pub` on the synthesised DType node.** Rejected because
the `pub` flag is already preserved — the inner `DType(true, ...)` is
left intact by `expand_derives_loop` (line 46368). The bug is not lost
metadata; it is metadata that `collect_pub_exports` never reads.

## Structural surprises

None worth flagging. The fix sits in the file at the right level of
abstraction, next to the other wrapper-style decls. The most useful
discovery during diagnosis was that `validate_derive_ord` and the
later typer passes already recurse into `DDerive` — every other
walker in the pipeline does. `collect_pub_exports` was simply missing
the arm.

## Fixtures added

- `examples/modules/export_derived_record/` — `#derive(Show)` on a
  pub record, plus a second non-derived pub sum in the same module to
  guard against future fall-through bugs that drop one but not the
  other.
- `examples/modules/export_derived_sum/` — `#derive(Show)` on a pub
  sum with one nullary, one nullary, and one `Crear(String)` ctor;
  verifies the variant names continue to export through the
  `collect_variant_names` branch.
- `examples/modules/export_multiple_derives/` — `#derive(Show, Eq, Hash)`
  on a pub record; exercises the multi-protocol case and uses `==` in
  the importer to make sure the derived `Eq` impl resolves through
  the export-registered type name.

All three are wired into `test-modules-derive-export` in
`stage2/Makefile` (and added to both `test` and `test-fast`), parallel
to the `test-modules-collision` target shipped with #502.

## Coverage gaps

None known. The fix is one match arm; the three fixtures cover the
two type bodies (record + sum) and the multi-derive shape. Negative
fixtures were not added because the bug is not a diagnostic
regression — there is no error path to assert; the fix is purely
about making positive compilation succeed.

## Real cost vs estimate

Estimate: 2–6 hours. Real: about an hour and a half, mostly
bisection and tier verification. The diagnosis pointed straight at
`collect_pub_exports` from the first grep of `does not export`, and
the fix was structurally the same shape as the pre-existing arms.

## Verification

- `make tier0` green — selfhost byte-identical, demos baseline holds.
- `make tier1` green locally — full test suite + demos + fmt + bench
  + check + library-mode + diagnostics-collected.
- `make -C stage2 test-modules-derive-export` green standalone on
  all three fixtures.
- Reproducer from the issue body (`#derive(Show) pub type Nota` plus
  `pub type Comando = Listar | Borrar`, imported from `main.kai`)
  now prints `id=1`.

## Follow-ups left for next lanes

None. The fix is self-contained and additive; no behavioural change
to derive semantics or to non-derived `pub type` export. #504 (Tree
name collision) was queued behind this lane and is unaffected — it
sits in a different part of the typer.
