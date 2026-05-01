# Lane experience report — tco-dropmask-regression

Best-effort retrospective by the implementing agent. See limitations
at the bottom.

This lane was not instrumented prospectively per
`docs/lane-instrumentation.md` — no `/tmp/lane-${LANE}-start.txt`,
no TSV build log. The brief did not include the snippet. All
metrics below are reconstructed from agent context after the fact.
The integrator asked for the retrospective explicitly to fill in
the gap left by 17 non-instrumented PRs since 2026-04-28.

**Outcome: option B.** Three structurally different attempts at
re-landing rule 3 of the precise per-call-site dropmask all tripped
the same glibc abort during the kaic2 self-compile on Linux
ubuntu-clang. Lane closes with a documentation fixture +
`docs/known-regressions.md` § R6, no compiler change.

## Objective metrics (reconstructed)

- Start: not recorded.
- End: not recorded.
- Wall-clock: subjective ~2.5–3 h of active agent work between
  the user's first "B" decision (option-B for the lane scope, not
  the final outcome — see below) and PR-close.
- Build/test invocations (counts from agent context, may
  undercount):
  - `make kaic2` / forced rebuild (`rm kaic2 build/stage2.c &&
    make kaic2`): ~6 — initial build, sanity-check no-rule-3
    rebuild, restore rebuild, post-rebase rebuild, post-revert
    rebuild, final.
  - `make selfhost`: 4 — all OK byte-identical after each
    compiler.kai edit.
  - `make test-tco`: 2 — verified existing 50M-frame fixture
    still passes.
  - `make test-tco-regression` (the new target): 5 across three
    different shapes — initial PASS, sanity-check FAIL, restore
    PASS, post-redesign PASS, final option-B PASS.
  - `make tier1`: 1 (3:18 wall, all green locally).
  - **CI runs**: 2 (both red, same `malloc(): unaligned tcache
    chunk detected` abort).
  - Docker reproductions: ~6 (different `--ulimit stack=` /
    `--platform linux/amd64` / ASan-vs-plain combinations to
    chase the CI abort locally; only `--ulimit
    stack=67108864:67108864` reproduced).
  - `git log` / `git show` invocations: many (PR #41 force-push
    history mining, then post-CI-fail rebase decision).

## Compiler errors I encountered

None visible. The kaikai source edits compiled cleanly on every
attempt; selfhost stayed byte-identical after every edit. The
errors that mattered were RUNTIME under glibc — `malloc():
unaligned tcache chunk detected` on Linux during the kaic2
self-compile — which the kaikai source compiler does not report.

