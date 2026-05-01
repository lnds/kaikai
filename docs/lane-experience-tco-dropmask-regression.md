# Lane experience report — tco-dropmask-regression

Best-effort retrospective by the implementing agent. See limitations
at the bottom.

This lane was **not** instrumented prospectively per
`docs/lane-instrumentation.md` — no `/tmp/lane-${LANE}-start.txt`,
no TSV build log. The brief did not include the snippet. All
metrics below are reconstructed from agent context after the fact
and are subject to recall bias. The integrator asked for the
retrospective explicitly to fill in the gap left by 17
non-instrumented PRs since 2026-04-28.

## Objective metrics (reconstructed)

- Start: not recorded.
- End: not recorded.
- Wall-clock: not measurable; subjective span ~45 min of active
  agent work between the user's "B" decision and PR-open. The
  `make tier1` invocation alone took 3:18 (real time, captured in
  the agent's terminal output).
- Build/test invocations (counts from agent context, may
  undercount):
  - `make kaic2` (or forced rebuild via `rm kaic2 build/stage2.c &&
    make kaic2`): 3 invocations, 3 passes — once after the initial
    edit, once for the sanity-check no-rule-3 build, once to
    restore rule 3.
  - `make selfhost`: 2 invocations, 2 passes (byte-identical both
    times).
  - `make test-tco`: 1 invocation, 1 pass (validated existing
    50M-frame fixture still works post-edit).
  - `make test-tco-regression` (the new target): 3 invocations —
    1 PASS (rule 3 enabled), 1 FAIL (rule 3 disabled, sanity
    check), 1 PASS (rule 3 restored).
  - `make tier1`: 1 invocation, 1 pass (3:18 wall, includes the
    full `make test` chain and demos baseline).
  - Direct `kaic2 + cc` runs of the fixture for inspection: ~3.
  - `KAI_TRACE_RC=1 ./build/list_nth_shape`: 2 runs (with rule 3,
    and with the sed-edited C that simulates no-rule-3).

## Compiler errors I encountered

None visible. The kaikai source edits compiled cleanly on the
first attempt. The only "error" surfaced was on a fixture
recompile when I had `2>&1`-folded a previous `kaic2` stderr into
the .c file from a prior run — easy to spot and recover from. No
actual compiler diagnostics had to be parsed.

## Friction points

1. **Brief vs `main` reality (pivot 1).** The brief stated "the
   fix is already landed in PR #41; this lane is regression
   coverage only — do NOT modify the dropmask." The actual
   `tcrec_compute_site_dropmask` in `main` (cb026b4) is the
   *conservative* version with a comment that explicitly cites
   the glibc tcache abort that prevented the precise version from
   landing in PR #41. Issue #43's title is "tco: re-land precise
   per-call-site dropmask". The PR #41 *description* documents the
   3-rule precise dropmask, but the merged *code* shipped only
   rules 1 and 2. I escalated, the user chose option B (full
   re-land + regression test), and the lane scope expanded from
   "test only" to "compiler edit + test + sanity check".

   What helped converge: reading `git log --all` for the
   PR #41 force-push history surfaced the 8 DEBUG bisect commits
   (`f43efdd` through `82b4a66`), the conservative switch
   (`5131116`), and the dead-helper removal (`1dc4aa1`). The
   removed walker code in `1dc4aa1`'s diff was the verbatim
   `tcrec_args_have_evar` family I needed to reintroduce.

2. **`KAI_TRACE_RC` leak count was the wrong signal (pivot 2).**
   The initial test design was "build a 100k-element list, walk
   it tail-recursively, assert the cons-cell leak count is below
   a threshold." Empirically: with rule 3 the run reports
   `leaked=199874`; with rule 3 sed-removed from the generated C
   the same run reports `leaked=199875` — a **one-cell**
   difference, not the ~99,999 the issue's diagnosis predicts.

   Diagnosis: `kai_make_range_loop`'s emit is
   `kai_cons(kai_internal_dup(kai_n), kai_incref(kai_internal_dup(kai_acc)))`.
   The `kai_incref(kai_internal_dup(kai_acc))` is a double-incref
   on `acc` for every cons allocation; the cons cell takes one
   ref, leaking the second per build iteration. By the time the
   walk starts, every cons cell already has a stale +1 ref, so
   the per-iteration `kai_internal_drop(kai_xs)` that rule 3 emits
   only ever brings the cell from rc=2 to rc=1 (not freed) on
   every iteration except the first. The leak count is dominated
   by the build-phase emitter bug (memory note:
   "stage 2 emitter has no RC discipline — 97% of allocs leak"),
   not by rule 3's effect.

   Pivot: replace the leak-count assertion with a C-text
   assertion. `make test-tco-regression` now greps the emitted C
   for `kai_internal_drop(kai_xs)`. With rule 3: count=1 (the
   goto-block drop). Without: count=0 (no drop, because nth_loop
   has no exit-side drop of `xs` — the match scrutinee handles
   that and the goto bypasses it). The signal is binary and
   directly tests rule 3's effect.

   Sanity check confirmed both directions: with rule 3 disabled
   the regression test fails with the right diagnostic; with rule
   3 restored it passes.

3. **Worktree creation failed silently (minor).** The agent's
   `Agent --isolation worktree` call returned `WorktreeCreate
   hook failed: no successful output`. Fell back to working
   directly on the existing worktree branch. No actual
   correctness impact, just lost the isolation benefit.

4. **`cd <subdir>` jumped to a different repo via zoxide
   (minor).** `cd stage2` from the worktree path
   `/Users/ediaz/work/src/github/lnds/kaikai.tco-dropmask-regression`
   resolved to `/Users/ediaz/work/src/github/lnds/kaikai/stage2`
   (the canonical clone, not the worktree). Worked around by
   using absolute paths in all `Bash` invocations. Cost ~1
   confused diagnostic round-trip.

## Spec ambiguities or interpretive choices

1. **Issue #43 prescribed two options for dodging the glibc
   tcache abort:** (1) find the refcount imbalance in stage 1's
   emit for the call-site shape, (2) restructure the call site so
   the imbalance does not manifest. I chose option 2 verbatim:
   `tcrec_compute_site_dropmask` no longer takes `[Expr]` —
   instead the caller (`tcrec_rewrite_kind`) computes a per-param
   `Int` bitmask via `tcrec_per_param_has_evar` and threads only
   the Int through. The walker (`tcrec_args_have_evar` and
   friends) is reintroduced as-is from the dead-code diff in
   `1dc4aa1`, but it is now called only from the per-param
   computation site, never from inside `tcrec_compute_site_dropmask`.

   Decision rationale: the issue's bisection was unambiguous
   ("the call site's own emit is the locus, not the walker"). The
   `Int`-bitmask shape is one step flatter than the `[Bool]` the
   issue proposed — list-typed args are exactly the kind that
   trigger stage 1's perceus dup-wrap that PR #41 hit. An Int
   parameter is structurally trivial to dup/drop and avoids the
   suspect path entirely. **CI on Linux ubuntu-latest is the
   only authoritative validation that the abort is gone** —
   macOS Darwin malloc tolerated the original imbalance and could
   not catch it locally. Pending PR opens; if CI fails we iterate.

2. **Threshold for the regression signal.** Chose strict
   "exactly 1 occurrence" via `grep -c >= 1` rather than a leak-
   count threshold. The C-text signal is unambiguous and does not
   depend on the noisy upstream RC discipline. Trade-off: a
   future emit refactor that inlines or renames the drop would
   need to update the test, but the goal is "this specific drop,
   in this specific shape" so the brittleness is desirable.

## On the integrator's question — was `--effects-json` / `--effect-holes-json` useful?

**No, neither was used or needed.** This lane is compiler
internals (TCO emit, RC dropmask), not effect-row inference.
There were no holes in the kaikai source, no effect annotations
to query, no row-polymorphic mismatches to diagnose. The signals
that mattered were:

1. **`git log` + `git show` on PR #41's force-push history.**
   The DEBUG bisect commits (8 of them) made the abort's locus
   explicit: stubbing the helper body kept the crash; stubbing
   the call site fixed it. The conservative switch's commit
   message (`5131116`) was the design rationale in prose. The
   dead-helper removal (`1dc4aa1`) was the recoverable shape of
   the walker. None of this is queryable from `--effects-json`
   or `--effect-holes-json`; it lives in the VCS history. **This
   was the highest-value signal of the entire lane.**
2. **The generated C output (`stage2/build/list_nth_shape.c`).**
   Reading `sed -n '172p'` on the fixture's emitted C confirmed
   that the goto block contained `kai_internal_drop(kai_xs)`
   exactly where rule 3 would put it. This was also the
   regression test's mechanism. A hypothetical
   `kai dump --tcrec-decisions` would have given the same data
   in a structured format and is probably worth ~10 lines of
   compiler code; I did not propose it inside this lane to stay
   in scope.
3. **`KAI_TRACE_RC=1` runtime trace.** Useful as a smell-check
   for "did anything I changed actually do something." Insufficient
   as a *regression* signal because the build-phase emitter swamps
   the metric (pivot 2). A per-tag free counter (~5 lines in
   `runtime.h`) would make `KAI_TRACE_RC` usable for cons-cell
   regressions specifically, but that crosses into runtime
   changes which are a separate lane.
4. **The CLAUDE.md `Things to avoid` and the inline comment in
   `tcrec_compute_site_dropmask`.** Both pointed at the same
   constraint (no `[Expr]` through the suspect call shape) from
   different angles. The inline comment was added in `5131116`
   precisely as a forward-pointer to anyone re-attempting the
   re-land — a high-value practice the project should keep up.

If the prompt had been a kaikai *user* writing kaikai *programs*,
`--effects-json` / `--effect-holes-json` would likely have been
indispensable for typed-hole completion and effect-row diagnosis.
For a compiler-internals lane on the C emit pipeline, the
existing signals are sufficient — the gap, if anything, is
compiler-internal: a `--dump-tcrec` or `--dump-dropmask` flag
that prints "for fn `nth_loop`, call site at L:C, dropmask=0bXX
because params=[xs:LUAt count=1 not-in-args, i:LUAt count=2]"
would have been the single most useful diagnostic I could have
asked for, and would have shortened the second pivot from
~15 min to ~2 min.

## Subjective summary

- Confidence in correctness: **high** for the macOS path
  (selfhost byte-identical, tier1 green, sanity check passed in
  both directions, generated C matches PR #41's first-attempt
  shape modulo the helper dispatcher). **Medium** for the Linux
  path (the actual abort PR #41 hit lives on glibc; I cannot
  reproduce locally and depend on CI for the final answer).
- Hardest sub-task: pivoting from the leak-count regression
  signal to the C-text signal. Required reading the build-phase
  emit and reasoning through the per-iteration RC trace. The
  one-cell delta was the surprise that forced the pivot.
- Easiest sub-task: implementing the new
  `tcrec_compute_site_dropmask` + `tcrec_per_param_has_evar`
  pair. The `1dc4aa1` diff handed me the walker code; only the
  signature change and bitmask handling were new.
- Did the compiler help or hinder? Mostly help. kaic2 compiled
  the change first try, no parser/check errors, no surprises.
  The hindrance was that the existing tooling can introspect the
  inferred effect rows but not the dropmask decisions — the latter
  would have been the right diagnostic for this lane.

## Limitations of this report

- Self-report bias acknowledged.
- Context truncation: counts and error lists exclude anything
  that fell out of my visible context window. The conversation
  did not approach the truncation threshold but several Bash
  outputs are paraphrased rather than verbatim.
- Single agent (Claude Sonnet 4.6 → Opus 4.7 mid-lane). Not
  generalisable across LLMs. Model handoff happened at the
  user's `/model` invocation after the initial brief-vs-reality
  escalation; the implementation work was Opus 4.7.
- The TSV-based instrumentation prescribed by
  `docs/lane-instrumentation.md` was not in the brief and was
  not started prospectively. Numbers above are reconstructed
  best-effort, not measured.

## Raw build log

Not captured (lane not instrumented prospectively). The agent's
terminal transcript holds the raw timestamps for each `make`
invocation but is not exported here.
