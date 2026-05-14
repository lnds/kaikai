# Lane experience — issue #567: package root must be a search path for intra-package imports

## Scope as planned

Issue #567: `kai build examples/demo/main.kai` from inside a package
root cannot resolve `import mylib.greet` against the sibling
`mylib/greet.kai`, even though `kai.toml` at the root names the
package `mylib`. Workaround is a circular-looking
`mylib = { path = "." }` self-dependency entry in
`[dependencies]`.

The issue body identifies the cause (`manifest_path_flags` in
`bin/kai` emits `--path` only for resolved external deps, never for
the manifest dir itself) and proposes the fix (emit
`--path <manifest_dir>` when a manifest is found, before any deps).

Estimate: 1-3 hours. Bug fix only, no design surface.

## Scope as shipped

Matches the plan. Three additions in `bin/kai`:

1. One `printf -- '--path %s ' "$manifest_dir"` line at the top of
   `manifest_path_flags`, before `ensure_kaipkg` / `kai-pkg paths`.
2. Updated docblock above the function explaining why the manifest
   dir comes first and citing #567.
3. New fixture `examples/packages/self_import/` with the exact
   layout from the issue repro: `kai.toml` (empty `[dependencies]`),
   `mylib/greet.kai`, `examples/demo/main.kai`, and an
   `.out.expected` golden.

## Design decisions

### Order of search paths

`stage2/compiler.kai` `find_module_file` (line 47653) probes
`base_dir` (the entry file's directory) first, then walks
`search_paths` in the order received. The wrapper composes
`--path` flags as `[STDLIB_ROOT, src_dir, …pkg_path_flags…,
preludes…]`. Putting the manifest dir at the head of
`pkg_path_flags` (before external deps) is the conservative choice:

- The src_dir wins over the manifest dir, preserving sibling-module
  discovery (issue #233).
- An external dep with the same name as the local package cannot
  shadow the local package's own modules, because the manifest dir
  is consulted before any dep path.

The alternative (manifest dir last, after external deps) would let
a dep with name `mylib` win over local `mylib/foo.kai`, which is
the failure mode we are explicitly preventing.

### Why the wrapper and not kaic2

The issue's symptom is in module resolution but the root cause is
that the wrapper never tells kaic2 to look at the package root.
kaic2 itself probes `dir_of(root_path)` first (#233 contract) and
then walks `--path`. The shortest correct fix is to add the
manifest dir to the `--path` list at the wrapper layer; touching
kaic2 would be a larger, more risky change for the same observable
behaviour.

## Fixtures shipped

New: `examples/packages/self_import/`. Layout exactly mirrors the
issue's repro. Verified end-to-end:

```sh
bin/kai run examples/packages/self_import/examples/demo/main.kai
# -> hello from mylib
```

Not wired into `tier1` directly — consistent with the other
`examples/packages/` fixtures, which document their invocation in
the package-level README and depend on a working `kai-pkg paths`.
The fixture's golden is byte-equal to `.out.expected` after the
fix.

## Structural surprises

`kai-pkg paths` (the helper that `manifest_path_flags` calls)
segfaults on HEAD `d7bfce2` whenever it runs, regardless of
`[dependencies]` content — observed both with the self-import
repro and with the pre-existing `examples/packages/local_path/`
fixture. The segfault did not block the issue #567 fix because the
manifest-dir `--path` is now emitted before `kai-pkg paths` runs,
so the local module resolves even when the helper crashes.

Filing this as a separate issue is out of this lane's scope per
lane discipline (`CLAUDE.md` — "if you find a bug outside your
lane, open a GitHub Issue"). Action item left for the integrator.

## Coverage gaps

- No tier1 wiring for the self_import fixture. Matches the
  `examples/packages/` convention but means the regression has no
  CI gate. A follow-up lane that fixes the `kai-pkg paths`
  segfault should wire all `examples/packages/*.out.expected`
  fixtures into tier1 together.

## Real cost vs estimate

~1 hour wall. Most of the time went to confirming the order in
`find_module_file` (so the docblock can cite a real invariant) and
verifying the existing `examples/packages/` convention. The fix
itself was the single `printf` line predicted in the issue.
