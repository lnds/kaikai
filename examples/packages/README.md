# examples/packages

End-to-end fixtures for the kaikai package manager (issue #405).
Each subdirectory contains a `kai.toml` (or `.template`) and a
runnable program.

## One-time setup (for git-based fixtures)

```sh
tests/fixtures/git-fixtures/setup.sh   # builds bare repos
examples/packages/render-fixtures.sh   # renders kai.toml templates
```

The bare repos live under `tests/fixtures/git-fixtures/*.bare/`
and are gitignored — regenerate them whenever the source under
`tests/fixtures/git-fixtures/{greet,util}/` changes.

## Fixtures

- **`local_path/`** — `app/kai.toml` declares
  `greet = { path = "../lib_greet" }`. Local-path deps need no
  cache and no lockfile. Expected output: `Hello, kaikai!`.

  ```sh
  bin/kai run examples/packages/local_path/app/main.kai
  ```

- **`simple_dep/`** — single git-source dep cloned into the
  cache, SHA pinned in `kai.lock`. Expected:
  `Hello from git, simple_dep!`.

  ```sh
  bin/kai run examples/packages/simple_dep/main.kai
  ```

- **`transitive/`** — `util` depends on `greet`; the resolver
  walks the transitive `kai.toml` of each fetched dep and pins
  both packages. Expected:
  `Hello from git, transitive! (shouted)`.

  ```sh
  bin/kai run examples/packages/transitive/main.kai
  ```

- **`lockfile_reproducibility/`** — `check.sh` runs
  `kai install` twice in two clean caches and asserts
  byte-identical `kai.lock` output (same SHAs).

  ```sh
  examples/packages/lockfile_reproducibility/check.sh
  ```

- **`add_failure/`** — `check.sh` asserts `kai add` is atomic on
  git-clone failure (issue #418): a bad source exits non-zero,
  leaves `kai.toml` unchanged, and never writes a `[[package]]`
  entry into `kai.lock`.

  ```sh
  examples/packages/add_failure/check.sh
  ```

- **`init_invalid_names/`** — `check.sh` asserts `kai init`
  rejects names that fall outside `[a-z][a-z0-9_-]*` (issue
  #419): names with spaces, slashes, `@`, leading dashes or
  digits, path traversal, and uppercase letters all exit
  non-zero without creating `kai.toml`.

  ```sh
  examples/packages/init_invalid_names/check.sh
  ```

- **`manifest_parse_error/`** — `check.sh` asserts every kai-pkg
  subcommand bails on a broken `kai.toml` (issue #420): `show`,
  `install`, `update`, and `add` each exit non-zero and print
  `kai.toml: parse error` instead of silently treating the
  manifest as empty.

  ```sh
  examples/packages/manifest_parse_error/check.sh
  ```

### Issue #569 — coverage matrix (multi-package harness)

The fixtures below were added by the #569 lane to close the
package-mode CI blind spot. They correspond 1-to-1 with the
issue's coverage matrix.

- **`self_import/`** (#1, regression guard for #567) — package
  whose own modules resolve from a sibling `examples/` dir,
  without a self-dep entry. `examples/demo/main.kai` imports
  `mylib.greet`.

- **`transitive_privacy/`** (#2, regression guard for #565) —
  three-package chain `consumer → mid → leaf`. `mid` wraps
  `leaf.public_secret()` and exposes it; `consumer` only sees
  `mid.middle_api`. Privacy stays module-local.

- **`pub_leak/`** (#3, negative companion to #2) — consumer
  tries to call `leaf.private_value()` directly; the build must
  fail with a diagnostic naming the private export.

- **`cross_package_effects/`** (#6) — `leaf` declares
  `pub effect Counter`; `mid` uses it; `consumer` installs a
  `handle ... with Counter` handler.

- **`same_name_shadowing/`** (#7) — package declares `pub fn
  length` colliding on the bare name with `string.length` /
  `list.length`. Qualified call resolves to the package's own
  definition.

- **`stdlib_across_deps/`** (#8) — every layer of a 3-package
  chain calls stdlib (prelude builtins + list operations) without
  re-declaring it.

- **`sibling_examples_tests/`** (#5) — package with `examples/`
  AND `tests/` directories. Both `examples/demo/main.kai` and
  `tests/test_adder.kai` resolve `mathlib.adder` from sibling
  directories without a self-dep entry.

- **`auto_install/`** (#9, regression guard for #512) — manifest
  with a git dep but no `kai.lock`. `check.sh` removes the lock,
  isolates the cache, and asserts `kai run` recreates the lock
  + produces the expected output on the very first call.

Run the whole 14-fixture harness:

```sh
tools/test-packages.sh
# or
make test-packages
```

## Coverage in tier 1

The full harness lives at `tools/test-packages.sh`, target
`make test-packages` (added 2026-05-20, issue #569). Wired into
`tier1` via the root Makefile, so every PR exercises kaikai as a
package consumer end-to-end — manifest discovery, transitive
imports, cross-package effects, stdlib visibility across deps,
auto-install. Before this lane, the only package-shaped CI was
the four legacy `check.sh` scripts (lockfile reproducibility,
add atomicity, init validation, manifest parse) and selfhost,
none of which exercise multi-package layouts.

## See also

- `docs/packages.md` — full v1 spec.
- `tools/test-packages.sh` — the harness runner.
- `tests/fixtures/git-fixtures/setup.sh` — bare repo generator.
- `docs/lane-experience-issue-569-package-mode-harness.md` — lane retro.
