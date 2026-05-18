# Lane experience — Hanga Roa pub-leak validator (2026-05-18 → 19)

## Scope as planned

The session brief was titled "Hanga Roa import-filter lane" and
laid out a six-step plan that read, in essence:

  1. Add `validate_module_pub_signatures` — intra-module catch
     for a `pub` decl whose signature mentions a non-`pub`
     neighbor.
  2. Filter `expand_imports` so only `pub` decls cross the
     module boundary into the consumer's decl stream.
  3. Retire the cross-module walking of `validate_pub_access`
     (filtering at step 2 would have made it redundant).
  4. Retire the cross-module bucket-gating of
     `validate_unit_refs` for the same reason.
  5. Add negative fixtures by category (return / params / row /
     record field / variant payload / effect op / protocol op).
  6. Author the `.err.expected` for the existing
     `m12_5_uom_pub_leaks_private.kai` once step 1 enabled the
     rejection.

The user expected a five-step closeout (`/clear` had freed
prior context) with one overnight autonomous run.

## Scope as shipped (five commits over 7e29888)

  58d4eb1  feat(typer): validate_unit_refs hygiene pass — declared global
  ef0f1a1  feat(typer): validate_module_pub_signatures (step 1)
  db095d2  feat(typer): extend validate_pub_access to walk decl signatures
  a434c38  test(sugars): pub-signature leak fixtures by category
  170fed0  test(modules): cross-module privacy fixtures

The plan was reoriented mid-session — see "Design decisions"
below. Final state: privacy is enforced by two cooperating
post-typer passes, not by filtering at the import boundary.

## Design decisions and alternatives considered

### Filtering at the import boundary was the wrong tool for kaikai

The original step 2 was: in `process_imports`'s `resolve_module`
hook, after typechecking `lib.kai` in isolation, drop every
non-`pub` decl before appending to `rs.decls`. Then a consumer's
merged stream would simply not contain `lib`'s privates, and any
reference by name would surface as "cannot find" — the same
diagnostic users get for typos.

This is what `.hi` / `.cmi` / Rust re-exports do, and it was the
brief's preferred approach.

**Why it does not work in kaikai today**: the merged stream
flows to **both** the typer and the codegen. The body of `pub
fn foo` in `lib` calls a non-`pub` helper `_bar` in `lib`; that
helper has to be present in the codegen output that compiles
the consumer's binary, because the binary links one flat C TU.
Filtering at the import boundary would drop `_bar` and leave
`foo`'s body referencing a missing symbol.

Haskell / OCaml resolve this by emitting one TU per module and
linking afterwards; bodies of `pub fn foo` never travel
syntactically into the consumer's typing context — only the
interface does. kaikai compiles to a single flat C / LLVM TU
today, so the body has to be in the merged stream.

The architectural change to two views (typer-visible vs
codegen-visible, à la SML signatures with structures) is a much
larger lane. It deserves its own design doc and PR. Out of
scope for this session.

### Revised approach: enforce at name resolution, not at stream membership

Two post-typer walkers:

  - `validate_module_pub_signatures` (intra-module, new). For
    each `pub` decl in module M, walk every `TyName` / `USym` /
    row label in its signature; if the name lookup table records
    it as a non-`pub` neighbor of M, flag the leak at the
    surface position.

  - `validate_pub_access` (cross-module, extended). The existing
    body walker already routed every `EVar(nm)` reference
    through `pae_decide`. Extending it to walk signatures was
    structurally the same code shape — params, return, row,
    record fields, variant payloads, effect ops, protocol ops,
    `TyDim(_, USym(s))` units. Reused `pae_decide` verbatim so
    same-home and pub-visibility rules match the body walker
    by construction.

Together they close the leak class without touching the stream
shape downstream passes depend on. The user-facing diagnostics
are at the right module (the authoring module for intra-module
leaks; the consuming module for cross-module references), which
is what the filtering-approach was supposed to deliver
ergonomically.

### `validate_unit_refs` simplification (step 4 done early)

`validate_unit_refs_decls` shipped on the working tree with a
per-bucket `UnitVis` table (priv/pub split per home module).
That was step 4's removal target. The simplification —
collapse to a flat `declared: [String]` list with no home
gating — fell out naturally when the new model emerged: cross-
module unit privacy is enforced by `validate_pub_access`'s
signature walker, so this pass only needs to catch "user wrote
`Real<USD>` without declaring `unit USD` anywhere". 87 lines of
bucket-gating retired in 58d4eb1.

### `decl_explicit_home` extended to read DType anchors

The pub-leak validator's home-attribution was reporting `array`
(the last preloaded core module) as the home of root-file decls
that opened with a `type` declaration. Trace: `tag_target_decl`
already stamps every root-file DType with `module_origin =
Some(<target_mod>)`, but `decl_explicit_home` was only reading
the `mo` field of DFn / DAxiom / DImpl. A root file that opened
with a `type` had no DFn ahead of it to seed
`propagate_home_back`, so the running `cur_mod` stayed at the
last preloaded prelude.

Fix in `a434c38`: extend `decl_explicit_home` to read
`DType`'s `mo` field too. Symmetric to the existing DFn arm,
no semantic surprise. Caught by the
`m12_5_pub_leak_type_record_field` fixture which reported the
wrong module name in its diagnostic.

### `DImpl` is not a leak surface intra-module

