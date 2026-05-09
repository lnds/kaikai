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

## Coverage in tier 1

Driver-level fixtures don't fit `test-llvm-coverage` (which
compiles fixtures through kaic2 directly without a manifest). The
`.out.expected` golden in each fixture dir is the contract; the
lane that wires fixtures into CI runs `bin/kai run <fixture>` and
diffs against the golden.

## See also

- `docs/packages.md` — full v1 spec.
- `tests/fixtures/git-fixtures/setup.sh` — bare repo generator.
