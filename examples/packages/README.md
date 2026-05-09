# examples/packages

End-to-end fixtures for the kaikai package manager (issue #405).
Each subdirectory contains a `kai.toml` and a runnable program.

## Fixtures

- **`local_path/`** — `app/kai.toml` declares
  `greet = { path = "../lib_greet" }`. Running
  `kai run local_path/app/main.kai` walks up to the manifest,
  resolves the local-path dep, and injects `--path` for `lib_greet`
  so kaic2 can find `import greet`. Expected output:
  `Hello, kaikai!`.

  ```sh
  bin/kai run examples/packages/local_path/app/main.kai
  # => Hello, kaikai!
  ```

## Coverage in tier 1

Driver-level fixtures don't fit `test-llvm-coverage` (which compiles
fixtures through kaic2 directly without a manifest). Each fixture's
`.out.expected` golden is the contract; the lane that wires
fixtures into CI runs `bin/kai run <fixture>` and diffs against the
golden.

## Out of scope (this lane)

- Git-based deps (`{ source = "...", ref = "..." }`) — recognised by
  the parser but `kai install` reports them as deferred. The git
  resolver lands in a follow-up commit.
- Lockfile (`kai.lock`) and cache layout. Local-path deps don't
  need either, so v1 ships without them.
- Transitive resolution and minimum-version selection. v1 reads
  only the immediate manifest.
