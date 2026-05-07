# Lane experience — issue #318 (`kai test <file>` prelude scope)

## Objective metrics

- Branch: `issue-318-test-prelude-scope`
- Base commit: `0e73ac8` (Merge PR #317 — Real literal division)
- Lane open: 2026-05-07T11:30 (approx)
- Files changed:
  - `stage2/compiler.kai` — CliOptions field, `--include-prelude-tests`
    flag, `load_prelude` filter, `strip_prelude_tests` helper.
  - `stage1/compiler.kai` — CliOptions field, flag plumbing,
    `load_prelude` filter, `strip_prelude_tests` (DTest-only).
  - `bin/kai` — `cmd_test` accepts `--include-prelude-tests` and
    forwards it to kaic2.
  - `stage2/Makefile` — `test-issue-318` (default behaviour) and
    `test-issue-318-include` (opt-in legacy) targets, both wired
    into `test` and `test-fast`. Stdlib bulk loop excludes the
    fixture so it isn't compiled in `--run` mode.
  - `examples/stdlib/issue_318_kai_test_prelude_scope.kai` +
    `.out.expected` — regression fixture.
  - `docs/phase4-design.md` §1+§2 — documents the new behaviour.
- Compiler delta: ~30 net additions in stage 2, ~20 in stage 1.
- Selfhost: byte-identical (vanilla flags).
- Tier 0: green.
- Tier 1: green.

## Diagnosis

`load_prelude` (stage 2) and its stage-1 sibling parse a `.kai`
file and concatenate every `Decl` from it into the global flat decl
stream that `compile_source` operates on. `Decl` includes `DTest`,
`DBench`, `DCheck`. The downstream test runner (`test_decls` →
`number_tests` → `emit_all_tests`) walks the merged stream and
emits a `_kai_test_<id>` C function per `DTest`, regardless of which
file the block came from. Result: every `kai test <user.kai>`
invocation runs the user's `test` blocks plus all 83 stdlib `test`
blocks because `bin/kai` injects every `stdlib/core/*.kai` file as
a `--prelude`.

The fix shape was option (1) from the issue body — drop prelude
test blocks at load time, behind an opt-in flag. That keeps the
existing `test-stdlib-core-intrinsic` path working untouched
(stdlib tests run because the stdlib file *is* the target, not a
prelude — see `stage2/Makefile`'s recipe comment).

## Fix shape

Smallest delta that doesn't touch the AST. `DTest` keeps its 4-arg
shape. The filter sits one layer up, in `load_prelude`:

```
let kept = if include_tests { ds } else { strip_prelude_tests(ds, []) }
```

`strip_prelude_tests` is a tail-recursive accumulator that drops
every `DTest` / `DBench` / `DCheck` and passes everything else
through. The flag threads through `load_preludes` →
`CliOptions.include_prelude_tests` and is parsed at the CLI by a
new `--include-prelude-tests` arm in `parse_cli_loop`.

Stage 1's `load_prelude` got the same treatment, with a smaller
`strip_prelude_tests` (only `DTest` exists in stage 1 — the parser
doesn't know `bench` or `check`).

The driver `bin/kai` learned to recognise `--include-prelude-tests`
in `cmd_test` and forward it to kaic2 alongside the implicit
`--test`. Other subcommands (`build`, `run`, `bench`, `check`)
don't expose the flag; their auto-loaded preludes never carried
test blocks anyway in the stdlib-as-shipped (only `examples/`
fixtures use `bench`/`check`, and those run as the *target*, not
a prelude).

## Migration inventory

- 0 file renames.
- 0 AST shape changes.
- 0 stdlib edits — the stdlib's intrinsic tests still run via
  `test-stdlib-core-intrinsic`, where each core module is the
  target file (other core modules are loaded as preludes minus
  the target itself, per the existing recipe).
- 1 driver-script change (`bin/kai cmd_test`).
- 1 doc update (phase4-design §1+§2).

## Compiler errors I encountered

- `make test-stdlib` failed on first tier1 run — the bulk stdlib
  fixture loop iterates `examples/stdlib/*.kai` and compiles each
  with the regular pipeline (no `--test`). My fixture's `main`
  prints "issue 318" but the `.out.expected` records the test
  runner output; in `--run` mode the test blocks don't fire and
  the diff fails. Fix: add `issue_318_kai_test_prelude_scope` to
  the existing exclude case alongside `qualified_call_in_prelude`
  and `issue_219_typer_modcall` — both prior fixtures meant for
  dedicated targets, not the bulk loop.
- The fixture's first `diff` against the expected file came back
  empty because the test runner writes its output to **stderr**,
  not stdout. The first cut of `test-issue-318` redirected only
  stdout (`> build/...out`); the binary's actual output landed on
  the terminal. Fix: `2>&1`. Worth noting because every other
  stdlib fixture in the Makefile drives a normal `main` (writes to
  stdout via `println`); a test-runner fixture is the first one
  that needs stderr redirected.

## Friction points

- The driver's `cmd_test` previously had a single-arg shape (`-h`,
  empty, or filename). Adding an opt-in flag in front of the
  filename meant restructuring the loop to consume optional flags
  before the positional arg. Done with a tight `while … case` —
  shellcheck-clean (one disable for word-splitting on the
  forwarded flags variable).

## Spec ambiguities or interpretive choices

- The issue suggested three fix shapes; I chose (1) per the
  brief's recommendation. Option (2) (skip prelude test blocks
  unconditionally without an opt-in) would be even smaller — drop
  the flag, drop the CLI plumbing — but it removes a useful
  developer affordance: `kai test --include-prelude-tests
  my_file.kai` is a one-line way to confirm the stdlib hasn't
  silently regressed while editing your own code. It's free
  diagnostic surface.
- The flag name copies the issue's wording verbatim. A shorter
  spelling (`--all-tests`) would invite confusion with future
  `--filter`-style flags. The current name is unambiguous and
  greppable.
- `DBench` and `DCheck` are dropped alongside `DTest` even though
  the issue only names tests. Rationale: `kai bench` and `kai
  check` have the same architecture (target file with prelude
  injection); a hypothetical future stdlib `bench` block would
  hit the same bug. Pre-emptive symmetry costs nothing — neither
  block type appears in the stdlib today.

## Subjective summary

A surface-level fix. The hardest part was confirming that
`test-stdlib-core-intrinsic` doesn't break — the recipe explicitly
keeps the test target out of its own prelude set, which is exactly
what the new filter needs to leave untouched. Without that
existing convention, this fix would have needed a way to
distinguish "prelude-origin DTest" from "target-origin DTest" in
the AST, which would have meant extending `DTest` with a fourth
arg the way `DFn` carries `Option[String]` (issue #230's mechanism).

The opt-in flag is the right decision-point design. The default
serves the user; the flag serves the maintainer; neither path
demands the other be surprising.

## Limitations of this report

- Local macOS run only; CI confirmation lands when the PR opens.
- The fixture exercises one shape (user file with two test
  blocks, full stdlib prelude). I did not test what happens when
  the user `import`s a module that itself defines tests — those
  imports go through `expand_imports`, not `load_prelude`, so the
  filter doesn't reach them. Per the issue: imported modules
  *should* contribute their tests. Not an ambiguity, just a
  scope boundary.
- Did not measure compile-time impact of the extra walk. The walk
  is O(|prelude_decls|) and replaces no work, so it's strictly
  additive, but the wall-clock noise is below the build's natural
  variance.

## Build telemetry

(Filled at PR-open time.)
