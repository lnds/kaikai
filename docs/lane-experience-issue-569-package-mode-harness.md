# Lane retro — issue #569 package-mode CI harness

Date: 2026-05-20. HEAD: v0.79.0. Edition: hanga-roa.

## Scope as planned vs scope as shipped

The issue body lists a 9-category coverage matrix. Six categories
needed brand-new fixtures (#2, #3, #5, #6, #7, #8, #9), one was
already there (#1 self_import), one was implicit (#4 local_path)
and got wired into the runner explicitly. All nine plus the four
legacy driver-level scripts ship as a single 14-line summary
under `tools/test-packages.sh`. No compiler changes.

| # | Category | Outcome |
|---|---|---|
| 1 | Self-import (#567 guard) | wired existing `self_import/` into the runner |
| 2 | Transitive privacy (#565 guard) | new `transitive_privacy/` |
| 3 | Pub leak (negative of #2) | new `pub_leak/` with `.err.expected` substring |
| 4 | Local-path dep | wired existing `local_path/` |
| 5 | Sibling examples + tests | new `sibling_examples_tests/` |
| 6 | Cross-package effects | new `cross_package_effects/` (`pub effect`, `handle`, `var`) |
| 7 | Same-name shadowing | new `same_name_shadowing/` |
| 8 | Stdlib visible across deps | new `stdlib_across_deps/` |
| 9 | Auto-install on first compile (#512 guard) | new `auto_install/` with `check.sh` |

The harness shape settled on three fixture types:

- **Positive**: `kai.toml` + `main.kai` + `main.out.expected`. Runner
  does `bin/kai run .` from the dir, strips `kai:` driver lines from
  stdout, diffs against the golden.
- **Negative**: `<sub>/main.err.expected` (single-line substring).
  Runner does `bin/kai build .`, expects non-zero exit (treats as
  "ok" via `|| true`), `grep -F` substring in combined stderr.
- **Driver-level**: `check.sh`. Runner just invokes it; the script
  owns its own assertions.

## Design decisions and alternatives considered

### Fixture layout convention — package name = top-level dir

Three of the new multi-package fixtures (#2, #3, #6, #8) use the
pattern:

```
fixture/
├── leaf/{kai.toml,leaf.kai}
├── mid/{kai.toml,mid.kai}
└── consumer/{kai.toml,main.kai,main.out.expected}
```

The first attempt at #5 (`sibling_examples_tests`) used a more
Cargo-ish layout — `src/main.kai`, modules under `src/`. That
broke: kaikai resolves `import mathlib.adder` to
`<search-path>/mathlib/adder.kai`, not `<src>/adder.kai`. The
package name must be a literal directory name visible somewhere on
the search path. Switched to flat-with-modules layout (`main.kai`
at the package root + a `mathlib/` subdir holding the module
files). Matched the shape `self_import/` already uses.

Documented surface convention worth pinning later: **the package
name is also a directory name** for module path resolution. Today
this is implicit in `manifest_path_flags` (`bin/kai:343-346` —
exposes the manifest's parent dir as a `--path`); it should be
called out in `docs/packages.md`.

### Transitive local-path deps not resolved by kai-pkg paths

`consumer → mid → leaf` with local-path deps: `kai-pkg paths` only
emits direct deps of the consumer's manifest. `leaf`'s `--path` is
never added unless the consumer also lists `leaf` in its own
`[dependencies]`.

Two ways out: fix `kai-pkg` to walk transitively, or document the
limitation and have the fixture list both `mid` and `leaf` as
direct deps. Chose the latter for this lane — the fixture's job
is to prove privacy + effect attribution work, not to fix the
resolver. Each fixture's `kai.toml` has a comment explaining the
workaround. The transitive resolution work is a follow-up.

### `kai run <abs-path-to-dir>` does not work; `kai run .` does

While building `auto_install/check.sh`, the first version invoked
`bin/kai run "$DIR"`, which surfaced `file '<dir>' not found`.
`bin/kai run` expects either a `.kai` file or `.`. The fix is
`(cd "$DIR" && bin/kai run .)`. Not a bug per se — `kai run`'s
spec says "<file>.kai or '.' or './<sub>'" — but the surface is
brittle. Adding "absolute path to a package dir" to that list
would be ergonomic; out of scope for this lane.

### Auto-install detection grep matches `source =`

`needs_auto_install` (`bin/kai:498`) returns true only when the
manifest mentions `source =` (git deps) and either no `kai.lock`
exists or a pinned cache dir is gone. Local-path-only manifests
deliberately skip auto-install — they need no install at all.
The `auto_install/` fixture therefore uses the git-source path
(`greet = { source = "GREET_BARE", ref = "v0.1.0" }`) with the
bare repo from `tests/fixtures/git-fixtures/`. The check removes
`kai.lock`, isolates `KAIKAI_CACHE` to a tmpdir, and asserts
`kai run` recreates the lock from scratch.

## Structural surprises

### kai-pkg paths v1 limitations

The harness surfaced two limitations that the issue body did not
anticipate:

1. **No transitive local-path resolution.** `kai-pkg paths` emits
   only direct deps. Three of the new fixtures duplicate the
   transitive dep in the consumer's manifest as a workaround.
2. **`kai run <abs-path>` does not work.** Only relative or `.`
   accepted. The harness `cd`s before invoking.

Neither blocks the harness — both have one-line workarounds — but
they are real ergonomic gaps that downstream packages (ahu) will
hit eventually. Worth follow-up issues.

### Output filtering — drop `kai:` lines before diffing

`bin/kai run` prints `kai: building package '<name>'` before the
program output. The runner strips that with `grep -v '^kai:'` so
goldens stay clean. Documented as part of the convention in
`tools/test-packages.sh`.

## Fixtures added and coverage gaps

- 6 new fixture directories under `examples/packages/`.
- 1 new helper script: `tools/test-packages.sh` (~140 lines, three
  helper functions: `run_positive`, `run_negative`,
  `run_check_script`).
- `examples/packages/render-fixtures.sh` extended to render the
  `auto_install/` template.
- `Makefile` gets a new `test-packages` target wired into `tier1`.
- `stage2/Makefile` gets a parallel `test-packages` target wired
  into `test-fast` for local iteration.

Coverage gaps not closed in this lane:

- **Transitive local-path resolution.** Workaround in three
  fixtures. Real fix is in `tools/kai-pkg`.
- **Cross-platform.** Harness uses `mktemp`, `find`, `grep`, `awk`
  in POSIX-compatible form; tested on darwin-arm64 only. CI Linux
  runners should pick it up cleanly via the `tier1` target.
- **LSP against multi-package layouts.** Explicitly out of scope
  per the issue.

## Real cost vs estimate

Issue estimated 1-2 days. Real cost: ~3 hours (single agent
session). The estimate was conservative because the fixture shape
was unfamiliar; once the first two fixtures landed, the rest were
mechanical.

## Follow-ups for next lanes

1. `kai-pkg paths` should walk transitive local-path deps.
2. `kai run <abs-path>` should accept a directory.
3. Document the "package name is a directory name" convention in
   `docs/packages.md`.
4. Consider extending the harness with cross-platform asserts once
   the second CI runner (Linux) is wired (eventual #569 follow-up).

## What this lane closes

- 9-category matrix from #569 fully covered.
- `tools/test-packages.sh` runner exists.
- `make test-packages` wired into `tier1`.
- 14 fixtures green on darwin-arm64.

CI is no longer blind to package-mode regressions. The next
`#565` / `#567` shape would be caught by this harness before
landing on `main`.