`vmps_decl`'s DImpl arm initially walked the target type and
every method's signature, treating the whole impl as
pub-equivalent. `demos/blackjack` has an `impl Show for Rank`
over a non-`pub Rank`; the validator flagged the impl method's
`r: Rank` parameter as a leak.

The actual semantics: an `impl P for T` does not expose `T` as
a syntactic reference to other modules — the protocol
dispatcher resolves the impl by value type at the call site,
not by name. Cross-module references to `Rank` are caught by
`validate_pub_access`'s signature walker (the cross-module
counterpart). Demoted the DImpl arm to a no-op. The helper
`vmps_impl_methods` became dead code and was removed in the
same commit (a434c38).

## Structural surprises the brief did not anticipate

  - The architecture of one flat merged stream (typer +
    codegen sharing the same `[Decl]`) made the brief's
    "filter at import" approach unworkable. Anyone authoring
    that brief in the future should know: stage 2 today does
    not separate interface from implementation. The retrofit
    is bigger than a one-session lane.

  - `demos/free_fall` references `unit m` and `unit s`. The
    working tree's `validate_unit_refs_decls` bucket-gating
    bucketed those units under the wrong home (the last
    preloaded core module) because home-anchor propagation
    skips DType / DUnit. tier0 caught this immediately —
    `free_fall` fixture failed compile. The bucket
    simplification unblocked it without losing semantic
    coverage.

  - `decl_explicit_home`'s asymmetric treatment of DType vs DFn
    is not documented as a known foible anywhere. The extension
    in a434c38 is small but suggests a broader audit: do any
    other `home_anchor`-using passes silently mis-attribute
    decls coming from a root file that opens with `type`? Out
    of scope for this lane; opened as a mental TODO.

  - `examples/minimal/interp.kai` had `type Expr` non-pub +
    `pub fn eval(e: Expr)`. Once step 2 caught signature
    references, this surfaced as a leak. Changed `Expr` to
    `pub type` rather than dropping the `pub` from the
    functions — the eval/show pair is the file's surface API.
    A pre-existing fixture, not authored by this lane.

  - test-intervals (basic), test-stdlib (crypto subpath),
    test-protocols (add_user_record), test-shadowing
    (private_nbfragr family), selfhost-llvm: every one fails
    in baseline 7e29888 too. **None are regressions from this
    lane.** They appear to be fallout from the `--prelude` /
    auto-load transition (7c1bb4b) and the modular typer rework
    (af1656e), both of which preceded this session. Worth a
    separate sweep; not blocking lane close.

## Fixtures added

Intra-module (sugars/, exercised through `test-sugars`):

  m12_5_pub_leak_fn_return_type     pub fn return — priv type
  m12_5_pub_leak_fn_row_effect      pub fn row — priv effect
  m12_5_pub_leak_type_record_field  pub record field — priv type
  m12_5_pub_leak_type_sum_variant   pub variant payload — priv type
  m12_5_pub_leak_effect_op_param    pub effect op — priv type
  m12_5_pub_leak_protocol_op        pub protocol op — priv type
  m12_5_uom_pub_leaks_private       pub fn param — priv unit (already
                                    on tree; .err.expected added here)
  m12_6_uom_undeclared_unit         pre-existing fixture from earlier
                                    session work, .err.expected added

Cross-module (modules/, exercised through
`test-modules-uom-privacy`):

  pub_type_used_in_main             positive: pub type visible
  uom_pub_import                    positive: pub unit visible
  uom_private_unit                  negative: priv unit at use site
  priv_type_used_in_main            negative: priv type at use site
  priv_effect_used_in_main          negative: priv effect at use site

Coverage gap: positive fixtures for `pub effect` visible across
modules and `pub protocol` visible across modules are not yet
authored. The mechanism is the same as `pub type` (the
cross-module walker checks `pae_decide` for any name), so
correctness is already exercised; a follow-up lane can pin them
for narrative completeness.

## Real cost vs estimate

The brief estimated the lane as "five-step closeout" with the
implied scope of an overnight run. Real outcome:

  - Step 1 (validate_module_pub_signatures) shipped as planned.
  - Step 2 reoriented from filter to walker; same outcome,
    different architectural cost.
  - Step 3 cancelled (validate_pub_access not retired —
    extended instead).
  - Step 4 done early (in 58d4eb1, alongside step 0 cleanup of
    the working tree).
  - Step 5 + 6 (fixtures + golden) shipped.

Total: five commits, ~700 lines of compiler changes, ~150 lines
of fixture infrastructure, lane retro at ~250 lines.

## Follow-ups left for next lanes

  - Stage 2 today has no interface-vs-implementation split.
    The brief's filter-at-import idea is the right shape for a
    future lane that introduces that separation; it would
    enable smaller per-module rebuilds and cleaner cross-module
    privacy by construction. Will need a separate design doc.

  - Pre-existing test fallout: `test-intervals` (basic),
    `test-stdlib` (crypto), `test-protocols` (add_user_record),
    `test-shadowing` (private_nbfragr family), `selfhost-llvm`.
    Audit which of these are real regressions vs intentional
    after the `--prelude` retire and the modular typer.

  - The cross-module walker's diagnostic does not yet name the
    consumer's `import` statement that brought the offending
    type into scope; an Issue may want this UX polish.

  - Positive cross-module fixtures for `pub effect` and `pub
    protocol` to mirror `pub_type_used_in_main`.
