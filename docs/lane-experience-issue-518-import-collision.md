# Lane experience — issue #518 (import + duplicate type-name collisions)

## Scope as planned

Close the import-path half of #504 (type-name shadowing). The #504
fix landed via #510 (`pub` enforcement) and #502 (constructor-name
validator). Both targeted the *variant* axis — `Option.Some`,
`Result.Ok` baked into the builtin claims table. The *type-name*
axis was left open: declaring `type Tree = Leaf | Node(...)` in a
user file while an imported (or `--prelude`-loaded) module exports
`pub type Tree[k, v]` was silently accepted. The typer's variant
lookup walked both definitions non-deterministically and reported
match-exhaustiveness errors against the *other* file's constructors.

The brief also folded in the sibling silent contract caught by the
#511 audit: two `type T = ...` declarations in one module compiled
cleanly and the typer picked one of them at random.

## Scope as shipped

Identical to the brief. Three behaviours close in one validator:

1. User `type T` whose name collides with a *prelude* module's
   exported `pub type T`.
2. User `type T` whose name collides with an *imported* module's
   exported `pub type T`.
3. Two `type T = ...` declarations in the same root file.

All three reject with a diagnostic pointing at the user's `DType`
line/col; the message distinguishes prelude vs import vs in-file
sibling, and the help text suggests the appropriate fix (rename
locally, drop the import, or remove the duplicate decl).

## Diff between the prelude case and the import case

Worth pinning because the brief framed them as separate paths and
they aren't, quite: the same `module_table` carries both.

- `bin/kai` loads every stdlib file (`collections/map.kai`,
  `protocols.kai`, …) with `--prelude <path>`. `load_preludes`
  registers each file as a `ModuleEntry` and accumulates a
  `[PreludeSegment]` so per-file boundaries survive the flat
  decl append.
- An explicit `import lib` is processed later by `expand_imports`,
  which appends one `ModuleEntry` per imported file to the same
  table.
- `compile_source` concatenates `prelude_mods` ++ import-graph ++
  target-ME into a single `module_table` (line ~54219). Downstream
  passes (qualified-type rewrite, `me_has_export` for `EModCall`,
  pub-access validator) all read this combined list — they don't
  distinguish prelude vs import either.

So the **fix** is one validator (`validate_type_name_collisions_decls`)
that walks `root_only_decls` (i.e. the post-#510 split: the tail of
`qualified_decls` after `pae_drop(qualified_decls, n_imports_raw)`)
and asks each `DType` two questions:

1. Is this name already in my in-flight claim list? → duplicate
   in-file decl.
2. Else, does any other `ModuleEntry` in the table export this
   name? → cross-module collision.

The **only** prelude-vs-import distinction is cosmetic, used to
shape the diagnostic. A helper (`me_list_has_name(prelude_mods, …)`)
checks whether the colliding module is in the prelude list and
picks the wording accordingly.

## Why Approach 1 (reject) over shadowing

Two pieces of precedent and one of judgment:

- **#502 (constructor-name validator)** and **#510 (`pub`
  enforcement)** both ship strict-reject. Adding the type-name
  axis as reject keeps the user model coherent: a colliding name
  is a static error, regardless of which axis (type vs variant vs
  fn) collides.
- **The runtime cost of shadowing is real.** The variant-table
  lookup that drove the original symptom is keyed by bare type
  name. Real shadowing would require scope-aware lookups in
  exhaustiveness, in qualified-type resolution, in the typer's
  `TyName` instantiation. That's a larger surface than #518
  warranted.
- **No user demand for shadowing surfaced.** The issue and the
  #511 audit both phrase the contract as "collisions should be
  rejected". Defer the shadowing question to post-1.0 if real
  demand appears; today, the rejection diagnostic is precise
  enough to fix the user's program in seconds.

## Structural surprises

- **`DType` has no `module_origin` field.** Unlike `DFn`, which
  carries `Option[String]` since m6.2 v2, `DType` (and `DEffect`,
  `DProtocol`) are positional — their home module is inferred from
  the decl stream's split into prelude segments + import segments +
  root tail. This is why the validator takes `root_only_decls` as
  a direct argument rather than walking `merged` with a stamp filter.
  Adding `module_origin` to `DType` would have rippled through ~20
  match arms; the positional split was the cheap path.
