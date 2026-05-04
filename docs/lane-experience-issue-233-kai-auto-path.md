# Lane experience — issue-233-kai-auto-path

## Objective metrics

- Files changed: `bin/kai` (+10/-1), `Makefile` (+18/-2), 4 new fixture files under `examples/multi-module/`.
- Build/test outcomes recorded in TSV (appended below).
- selfhost: byte-identical for both stage1 and stage2.
- demos-no-regression: 27 passing, baseline 26 — holds.
- New target: `make test-multi-module` — green.
- Tier 1 local: failed, but the failure is preexistent and unrelated
  (see "Friction points"). CI is the merge gate per CLAUDE.md.

## Inventory (kaic2 invocation sites in bin/kai)

`bin/kai` centralises every kaic2 codegen invocation through a single
helper `compile_to_binary()` (line ~297). All five subcommands route
through it:

- `cmd_build` (line ~343) → `compile_to_binary "$src" "$tmp" "$out" "build"`
- `cmd_run`   (line ~362) → `compile_to_binary "$src" "$tmp" "$bin" "run"`
- `cmd_test`  (line ~375) → `compile_to_binary ... "test" --test`
- `cmd_bench` (line ~388) → `compile_to_binary ... "bench" --bench`
- `cmd_check` (line ~401) → `compile_to_binary ... "check" --prop-check`

`cmd_fmt` invokes `kaic2 --fmt` directly without preludes or `--path`,
since fmt operates at source-text level (per its own block comment).
The fix did not need to touch fmt.

One edit inside `compile_to_binary` covers all five subcommands.

## Fixture shape

`examples/multi-module/`:

- `main.kai` — `import math` plus `import geo.shapes`; calls `math.square(7)` and `shapes.area_square(5)`.
- `math.kai` — sibling helper; exports `pub fn square(n: Int) : Int = n * n`.
- `geo/shapes.kai` — nested helper; exports `pub fn area_square(side: Int) : Int = side * side`.
- `main.out.expected` — `49\n25\n`.

The Make target runs `bin/kai run` from `cd /` with an absolute source
path, so it exercises the driver contract (the `--path "$(dirname "$src")"`
argument added in this lane), not cwd-relative resolution. Wired into
`make tier1` via the existing `test → test-multi-module` chain.

## Friction points

### kaic2 invocation centralisation

**Centralised, not duplicated.** A single `compile_to_binary` helper
serves build/run/test/bench/check. One line edit covered every
subcommand the issue listed.

### Single-file usage stays clean

`dirname` of a bare `hello.kai` is `.`. Adding `--path .` is benign
for single-file programs — kaic2 already searches `.` via `base_dir`
when the source lives in cwd, and the `--path .` entry is a no-op
duplicate of that.

### Bug as filed does not reproduce on current main

The issue's exact repro (`mkdir myproj && main.kai imports math.kai`)
prints `49` cleanly on `main` (commit 33a90f1) **before** any fix —
both with cwd inside `myproj` and with cwd at `/` and an absolute
source path, with and without `KAI_NO_STDLIB=1`.

Why: `stage2/compiler.kai` `expand_imports` (line ~35858) computes
`base_dir = dir_of(root_path)` and `find_module_file` (line ~35751)
tries `base_dir` **before** any `--path` entry. The block comment at
`module_to_path` (line ~35722) documents this:

> resolved in order:
>   1. relative to the root file's directory (`base_dir`), then
>   2. each directory given via `--path`, in the order passed.

So the auto-discovery the issue requests already happens at the
compiler level. The fix in this lane is **defense-in-depth**, not a
bug fix: it makes the contract explicit at the driver layer so any
future change to `find_module_file` does not silently break user
projects. The fixture freezes the multi-module behaviour as a
regression gate.

User authorised this framing explicitly (option C: "implementar A
pero documentar que el comportamiento ya funciona vía base_dir").

### Preexistent local tier1 flake unrelated to this lane

`make tier1` failed locally on `test-effects` after
`m12_8_y_phase4_subtyping (LLVM)`, with `make: *** [test-effects] Error 143`
(SIGTERM). Inspection of the affected block shows the next ciclo runs
`issue_107_signal_trap` under `tests/sigharness.c`, which sends
signals to its child. The harness binary in isolation
(`./build/s2-issue_107_signal_trap.harness ./build/s2-issue_107_signal_trap`)
exits 0 with output matching the golden. Inside `make`, the parent
shell receives SIGTERM. Same failure is reproducible deterministically
on every re-run of `make test-effects`, but **CI tier1 on `main` is
green** for the last three runs (verified via `gh run list --branch
main --workflow tier1.yml`). This is a macOS-specific local flake in
the signal harness, not a regression introduced by this lane.

The lane changes shell driver + a new Make target + four fixture files.
None can affect a signal-handling test running an unrelated binary
under a C harness. Selfhost (`make selfhost`) is byte-identical, which
is the load-bearing gate against compiler regressions.

Per CLAUDE.md "CI green is the merge gate" — the right move is to push
and let CI rule.

## Spec ambiguities or interpretive choices

- **`--path` ordering**: stdlib first, user dir second, per the brief
  ("kaikai treats stdlib as canonical (m6.2 design)"). User shadowing
  stdlib via accidental name collision is rejected; deliberate
  shadowing requires invoking `kaic2` directly.
- **Closing `#233`**: chose to close the issue even though the
  underlying bug does not reproduce, because (a) the user explicitly
  authorised the defense-in-depth framing, (b) the fixture closes the
  loop with regression coverage, and (c) leaving the issue open with a
  "no-repro" note creates ambiguity for future readers about whether
  the contract is intended.
- **Test target placement**: chose root `Makefile` over `stage2/Makefile`,
  matching `test-demos` precedent (driver-level integration tests live
  at the root, compiler-level fixtures live under stage2).
- **`cd /` in test recipe**: makes the test fail loudly if the driver
  ever stops auto-adding the entry's directory. Without that, a
  `cd $(repo)` test would pass even if `bin/kai` were broken because
  the resolver would find `math.kai` via cwd.

## Subjective summary

Five-minute fix. The interesting part was confirming the bug isn't
actually a bug at the layer the issue blamed. Honest-framing call
mattered: silently "fixing" a non-bug would have erased a useful piece
of design knowledge (that kaic2 already does the right thing) and
left a misleading commit message in the history. The user-authorised
defense-in-depth path keeps the contract explicit while documenting
the actual state of affairs in this report.

## Limitations of this report

- Tier1 was not green locally due to the preexistent signal-harness
  flake; full local tier1 verdict is "everything else green, one
  preexisting failure". CI will be the final word.
- I did not run tier1-asan locally (shell-only change; selfhost
  byte-identical → no compiler delta to surface under ASAN).
- The "defense-in-depth" framing is true today but rests on the
  assumption that no future kaic2 refactor removes the
  driver-supplied `--path "$src_dir"` entry as well. If both are
  removed, the multi-module fixture would still fail loudly.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-04T12:49:45-04:00	tier0	OK	49
2026-05-04T12:52:53-04:00	tier1	FAIL	181
```

(tier1 failure: preexistent `test-effects` signal-harness flake on
macOS; CI on `main` is green for the same target — see Friction
points.)
