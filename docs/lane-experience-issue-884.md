# Lane experience — issue #884 (spurious duplicate-constructor diagnostic)

## Scope as planned vs as shipped

**Planned (issue #884):** a valid program declaring its sum type exactly
once (`type Status = Exited(Int) | Signaled(Int)`) was rejected by the
typer's D2 constructor-uniqueness check, which hallucinated a second
declaring type `type Exit = ...` at a bogus source span (`:39:5`, inside
an unrelated function). The brief's leading hypothesis: a use site (a
`match` pattern) was seeding or corrupting a `VariantClaim`, OR the
shared-tag sub-discrimination shape mis-populated a claim's parent/loc
fields.

**Shipped:** the root cause was neither a use-site leak nor field
corruption. The `:39:5` span and the `type Exit` name were *correct* —
they point at `stdlib/effects/os.kai:39`, `pub type Exit = Exited(Int) |
Signaled(Int)`, a core auto-loaded module. The D2 check walks `merged`,
which carries the prelude + every imported module, and registered the
core type's variants as claims that then collided with the user's union.
The diagnostic only *looked* like garbage because `report_d2_collision`
renders both the previous and current decl with the root file's path —
so a real `os.kai:39:5` claim printed as `<user-file>:39:5`. The fix is a
one-predicate guard: only root-file unions seed and test D2 claims.

## Where the bogus claim was seeded

`validate_union_collisions_loop` (`infer.kai`) iterated every `DType` in
`merged` and registered each `TBSum`'s variants as a `VariantClaim`,
without consulting the decl's `module_origin`. `effects/os.kai` is in the
core auto-loaded set (`core_module_files`, `driver.kai`), so `Exit`'s
variants `Exited`/`Signaled` were already in the claims table by the time
the loop reached the user's `Status`. A user reusing those names tripped
the `_ ->` (real-D2) arm — not the builtin-sentinel arm, because `Exit`
is a real stdlib type with a real source span, not an `Option`/`Result`
builtin pre-seeded with sentinel `(0,0)`.

## Why the shared-tag shape was a red herring

The brief tied the bug to the shared-tag sub-discrimination shape
(`Exited(0)`/`Exited(1)`/`Exited(_)` + catch-all) and a nearby `Option`
match. Bisection killed both: the bug fires on the two-line minimal
program `type Status = Exited(Int) | Signaled(Int)` + `fn main` — no
match, no shared tag. It is purely name-driven: any user union whose
variant names coincide with a core type's variants. `Foo = Bar | Baz`
compiles clean; `Status = Exited | Signaled` does not, only because
`os.Exit` already claims those names. This is exactly the "ctor
overloading at use sites" case the code already declared ALLOWED for
builtins — extended (incorrectly omitted) to stdlib core types.

## The fix

`stage2/compiler/infer.kai` only. Two changes:

1. `validate_union_collisions_decls` derives `target_mod =
   inf_prelude_module_name(file)` (the root file's stamped
   `module_origin`) and threads it through the loop.
2. `validate_union_collisions_loop` processes a `DType`'s `TBSum` only
   when `is_root_decl_origin(origin, target_mod)` — `origin == None` or
   `origin == Some(target_mod)`. Core/imported types (`Some("os")`,
   `Some("option")`, …) are skipped entirely: they neither seed nor test
   claims.

The subtlety that broke the first attempt: the root file's own DTypes are
*not* `module_origin == None`. `tag_target_decls_module_origin`
(`driver.kai`) stamps every root DType with `Some(target_mod)` (the
basename) so per-module record/sum resolution treats them as a named
scope. A naive `DType(..., None)` guard therefore matched nothing and
silently disabled D2 entirely — `d2_collision` stopped firing. The
correct root predicate is "unstamped OR stamped with the root module's
own name", mirroring `is_root_origin` in `driver.kai` but parameterised
by `target_mod`.

`builtin_variant_claims()` (the `Option`/`Result` sentinel pre-seed) is
untouched — it lives outside the loop and still guards the
user-reuses-`Ok` case via the `VC(_, _, 0, 0)` arm.

## Proof the real D2 collision still fires

`make test-unions` green (exit 0):

- `unions OK d2_collision (rejected as expected)` — `type A = Foo | Bar`
  / `type B = Foo | Baz` (both root) still trip D2, now with correct
  spans for both decls (`12:1` and `13:1`, both in the user file).
- `unions OK d2_no_collision` — the pre-declare escape hatch still works.

The fixture `examples/perceus/match_shared_tag_subdiscrimination.kai`
compiles under the C backend, runs, and its stdout matches
`match_shared_tag_subdiscrimination.out.expected` exactly.

## Fixtures added / coverage

No new fixture needed — `match_shared_tag_subdiscrimination.kai` (already
in `examples/perceus/` with an `.out.expected`) is the positive
regression fixture, auto-discovered by `test-backend-parity.sh` and the
perceus test harness. `examples/unions/d2_collision.{kai,err.expected}`
remains the negative fixture for genuine user-vs-user D2 and stays red
for the right reason. Coverage gap closed: before this lane, no fixture
exercised "user union variant name coincides with a core stdlib type's
variant" — the perceus fixture now does, since `os.Exit` is the colliding
core type.

## Cost vs estimate

Single-predicate typer fix. The investigation cost was front-loaded: the
bogus span made it look like memory corruption until grepping the stdlib
for the exact variant names (`Exited`/`Signaled`) surfaced `os.Exit`. The
one rework was the `None`-vs-`target_mod` origin distinction, caught
immediately because `d2_collision` stopped firing on the first rebuild.

## Follow-ups left for next lanes

- `report_d2_collision` renders the *previous* claim's location with the
  root file's path, not the declaring module's. For a genuine root-vs-root
  D2 this is correct (both decls are in the root file), but if D2 is ever
  extended to cross-module collisions the path must come from the claim,
  not from `file`. Out of scope here.
- This bug was masked on the parity ratchet by the flaky-recheck (#885);
  with the fix in, one of the two failures that recheck was hiding is
  gone.
