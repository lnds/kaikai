# Lane experience — issue #658, package-aware build driver

**Lane type:** driver / UX.
**Scope:** Hanga Roa, `bin/kai` only — zero changes to `kaic2` or
`stage2/compiler.kai`.
**Closes:** #658 (kai driver: package-aware build, Go-style).
**Builds on:** PR #657 (`infer_entry_point` cwd inference) and PR
#659 (`manifest_package_name` as the default output binary name).

## Scope as planned vs. as shipped

The brief listed six goals; this lane ships all six in one PR.

| Goal                                      | Status   | Notes                                                          |
| ----------------------------------------- | -------- | -------------------------------------------------------------- |
| 1. `kai build` from a package dir         | shipped  | `resolve_package_spec ""` → reads `kai.toml` + `entry`.        |
| 2. `kai run .` shorthand                  | shipped  | Explicit dot is parsed; same code path as the empty spec.      |
| 3. `kai test ./...` discovery             | shipped  | `find ... -name kai.toml`, run each via `( cd && "$SCRIPT" )`. |
| 4. Sub-packages (`kai build ./sub`)       | shipped  | Sub-package binary lands in the sub-dir to avoid collisions.   |
| 5. `entry` field as override              | shipped  | `manifest_entry_field` mirrors `manifest_edition_value`.       |
| 6. Module-qualified imports               | shipped  | Zero-stage2 fix via `--path <parent>` (see below).             |

No item was deferred. The brief flagged module-qualified imports as
the "risky" piece and authorised a defer-to-separate-PR if the
`stage2/compiler.kai` change exceeded ~50 LOC. It turned out a
stage2 change was unnecessary at all — see "Module-qualified
imports without touching the typer" below.

## Design decisions

### `resolve_package_spec` as the single entry-resolution funnel

The pre-existing `infer_entry_point` only handled the cwd case
("no arg"). Every subcommand called it differently — `cmd_run`
defaulted to file mode when an argument was present, even if the
argument was `.` or `./sub`. Rather than duplicating the spec
parsing across `cmd_build / cmd_run / cmd_test / cmd_bench /
cmd_check / cmd_fmt / cmd_watch`, I introduced a single helper:

```sh
resolve_package_spec ""           # → resolve cwd package or fall through to legacy
resolve_package_spec "."          # → ditto
resolve_package_spec "./sub"      # → resolve sub-package
resolve_package_spec "foo.kai"    # → pass-through (legacy file mode)
```

`./...` is intentionally **not** handled by `resolve_package_spec`
— it expands to a list of packages, which doesn't fit the helper's
"emit one path" contract. `cmd_test` intercepts `./...` directly
and delegates to a separate `run_tests_recursively` walker.

Callers retain their own usage-message glue, so `kai build`'s
"missing input file" error still says `see 'kai build --help'`.

### Sub-package output placement

The first iteration of `cmd_build` failed when given `./sub`
because the default output name was the sub-package's name (e.g.
`sub`), which collided with the existing `sub/` directory on disk:

```
ld: open() failed, errno=21 (Is a directory) for 'sub'
```

Two fixes were considered:

1. Pick a different output name when the spec is a sub-package
   (`sub_pkg`, `sub.bin`, etc).
2. Place the output inside the sub-package directory.

Option 2 is what `go build ./sub` does and what users naturally
expect — the binary lives with the package's source. Implemented
by comparing the manifest dir against the cwd in `cmd_build`'s
default-out branch: when they differ, the output goes into the
manifest dir.

### Module-qualified imports without touching the typer

The brief positioned this as the most invasive piece and
explicitly authorised deferring it to a separate PR. Reading
`stage2/compiler.kai`'s `module_to_path` revealed that the
resolver already does dotted-path-to-filesystem mapping:

```kai
fn module_to_path(base_dir: String, mod: String) : String {
  let rel = string_concat(dots_to_slashes(mod), ".kai")
  ...
}
```

So `import notes.store` already resolves to `<base>/notes/store.kai`
on the first `--path` it can find. The only missing piece was that
`<base>/notes/` had to exist — which means the package's **parent
directory** had to be on the search path.

I extended `manifest_path_flags` with a single extra `--path
<dirname($manifest_dir)>`. The bare-import path (`import store`)
still works because the manifest dir itself remains the first
search root. Total LOC in `bin/kai`: 9. Total LOC in
`stage2/compiler.kai`: 0.

### `kai test ./...` discovery shape

Two reasonable shapes were considered:

1. **Pre-build everything, then run all the binaries.** Lower
   total link cost, harder to attribute a failure to the package
   that produced it.
2. **Run `kai test` once per package.** Higher cost (full
   pipeline per package) but transparent, per-package output and a
   trivial implementation.