The CI log surfaces only `Aborted (core dumped)` with no frame
information, which forced the local reproduction work via Docker
+ `--ulimit stack=` matching CI. ASan revealed a stack overflow in
`kai_report_errors_loop` (which is goto-looped via PR #45's stage
1 mirror, so the recursion is no longer the root cause — but
ASan's redzones still pushed it past the limit). Without ASan,
under glibc strict, the same path corrupts heap and the next
malloc tcache check fires.

## Friction points

1. **Brief vs `main` reality (pivot 1, escalated to user, B
   selected for lane scope).** Initially briefed as "fix already
   landed in PR #41, regression test only". Code in `main`
   (cb026b4) was the conservative dropmask; issue #43 explicitly
   asks for re-land. Escalated, user chose to expand scope.

2. **`KAI_TRACE_RC` leak count was the wrong signal (pivot 2).**
   Build-phase emitter has known broken RC discipline (97% leak
   rate per memory entry 8). Switched to a C-text assertion:
   `grep -c 'kai_internal_drop(kai_xs)' build/tco-nth.c >= 1`.
   Worked locally; sanity check confirmed both directions.

3. **CI red on Linux despite local green (pivot 3, the lane
   killer).** Locally selfhost byte-identical, tier1 green, sanity
   check passed. CI on ubuntu-latest aborts with the same `malloc():
   unaligned tcache chunk detected` PR #41 first-try hit. The
   integrator's diagnosis ("mac/Linux drift in malloc tcache
   implementation, same pattern as R5 euler4") was correct.

4. **Branch was 13 commits behind `main` at the start
   (pivot 4).** Did not check `git diff main..lane --stat`
   before opening PR — a documented rule (memory entry 17:
   "Long lanes silently revert main if not rebased"). Main had
   landed PR #45 (TCO stage 1 mirror, fully closes R5) which
   should have helped. Rebased mid-investigation. The abort
   reproduced *after* rebase too, so R5 was not the load-bearing
   factor.

5. **Three structurally different rule-3 shapes all aborted on
   Linux** (the lane's actual finding):
   - Shape A (PR #41 first try, never merged):
     `tcrec_compute_site_dropmask(params, uses, args: [Expr], i,
     acc)`. Threaded `[Expr]` directly into the dropmask body;
     the body called `tcrec_args_have_evar(args, p)` from inside.
   - Shape B (PR #48 first commit `522b9df`): dropped `[Expr]`
     from the dropmask signature, added
     `tcrec_per_param_has_evar(args: [Expr], pnames: [String], i,
     acc)` as a new helper. The helper still threads `[Expr]` and
     calls `tcrec_args_have_evar`.
   - Shape C (PR #48 force-push `bd54356`): removed the helper
     entirely; replaced with `map(pnames, (p) =>
     tcrec_args_have_evar(xs, p))` inside `tcrec_rewrite_kind`,
     so the lambda captures `xs` via closure and no helper takes
     `[Expr]` as a parameter. Reused the already-exercised
     `map(args_e, (a) => emit_expr(...))` shape at line ~7827.

   All three abort identically. The PR #41 bisection's narrowing
   ("the call site of `tcrec_args_have_evar` from inside
   `tcrec_compute_site_dropmask` is the locus") under-described
   the bug — even routing the call through a closure capture
   trips the same imbalance. Without strict-malloc visible on
   macOS, the Mac `make tier1` cannot detect this class of bug;
   only Linux CI can.

6. **Docker reproduction took several iterations.** Default Docker
   on macOS uses Linux/amd64 emulation; default ulimit stack was
   `unlimited` which let kaic2 run cleanly. CI's runner has hard
   stack limit ≈ 64 MB. Reproduced only after
   `--ulimit stack=67108864:67108864`. ASan inside Docker also
   needed `--cap-add SYS_PTRACE --security-opt seccomp=unconfined`
   for gdb (which still failed to read registers due to QEMU's
   ptrace surface). Switched to ASan stack-trace alone.

7. **Worktree creation failed silently** at the start (Agent tool's
   `--isolation worktree`). Worked directly on the existing
   worktree branch, which was an artifact of an unrelated
   fixture lane the user had open.

## Spec ambiguities or interpretive choices

1. **Which "option" from issue #43 to try first.** Issue listed
   two: (1) find the refcount imbalance in stage 1's emit, (2)
   restructure the call site so the imbalance does not manifest.
   Started with (2), tried two distinct (2)-shaped designs,
   both failed. Did not attempt (1) — it requires runtime
   debugging of stage 1's perceus emit, which is beyond the lane's
   scope and arguably the right next step. R6 documents this.

2. **What to ship in the option-B close.** Choices:
   - (a) Drop everything; close PR #48; only the R6 doc lands.
   - (b) Keep the fixture as documentation; the C-text grep is
     gone, but the .kai file pins the canonical shape so a future
     re-land has a fixed target. **Chose this.**
   - (c) Keep the fixture AND grep, mark as XFAIL. Rejected: a
     test that fails on `main` is just noise.

## On the integrator's question — was `--effects-json` / `--effect-holes-json` useful?

**No, neither was used or needed.** This lane is compiler internals
(TCO emit, RC dropmask), not effect-row inference.

The signals that mattered:

1. **`git log` / `git show` on PR #41 force-push history** — the
   8 DEBUG bisect commits were the highest-value signal of the
   lane. They documented exactly which call-site was the locus,
   in commit-message prose. Without them, three rule-3 shapes
   would have taken much longer to converge on. Re-mining them
   after each CI failure was effective.
2. **Direct C-output inspection** (`sed -n '172p' build/tco-nth.c`)
   — confirmed rule 3 emitted the goto-block drop. Pivot 2's
   regression signal is built on this.
3. **Docker `--ulimit stack=N:N` matching CI** — without this,
   the abort reproduces neither locally nor in default Docker.
   CI's specific stack limit is the hidden parameter.
4. **ASan stack overflow trace** — even though the actual CI
   abort is in glibc tcache, ASan's stack-overflow detector
   fires earlier on the same path and gives a useful symbol
   trace (modulo `--ulimit` to constrain it). Catches a class of
   bug Mac-only `make tier1` cannot see.

If the prompt had been a kaikai *user* writing kaikai *programs*,
`--effects-json` / `--effect-holes-json` would likely have been
indispensable. For this lane the gap was elsewhere:

- A `--dump-tcrec` or `--dump-dropmask` flag printing per-call-site
  decisions would have shortened iteration on rule-3 shape design.
- Per-tag free counts in `KAI_TRACE_RC` would make leak count
  usable as a regression signal — the build-phase emitter's RC
  leak masks the rule-3 signal today.
- A `make tier-linux` target that runs Docker + ulimit-matched
  Linux build inside the dev loop would catch this class of bug
  before opening a PR (CI is the gate today, but the round-trip
  is ~5 minutes per attempt).

## Subjective summary

- Confidence in correctness:
  - For the **option-B close** (fixture + R6 doc): **high**. The
    fixture compiles + runs; selfhost byte-identical; `make tier1`
    green; R6 captures the diagnosis.
  - For the **rule-3 re-land**: **low**, regardless of code shape.
    Three structurally distinct attempts all hit the same abort.
    The structural fix likely sits in stage 1's perceus emit, not
    in `tcrec_compute_site_dropmask`.
- Hardest sub-task: pivot 5. Three failed shapes is a real signal
  that the bug is in a different layer than I was edit'ing. The
  lane should have stopped at shape B and gone straight to R6
  rather than try shape C — sunk-cost reasoning extended the
  lane by ~30 min.
- Easiest sub-task: writing R6. The PR #41 bisection commits
  handed me the diagnosis prose almost verbatim.
- Did the compiler help or hinder? Mostly hindered — the abort
  surfaces only on Linux CI, with no symbolic context locally on
  macOS. The kaikai source compiler does not see the bug because
  it lives in stage 1's emitted C, not in the kaikai source.
- Did the **integrator's process** help or hinder? Helped
  significantly. The "A/B/C with explicit 2-h time box" framing
  prevented unbounded debugging on a lane that was always going
  to need a different layer's fix. The "Mac local NO valida nada"
  rule was correct and cut a class of false-confidence error.

## Limitations of this report

- Self-report bias acknowledged.
- Context truncation: counts and error lists exclude anything
  that fell out of my visible context window.
- Single agent (Claude Sonnet 4.6 → Opus 4.7 mid-lane). Not
  generalisable across LLMs.
- The TSV-based instrumentation prescribed by
  `docs/lane-instrumentation.md` was not in the brief and was
  not started prospectively. Numbers above are reconstructed
  best-effort, not measured.

## Raw build log

Not captured (lane not instrumented prospectively). The agent's
terminal transcript holds the raw timestamps for each `make`
invocation but is not exported here.
