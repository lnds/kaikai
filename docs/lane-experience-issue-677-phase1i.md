# Lane experience — issue #677 Phase 1i (name-resolution extraction)

**Lane branch:** `resolve-extract`
**Closes:** issue #677 Phase 1i (first of the "easy three": resolve / fmt / driver)
**Predecessor:** Phase 1h (PR #684, parser extraction into `compiler/parse.kai`)

## Scope as planned

Move the post-parse name resolution + checker section (the
`# checker: name resolution` block, originally lines ~713–1759 of
`stage2/main.kai`, ~1050 LOC) into a new `compiler/resolve.kai`, with
full module header, single-line `#[doc]` on every `pub` decl, a
per-module test file, and this retro. Wire the module into
`BUNDLE_SRCS` before `main.kai` and `import compiler.resolve` into
`main.kai`. Serialised intentionally because resolve sat at the *top*
of the residual `main.kai`, so its removal renumbers everything below
it — fmt + driver lanes follow once this merges.

## Scope as shipped

Matches the plan, with one cross-section refinement (below). Final
shape of `compiler/resolve.kai`:

- **63 top-level decls** (61 fns + 2 types: `Env`, `EffArity`).
- **9 `pub`** (1 type `Env`, 8 fns: `env_new`, `add_all`,
  `prelude_names`, `register_decls`, `chk_decls`, `last_segment`,
  `check_program`, `collect_effect_names`).
- **54 internal** (the scope walkers, the arity table, the row-label
  check, the tyvar collectors).
- **1095 LOC** including the ~85-line module header.

`main.kai` dropped from 60 238 → 59 24x LOC.

`stage2/tests/test_resolve.kai`: **15 unit tests + 3 property checks**.

## Cross-section analysis (the load-bearing finding)

Following the Phase 1h discipline (don't extract blindly — verify the
section is self-contained via a callee graph), the scan surfaced **two
outsiders co-located with the resolver that must NOT move**:

1. **`row_effects_without_default_handler`** calls
   `effect_default_block_all_extern`, which is defined in `main.kai`'s
   default-handler-wiring section (line ~9742) and is *also* called
   from the emit path (line ~29901). Its own two real callers
   (`main_row_labels` sites) are both in `main.kai`. It is
   default-handler analysis, not name resolution — it just happened to
   sit next to the resolver because the original author grouped it
   with the row-label check. **Moving it would create a resolve↔main
   import cycle** (resolve would need a function from main, while main
   imports resolve). Left in `main.kai`.

2. **`collect_type_names`** is referenced *only* by
   `inject_builtin_effects` in `main.kai` and is not used by the
   resolver at all. No cycle risk, but it belongs with its sole
   caller, not in the resolver's public surface. Left in `main.kai`.

Had I extracted the literal line range `[713..1759]` without the
callee graph, both would have moved and the build would have failed
on the cycle — exactly the failure mode the cross-section discipline
exists to catch.

A third near-miss: **`lookup_effect_op_names`** (and its helper
`effect_op_names`) has **zero callers anywhere** — it is dead but
exhaustive code (issue #308 scaffolding). It moved with the resolver
because it is genuinely effect-decl introspection that belongs in the
resolution layer, and kaikai does not flag unused top-level fns as
errors. Kept for now; a future dead-code sweep can decide its fate.

## Design decisions

- **Public surface kept minimal.** Of 63 decls, only the 9 with real
  external callers (the driver's `check_program` seeding path + the
  module resolver's `last_segment` + the effect-injection/typer path's
  `collect_effect_names`) are `pub`. The `Env` *type* is `pub` because
  it appears in the signatures of pub fns (`env_new`, `add_all`,
  `register_decls`, `chk_decls`); its record fields (`scopes`,
  `had_error`, `file`, `diags`) are read directly in the tests via
  projection, which kaikai permits on any in-scope pub record.

- **Imports.** `compiler.util` (`list_has`, `concat_all`),
  `compiler.diag` (`Diagnostic`, `Severity`/`SevError`, `mk_diag`,
  `ri_help`, `closest_name`, `diag_error`/`note`/`help`),
  `compiler.ast` (every AST type the walkers match on),
  `compiler.refinements` (`check_call_site_refinements`, folded into
  `check_program`). No import of `compiler.lex` was needed — the
  resolver consumes the parsed AST, not tokens.

- **Test strategy: parser-driven over hand-built AST.** Rather than
  construct `[Decl]` nodes by hand (verbose, and couples the test to
  every constructor's slot order), the end-to-end tests parse real
  source via `tokenize` → `parse.parse_program` and feed the resulting
  AST to `register_decls` / `chk_decls` / `collect_effect_names` /
  `check_program`. The pure helpers (`env_new`, `add_all`,
  `last_segment`, `prelude_names`) are tested directly. Internal
  walkers are covered transitively through `check_program` and
  exhaustively during every selfhost.

## Structural surprises

- The original section was **not** a clean `[713..1759]` block: the
  two outsiders sat in the *middle* of it (lines 1462–1497), between
  the row-label check and the unbound-tyvar check. The extraction had
  to splice them back out and leave them in place, rather than a
  single contiguous cut. The Python splice (delete 713–1759, reinsert
  the 36-line outsider block + a redirect comment) was cleaner than a
  sequence of fragile `Edit` calls across shifting line numbers.

- The `Env` resolver type **collides by name** with the `Env` effect
  (the environment-variables capability) and with documentation
  examples like `type Env = [(String, Real)]`. The grep-based caller
  scan had to be careful: 70 raw `Env` hits in `main.kai`, almost all
  of which are the effect or doc text, none of which reference the
  resolver type. Confirmed the resolver `Env` has no external type
  references before deciding `pub` was needed only for signature
  leakage.

## Fixtures / coverage

- `stage2/tests/test_resolve.kai` (15 tests + 3 checks). Not yet
  wired into a Tier 1 gate per the standing #452 / #677 Phase 2
  deferral of per-test-file cost; runs under `kai test stage2/.` and
  is exercised by the selfhost on every build.
- Coverage gap: the internal arity-mismatch and unbound-tyvar
  diagnostic *messages* are not asserted at the string level (only the
  pass/fail count through `check_program`). A negative-golden fixture
  would tighten this, but the existing `examples/` negative goldens
  already exercise those diagnostics end-to-end through the driver.

## Cost vs estimate

Estimated "smallest of the easy three." Held: the analysis was the
bulk of the work (the two-outsider finding), the mechanical move was a
single splice, and the test file leaned on the existing parser-driven
pattern from `test_parse.kai`. No compiler edits required.

## Follow-ups for the fmt / driver lanes

- **fmt and driver can now run in parallel** — resolve was the only
  one of the three that sat at the top of `main.kai` and renumbered
  everything. After this merges, fmt and driver touch disjoint regions.
- **Dead code:** `lookup_effect_op_names` / `effect_op_names` are
  unused. If a dead-code sweep lane materialises, they are candidates.
- **Cross-section discipline confirmed again:** the literal line range
  in any modularisation brief is a *starting hypothesis*, not the cut.
  The callee graph is what decides the boundary. The fmt and driver
  lanes should expect the same — co-located helpers that belong to a
  different layer.