- **`bin/kai`'s default `--prelude` set already includes
  `collections/map.kai`,** which exports `pub type Tree[k, v]`.
  That means the original silent_contract fixture (the one this
  lane closes) was actually demonstrating *two* bugs at once:
  the prelude `map.Tree` was leaking into match arms AND the
  imported `lib.Tree`. The promoted fixtures rename to
  `Widget` / `Gadget` / `Foo` to keep each fixture exercising
  exactly one path.
- **`tools/test-negative.sh` runs `kaic2` directly,** without the
  `bin/kai` wrapper, so it sees no default preludes. The prelude
  fixture (`prelude_collision.kai`) uses the harness's optional
  `<stem>.prelude.kai` sibling to load one prelude on demand.
  This is the same pattern the existing pub-enforcement fixtures
  use (`violation_prelude/secret_lib.kai`).
- **Module name from `--prelude <path>` is the basename without
  the `.kai` extension** (via `prelude_module_name`). The fixture's
  `.err.expected` therefore expects the module name
  `prelude_collision.prelude`, not `prelude_collision`. The
  trailing `.prelude` is part of the test sibling's filename
  convention, not the rejection logic.

## Fixtures added

Promoted from `examples/negative/silent_contract/` into a new
`examples/negative/type_name_collision/` directory:

| Fixture | Path | Asserts |
| --- | --- | --- |
| Import collision (multi-file) | `import_collision/{lib,main}.kai` | `import lib` exports `pub type Widget[k, v]`; user `type Widget` rejected |
| Prelude collision (single-file + sibling) | `prelude_collision.kai` + `.prelude.kai` | user `type Gadget` rejected against the auto-loaded prelude's `pub type Gadget` |
| Duplicate decl (single-file) | `duplicate_type_decl.kai` | two `type Foo = ...` in the same file rejected at the second |

All three land green in `tools/test-negative.sh`. The
`silent_contract/README.md` row for #518 is removed per the
migration recipe pinned there.

## Coverage gaps deferred

- **Effect-name and protocol-name collisions** follow the same
  pattern (`DEffect` / `DProtocol` carry no `module_origin`, the
  typer's name lookup is bare). Out of scope for #518; the audit
  hasn't surfaced a real user-facing symptom yet. Open a follow-up
  if `import foo; effect Bar { ... }` produces a silent collision.
- **Same-name collision across two preludes** (two stdlib files
  both export `pub type Foo`) is theoretically caught by the same
  walker if we ran it over the prelude segments too. The current
  pass walks only the root file, on the assumption that the
  stdlib is internally consistent. If a future bug-bash adds a
  duplicate stdlib type, a sibling walker would catch it cheaply.

## Real cost vs estimate

- Estimated: 1–2 days.
- Actual: ~3 hours. The strict-reject path is mechanical once the
  hook location is identified (next to `validate_pub_access`,
  same decl-split as #510's pub gate). No typer-internal changes;
  no runtime changes; no doc-catalog flips needed (the closing
  PR doesn't move any "planned" marker in `docs/stdlib-roadmap.md`
  because #518 isn't a stdlib feature).

## Follow-ups for next lanes

- If users complain about the rejection for legitimate "I want to
  shadow this stdlib type" use cases, revisit shadowing semantics.
  The validator hook is the single edit point — flip from
  diagnostic to silent and lookups go from "first match in
  module_table" to "root-file first then module_table" everywhere
  in the variant-table read path. That's a bigger lane.
- Consider extending `DType` (and `DEffect`, `DProtocol`) with the
  same `Option[String] module_origin` field that `DFn` carries.
  Would simplify several downstream walkers that currently
  reconstruct origin via positional splits. Pure refactor, no
  semantic change; estimate ~half a day.
- The `tools/test-negative.sh` `--prelude` flag uses the sibling
  filename convention `<stem>.prelude.kai`. The prelude module's
  surface name therefore includes `.prelude` (`prelude_collision.prelude`).
  Minor cosmetic wart in the diagnostic; not worth a fixup if the
  fixture's intent is clear.