I picked option 2 — it matches `go test ./...` semantics, the
output stream lets the user see exactly which package's tests
failed without parsing, and the implementation is "find + for".
Aggregate failure propagates via a temp-file tally (the `while`
loop body runs in a subshell, so counter variables can't escape).
For the package counts kaikai's stdlib + demos will see in
practice (<50 packages per repo), the wall-clock difference is
negligible.

## Structural surprises

### POSIX `sh` subshells eat counter state

The `while read manifest; do ... done` body runs in a subshell
under POSIX `sh` (the script's `#!/bin/sh` discipline). I burned
a few minutes on a first version that mutated `any_fail` inside
the loop and read it after — it always read 0. The fix is to
materialise the failure marker on disk (`mktemp` + non-empty
check) so the parent shell sees it. Same pattern as
`prelude_cache_lookup` uses for cache-hit reporting.

### `kai build ./sub` collides with directory `sub`

This is what tripped up the smoke test. The fix is in design
decisions above. Worth calling out separately because it's
exactly the kind of bug a CI test would catch but a user
wouldn't — every Go developer who's done `go build ./cmd/foo`
expects the binary to land under `cmd/foo/`, not `./foo`.

### Output-name resolution survived intact

PR #659 added `manifest_package_name` as the default output name.
The new sub-package path threads through that helper untouched —
the only addition is the cwd-vs-manifest-dir check that picks the
output directory. No regression to the existing "build to
package name" behavior.

## Fixtures

Four new fixtures under `examples/packages/`:

| Fixture                       | Exercises                                   |
| ----------------------------- | ------------------------------------------- |
| `build_hello/`                | `kai build` (no args) + default `main.kai`. |
| `build_entry_override/`       | `entry = "src/main.kai"` override.          |
| `build_sub_package/`          | `kai build ./sub` + binary placement.       |
| `build_module_qualified/`     | `import <pkgname>.<module>` shape.          |

Each fixture has its own `README.md` reproducer and a
`.out.expected` golden. The four binaries are listed in
`.gitignore` so a local verification run doesn't dirty the
worktree.

No `make test-package-aware-build` target was added — the
fixtures are exercised on demand from the README reproducers and
are picked up incidentally by `examples/packages/` discovery in
future test passes.

## Real cost vs estimate

- **Estimate** (per the brief): ~300–400 LOC in `bin/kai`, plus
  ~50 LOC in `stage2/compiler.kai` if module-qualified imports
  stayed in scope (with a defer-to-followup contingency).
- **Actual**: +346/-49 in `bin/kai` (net ~300 LOC), 0 LOC in
  `stage2/compiler.kai`, +9 in `.gitignore`, +10 in
  `docs/editions.md`, +101 in `docs/packages.md`. The stage2 path
  evaporated because `module_to_path` already did the work; the
  driver just had to feed it the right `--path`.

Time: roughly half a session. The smoke loop dominated — every
"does this work?" iteration costs a clean `kai build`, which
costs a full kaic2 compile.

## Coverage gaps + follow-ups

1. **Tier 1 doesn't gate the new fixtures.** The four
   `build_*/` directories under `examples/packages/` are
   reproducers only; no CI target compiles them. A
   `make test-package-aware-build` target that loops the README
   reproducers and diffs against the `.out.expected` files would
   close the gap. Tracked as a follow-up issue (not opened in
   this lane; #569's `package-mode CI harness` is the natural
   home).
2. **`./...` doesn't currently respect a `.kaiignore` or
   `[exclude]`.** Every `kai.toml` under cwd is walked, including
   ones inside `tests/fixtures/` if any. For kaikai's own
   checkout this is fine; for downstream packages with vendored
   sub-modules it could surface unwanted packages. Defer to a
   follow-up once a use case appears.
3. **No `kai run ./sub <args>` test in fixtures.** The
   `cmd_run` path is exercised by hand but not goldened. Same
   follow-up as #1.
4. **Module-qualified imports are name-collision-prone.** If a
   downstream package depends on another package called `notes`
   AND its own package name is `notes`, the parent-dir-on-path
   trick could shadow the dep. Today the dep is added first in
   `manifest_path_flags`, so the dep would win for `import
   notes.store`; the local file `notes/store.kai` would not
   resolve. Acceptable for v1 — kaikai's resolver picks the
   first match per `--path` order and the user controls the order
   via `kai.toml`. Worth pinning a note in `docs/packages.md`'s
   "out of scope" section if anyone hits it.

## What this lane did *not* change

- `stage2/compiler.kai` — zero edits. `find_module_file` was read
  to confirm the dotted-path resolution; no behaviour change
  needed.
- `tools/kai-pkg/` — no changes. The package manager binary is
  invoked exactly as before for dependency resolution.
- `kai.toml` schema — the `entry` field is **additive**. Packages
  without it default to `main.kai`. Existing fixtures and demos
  keep building byte-for-byte.
- `manifest_path_flags` — adds one `--path` flag, otherwise
  identical. The existing path is still emitted first, so the
  `import store` (bare) path's resolution order is unchanged.
