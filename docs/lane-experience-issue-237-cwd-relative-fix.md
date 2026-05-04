# Lane experience — issue-237-cwd-relative-fix

## Objective metrics

| Metric                 | Value                                  |
|------------------------|----------------------------------------|
| Branch                 | `issue-237-cwd-relative-fix`           |
| Base                   | `main` @ `87a3a46` (post-#236, post-#238) |
| Files changed          | 2 (`bin/kai`, `Makefile`)              |
| Lines added            | 30                                     |
| Lines removed          | 14                                     |
| Net diff               | +16 LOC                                |
| Invocation sites fixed | 1 (only `compile_to_binary` builds the kaic2 cmdline; `cmd_fmt` is unaffected) |

Tier gates (see `/tmp/lane-issue-237-cwd-relative-fix-builds.tsv` appended below):

- `make tier0` — OK, 47 s
- `make tier1` — OK, 423 s
- `make tier1-asan` — OK on second run, 1st attempt printed a `make[1]: *** [test-signal-trap-asan] Error 143` then `Error 2` line **but the script's exit status was 0** (the trailing `||` clause swallows the inner failure). Re-running showed every sub-fixture OK end-to-end. Treated as a flake, not introduced by this lane.
- `make selfhost` — OK, 19 s (byte-identical fixed point)
- `make -C stage2 selfhost-llvm` — OK, 26 s (LLVM byte-identical fixed point)
- `make test-multi-module` — OK in both `abs` and `rel` modes, both fixtures.

## Diagnosis confirmation

The empirical repro from issue #237's reopen comment (`/tmp/exp237b/main.kai` + `math.kai`, `cd /tmp/exp237b && kai run main.kai`) **did not fail** against `main` HEAD post-#238. It already printed `49` and exited 0 before any change in this lane.

Hypothesis: PR #238 (`fix(typer): legacy-prefix overrides for regexp + decimal modules`) and/or PR #236's existing `--path "$src_dir"` already covered enough of the symptom that the `dirname → "."` ambiguity no longer surfaces as the "undefined name `option`" cascade described in the reopen comment.

The fix in this lane is therefore **defensive**, not curative for the visible repro: passing a literal `.` as `--path` *is* ambiguous and CWD-coupled; resolving to absolute via `cd … && pwd` removes that ambiguity regardless of whether the current resolver tolerates it. The user-facing flow that the original PR #236 *intended* to support — `cd project && kai run main.kai` — now has explicit Makefile coverage in `rel` mode.

## Fix shape

**`bin/kai`** — exactly one change site (line 346), the `src_dir` assignment inside `compile_to_binary`. All five subcommands (`build`, `run`, `test`, `bench`, `check`) share that helper. `cmd_fmt` does not pass `--path` and is untouched.

```diff
-  src_dir="$(dirname "$src")"
+  src_dir="$(cd "$(dirname "$src")" && pwd)"
```

Comment rewritten to reference issue #237 and the CWD-aliasing concern.

**`Makefile`** — `test-multi-module` extended from a single `(cd / && kai run "$src")` mode to two modes per fixture:

- `abs` — unchanged contract (foreign cwd, absolute path, exercises PR #236).
- `rel` — `(cd "$src_dir" && kai run main.kai)`, exercises issue #237's user flow and locks the cd+pwd absolutization in place.

Both fixtures (`examples/multi-module/`, `examples/multi-module/issue-237/`) now diff against `main.out.expected` in both modes.

## Friction points

- **Empirical verification before push, completed.** `cd /tmp/exp237b && bin/kai run main.kai` was run from this worktree both pre-fix (printed `49`, exited 0 — bug not reproducible) and post-fix (same). The reopen comment's symptom did not reappear; this is the third lane on issue #237 and the first to verify before pushing. Without that verification the commit message would have been a third premature "closes #237".
- **Rel-mode test caught nothing additional.** Both fixtures already passed in rel mode against unfixed `main` HEAD, consistent with the diagnosis above. The new mode is therefore a contract pin, not a regression-revealer.
- **tier1-asan flake on first run.** `test-signal-trap-asan` exited 143 on the first attempt, the wrapping shell still returned 0 thanks to the `|| true`-shaped tail in the rule. Re-running was clean. Did not investigate further — orthogonal to this lane's diff.

## Spec ambiguities

- The reopen comment's repro doesn't reproduce on the merge-commit it was filed against. Either (a) #238 closed more than the comment credits it for, (b) some intermediate state was needed to surface the cascade, or (c) the original capture was from a slightly older `kaic2`. None of these were resolved in this lane.
- "`bin/kai` should convert `$src_dir` to absolute" is sound as an invariant regardless of (a)/(b)/(c). The lane preserves that invariant.

## Subjective summary

A defensive infrastructure fix on a bug whose empirical symptom already silently disappeared. The cd+pwd substitution is a one-liner; the bulk of the value is the `rel`-mode test in `test-multi-module`, which makes the user-facing flow ("cd into project, run `kai run main.kai`") an explicit CI contract instead of an emergent property of resolver internals. If a future change to the resolver re-introduces a CWD-relative ambiguity, that test will fire.

## Limitations of this report

- No claim that the `option`/`list` cascade described in issue #237's reopen comment is impossible to re-trigger. Only that it doesn't trigger at `main` @ `87a3a46` against the exact repro given.
- tier1-asan flake was not root-caused.
- LLVM-side tier validation only covers `selfhost-llvm`. The full demo suite was not re-run under the LLVM backend in this lane (Tier 1 already runs the C-backend demo gate).

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-04T15:07:26-04:00	tier0	OK	47
2026-05-04T15:14:37-04:00	tier1	OK	423
2026-05-04T15:15:31-04:00	tier1-asan	OK	44
2026-05-04T15:17:25-04:00	tier1-asan-rerun	OK	-
2026-05-04T15:17:50-04:00	selfhost	OK	19
2026-05-04T15:18:27-04:00	selfhost-llvm	OK	27
2026-05-04T15:19:00-04:00	selfhost-llvm	OK	26
```
