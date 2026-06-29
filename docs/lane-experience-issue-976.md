# Lane experience — issue #976: `kai doc` empty for a package module importing stdlib

## Scope

As planned: `kai doc <pkg-module>` returned nothing (exit 1) when the
target **package** module imported a **stdlib** module (`import spawn`,
`actor`, `time`, `string`, …). The same module type-checked and built
fine, and stdlib modules importing stdlib (`actor` imports `spawn`/`time`)
documented correctly — the gap was specific to package modules.

As shipped: identical to the plan. One-line root cause, one-line fix in
the driver wrapper, plus a regression fixture wired into the existing
`kai doc` harness.

## Root cause

`kai doc` extracts docs via `kaic2 --doc-json <file>`. In `cmd_doc`
(`bin/kai`), the package-module branch built `doc_path_flags` from
`manifest_path_flags` (the package root, its parent, and deps) but never
added `--path $STDLIB_ROOT`. `compile_to_binary` (the build/check path)
passes `--path "$STDLIB_ROOT" --path "$src_dir"` explicitly; the doc path
dropped the stdlib root.

Why stdlib-imports-stdlib was unaffected: for a stdlib module, the doc
file lives under `$STDLIB_ROOT`, and kaic2 probes `dir_of(root_path)`
before the `--path` entries — so `import spawn` from `actor.kai` resolves
against stdlib's own directory. For a package module, `dir_of(root)` is
the package dir, not stdlib, so a stdlib import had no search root.

Confirmed directly: `kaic2 --path <pkg> --doc-json withimport.kai` →
`cannot open module 'spawn'`, exit 1; adding `--path $STDLIB_ROOT` →
correct JSON, exit 0. The driver's `MDocJson` handler skips `check_program`
(docs are syntactic), but import expansion runs first and bumps the error
counter on the unresolved import, aborting before the JSON is printed.

## Fix

Lead the `--doc-json` invocation with `--path "$STDLIB_ROOT"`, mirroring
`compile_to_binary`. Disjoint from `driver.kai` — purely in the `bin/kai`
wrapper — so it does not touch the typer/driver and does not collide with
#962 Lane 4 (batch mode), which edits `driver.kai` in parallel. No
selfhost byte-id concern (compiler bytes unchanged).

## Structural surprise

None. The package-module doc resolver (`doc_resolve_pkg_module`,
`doc_path_flags`) was added recently and already carried package `--path`
roots; it just never inherited the stdlib root that every build/check path
has. The fix is the same `--path $STDLIB_ROOT` lead the rest of the driver
already uses.

## Fixtures

`examples/packages/doc_stdlib_import/`:
- `pkg/clean.kai` — no imports (documents fine even without the stdlib path).
- `pkg/withimport.kai` — module doc + `import spawn` + `pub fn go`.

`tools/test-doc.sh` case #7 (tier1 `test-doc`): from the fixture dir,
`kai doc pkg/clean` exits 0, and `kai doc pkg/withimport` exits 0 with the
module header, module doc, and the `go` item present. `test-packages.sh`
uses an explicit fixture list, so the new directory is ignored there (no
`main.kai`/golden) rather than mis-run.

## Verification

- Issue reproducer reproduced exactly (empty, exit 1) on pre-fix `kai`,
  fixed (item + synopsis, exit 0) after.
- No regression: `kai doc pkg/clean`, `kai doc actor` (stdlib→stdlib),
  `kai doc pkg/withsibling` (package sibling import), `kai doc string`,
  and the bare `kai doc` list all exit 0.
- `tools/test-doc.sh` green (all 7 cases). `make tier0` green (selfhost
  deterministic, demos baseline, gates).
- Build was C-only (no libLLVM in PATH); `kai doc` uses `--doc-json`,
  which is backend-independent, so the native default vs C distinction
  does not apply to this path.

## Follow-ups

None for this lane. The broader `#[doc]` coverage audit across existing
stdlib stays tracked in #774 (unrelated to this resolution gap).
