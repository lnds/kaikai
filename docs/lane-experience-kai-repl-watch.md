# Lane experience — kai-repl-watch

## Objective metrics

- **Lane span**: 2026-05-04T16:20:43-04:00 → 2026-05-04T16:50:24-04:00 (~30 min wall).
- **Diff**: `bin/kai` only, +227 lines, 0 files removed, 0 compiler / stdlib / runtime touched.
- **PR**: lnds/kaikai#240, opened with `--auto --merge`; CI in progress at lane close (tier1 + tier1-stdlib-core).
- **Tier 0**: green locally. `selfhost byte-identical, demos baseline holds (27 passing, baseline 26)`.
- **Tier 1**: failed locally on `m8_fiber_stack_overflow` (Error 143 / SIGTERM in `make[1]: test-effects`). Reproduced on stock `main` HEAD with my diff stashed — pre-existing flake unrelated to this driver-only change. `mini_ledger` demo also FAILs on main HEAD; same story.

## Commands added

Two subcommands, both shell-only wrappers around the existing `compile_to_binary` helper:

- `kai watch <file.kai>` — `cmd_watch` + `usage_watch` + `watch_run_once` + `file_mtime` helpers. Detects `fswatch` on PATH; uses it when present, otherwise falls back to a 500 ms `stat`-based mtime poll. Build failures keep the loop alive (banner + stderr, then continues).
- `kai repl` — `cmd_repl` + `usage_repl` + `repl_eval` helper. Lines accumulate into `$tmp/buffer.kai`. Empty line or `:run` triggers `repl_eval`, which detects `fn main(` (passthrough) vs no-fn-main (wrap in `fn main() { ... }`) and reuses `compile_to_binary`. Meta-commands `:quit :show :clear :run :help` (and one-letter aliases).

Final `bin/kai` is 723 lines (was 496). Dispatch switch picked up two extra cases.

## Watch-mode dependency story

`fswatch` was not installed on this machine, so the polling fallback is what got exercised end-to-end. The fallback drives the demo just as well — 500 ms latency is invisible to a live audience. The script picks the backend at runtime via `command -v fswatch`, prints `(backend: ...)` so the user knows which one is active, and recommends `brew install fswatch` in the polling banner. No new hard dependency added.

## REPL Level-1 limitations documented

Documented inline in `usage_repl` and reaffirmed in the PR body:

- No state persists across evaluations — every eval recompiles from scratch. Bindings, fn defs, and imports live only until the next eval.
- No line editing / history (the user can wrap with `rlwrap` if needed).
- No paste-mode beyond the empty-line rule.
- Errors print and the buffer is reset; user retypes.

These limitations are deliberate. Persistent-state REPL is Level-2 territory and out of scope for "demo tomorrow".

## Friction points

**fswatch reliability** — couldn't be exercised here (not installed). The polling fallback worked first try: I edited `/tmp/wtest.kai` three times, the watcher detected each save and printed `[hh:mm:ss] Build OK` followed by the program output. The 500 ms cadence was set conservatively — could drop to 250 ms without much cost, but 500 ms is plenty for live demos.

**Empty-line-eval rule** — clean enough. No ambiguity in practice: real input never has trailing blank lines, and `:run` is available as an explicit alternative. The one snag is multi-line constructs (e.g. an `if` block over several lines) — the user must avoid blank lines inside the construct, but that is a documented limitation, not a bug.

**Wrap-fn-main rule** — tested two cases:
1. Plain expressions (e.g. `let x = 7; print(...)`) → wrapped in `fn main() { ... }`. Works.
2. User supplies their own `fn greet(...)` + `fn main()` → grep matched `fn main(`, passthrough. The compiler then surfaced a real type error in the user's code, which proved the passthrough reached kaic2 cleanly.

The rule is grep-based, which means `fn main(` inside a comment or string would also trigger passthrough. Acceptable for a Level-1 prototype — the user retypes if it bites.

**`printf` gotcha** — first version of `:show` used `printf '----- buffer -----\n'`. zsh's `printf` interpreted the leading `--` as a flag and errored. Fixed by switching to `printf '%s\n' '----- buffer -----'`. Caught on the second smoke test, not the first.

**Tier 1 noise** — local tier1 failures masked the question of "did I break something". Reproducing on stock `main` HEAD before commit confirmed they were pre-existing. Worth opening a separate issue for `m8_fiber_stack_overflow` SIGTERM and `mini_ledger` demo, but not in this lane (regla "una worktree, una cosa").

## Subjective summary

Smallest-delta-wins worked well here. The driver's existing `compile_to_binary` helper carried both new commands without modification, so the new code is mostly orchestration around stdin reads, mtime polls, and timestamp printing. Total new logic is ~80 functional lines + ~50 usage strings + boilerplate. Manual smoke tests took maybe 5 minutes total because the failure modes were in shell quoting, not in compiler interaction.

The REPL is genuinely useful for the demo even at Level 1 — typing a couple of lines and seeing output is striking, even without persistent state, because the audience sees the recompile latency live (sub-second on small inputs).

## Limitations of this report

- I did not exercise the `fswatch` backend (not installed locally). CI will not catch this either — there is no shell-driver test that runs `kai watch` automatically. If `fswatch -o` semantics differ from what I assumed, only a manual demo run would surface it.
- I did not write a regression fixture for either command. There is no precedent in the repo for shell-driver fixtures, and the lane briefing did not require one.
- Tier 1 was not green at lane close. CI is the merge gate; if CI also fails on the unrelated fixtures, that is a separate issue.
- The "27 passing, baseline 26" line in tier0 implies one demo is over-baseline — I did not investigate which one or whether the baseline should bump. Out of lane.
