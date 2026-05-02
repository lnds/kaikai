# LLM Authorship Baseline (n=6 lanes)

Snapshot 2026-04-26. First attempt at recording empirical baseline data
for the Tier 3 strategic bet declared in `CLAUDE.md`:

> **LLM authorability** — Bet: with typed holes + structured JSON +
> stable rules, LLMs can author kaikai, even though current models know
> Python / Rust far better than effect-typed languages.
>
> Acceptance criterion: an LLM with JSON access completes the top 80%
> of typical functions within one round of compilation.

The acceptance criterion presupposes structured JSON output (`kai *
--json`), which lands with m7f. This document does **not** answer the
acceptance question — it establishes the baseline that future
post-m7f experiments will compare against.

## What can and cannot be measured from git history

Git records commits, not the development process between them. From
`git log` and `git diff` we can extract:

- **Commit timestamps** — when each landed.
- **Diff size** — lines added/deleted per file per lane.
- **Number of commits per lane** — granularity of the work.
- **File breakdown** — compiler vs docs vs examples vs runtime.

We **cannot** extract from git alone:

- **Compiler errors hit during development** — only the final compiling
  state is committed.
- **Number of compile-fix iterations** — agents typically squash these
  before push.
- **Wall-clock of the agent actually working** — commit timestamps
  reflect the moment of push, not the start of work. For lanes pushed
  in a batch at end-of-task, `last - first` underestimates wall-clock
  by an unknown factor.
- **Human interventions** — questions, course corrections, manual
  review feedback to the agent. None of these appear in git.
- **Quality of intermediate code paths** that were tried and abandoned.

Future lanes should be instrumented at the agent level (e.g., session
transcripts captured) for richer data.

## Data: 6 lanes from 2026-04-26

| Lane | Sub-tasks | Commits | First commit (UTC-4) | Landed commit | Visible Δt | compiler.kai Δlines | docs Δlines | examples Δlines |
|---|---:|---:|---|---|---:|---:|---:|---:|
| m7c-a | 1 | 2 | 11:20:26 | 11:21:40 | 1m 14s | +60 | +51 | 0 |
| m7c-b | 1 | 2 | 11:33:35 | 11:34:44 | 1m 09s | +137 | +28 | 0 |
| m7c-c | 1 | 2 | 12:04:00 | 12:04:55 | 0m 55s | +186 | +22 | 0 |
| m7c-d | 1 | 2 | 12:29:30 | 12:31:23 | 1m 53s | +310 | +84 | 0 |
| m7d | 5 sugars | 6 | 11:35:12 | 12:20:11 | 44m 59s | +571 | +16 | +160 |
| m7b15c | 1 (#15) | 4 | 14:18:54 | 15:25:42 | 1h 06m | +234 | +193 | +39 |

Notes on the table:
- **Visible Δt** is `landed - first commit` — the *push window*, not
  the development wall-clock. Especially misleading for m7c-a/b/c/d
  which were pushed in a single batch each.
- **m7d** is the only lane where intermediate sub-tasks were committed
  separately (5 sugars, 5 commits + close), giving a partial picture
  of intra-lane pacing.
- **m7c-a through m7c-d are sequential** in clock time (11:20 → 12:31),
  totalling **1h 11m visible**. Same agent context, four sub-tasks.
- **m7c and m7d ran in parallel**: m7c-b at 11:33 and m7d §10 at 11:35.
  Confirms parallelisation worked at the LLM level.
- **m7b15c is the only lane with non-trivial doc revision** (+193
  lines), reflecting that #15 was scope-limited and required
  re-documenting what landed vs what was deferred.

## Productivity rates (calibrated by visible Δt — lower bounds)

| Lane | Total Δlines | Visible Δt | Apparent rate (lines/min) |
|---|---:|---:|---:|
| m7c-a | 111 | 1m 14s | ~90 |
| m7c-b | 165 | 1m 09s | ~143 |
| m7c-c | 237 | 0m 55s | ~258 |
| m7c-d | 468 | 1m 53s | ~248 |
| m7d | 789 | 44m 59s | ~17 |
| m7b15c | 376 | 1h 06m | ~5.7 |

The m7c rates are **clearly artefacts** — no agent writes 250 lines/min
of effect-emitting LLVM backend code. The numbers reflect that m7c was
written off-line and pushed in a batch. The m7d and m7b15c rates,
where intermediate commits were captured, are closer to actual pacing.

A rough realistic baseline: **5-20 lines/min of compiler.kai code in
sustained work**, with high variance based on subsystem complexity.

## Qualitative observations

1. **Sub-task granularity matters for measurement.** m7d's choice to
   commit each sugar separately gave us 5x the temporal data points
   m7c sub-tasks gave us. For future lanes, prefer fine-grained commits
   even if they will be squashed.

2. **The compiler is being built primarily by LLM agents (Claude).**
   This is itself the experiment in progress: the present-tense
   self-host bet is whether an LLM-built compiler reaches MVP D before
   refinements and regressions outpace velocity. So far (n=6 lanes,
   1 day), the answer trends positive: 6 lanes closed, 0 regressions,
   selfhost stays green at every block boundary.

3. **No compile errors visible in pushed history.** All 6 lanes have
   linear "code → docs → done" commit sequences. Either (a) the agents
   are encountering no errors, or more likely (b) they are iterating
   privately and only pushing the green state. We need pre-commit
   instrumentation to know which.

4. **Doc revision is non-trivial for scope-limited work.** m7b15c
   reverted 192 lines of docs to clarify what landed vs what was
   deferred. Lanes that intentionally narrow scope mid-flight need
   extra doc time.

5. **Test fixtures track tightly with feature scope.** m7d added 5
   sugars and 32 fixture files; m7c-* sub-tasks added zero fixtures
   (they relied on existing effect tests for verification). Fixture
   count is a proxy for "newly testable surface area".

## Limitations of this baseline

- **n = 6 is anecdotal.** No statistical power. Useful only as a "before"
  reference point for post-m7f comparisons.
- **Single project (kaikai).** No comparison to other languages.
- **Single agent family (Claude).** No comparison across LLMs.
- **No control corpus.** We do not know what the same lanes would have
  taken in Python or Rust.
- **Visible Δt is not wall-clock.** True development time is
  underestimated, possibly by 10x for batched lanes.
- **No iteration counter.** Cannot say how many compile-fix rounds the
  agents went through.

This baseline is honest qualitative description, not evidence.

## What richer instrumentation would capture

For future lanes (m7e13, m12.8, m5.x, etc.), the following data would
be valuable:

1. **Session transcript** — full prompt + agent response + tool calls,
   per lane. Enables counting iterations and characterising errors.
2. **Pre-commit error log** — every `make all` or `make test` failure
   the agent encountered before the green push.
3. **Agent self-report** — at end of lane, the agent summarises
   subjective friction points (e.g., "needed 3 attempts at handler
   dispatch type rule").
4. **Time-on-task** — explicit start/end timestamps, not derived from
   commits.

Adding (1) and (4) is cheap (just save the conversation log). Adding
(2) requires a thin wrapper around `make`. Adding (3) requires the
prompt to ask for it.

## Comparison points planned

- **Post-`!` postfix lane (m7e13)** — first lane with intentional
  instrumentation. Will set the bar for "instrumented baseline".
- **Pre-m7f vs post-m7f corpus** — same set of tasks run through the
  agent before and after `kai * --json` lands. Measures the marginal
  value of structured output for LLM authorship.
- **Cross-language comparative corpus (post-MVP B)** — a stable set of
  10-20 functions in kaikai, Python, and Rust, run through the same
  LLM with the same prompt structure. Measures whether kaikai requires
  fewer compile-fix iterations than the better-known control languages.

## Trampas to avoid in subsequent experiments

1. **Confirmation bias.** The author of this doc is itself an LLM
   working on kaikai. Self-reporting on whether kaikai feels easy is
   worthless. Use objective metrics: rounds-to-pass, tests passing,
   lines-to-converge.
2. **Pretraining advantage.** GPT-4 / Claude know Python orders of
   magnitude better than kaikai. Any cross-language comparison must
   normalise by giving the model the language reference docs in-prompt.
3. **Selection bias on tasks.** A corpus chosen by the language
   designer will favour the language. Use independent task lists
   (e.g., HumanEval, MBPP, Rosetta Code subset).
4. **N too small.** n < 20 functions is anecdotal. For a citable result,
   plan n >= 50.
5. **Conflating "compiles" with "correct".** Tests must pass, not just
   typecheck.

## Status

Baseline established. No conclusions drawn. Next data point is m7e13
post-cierre, with intentional capture of session transcript.

---

## Addendum — A/B experiments 2026-05-02 (n=2 experiments × 2 arms)

The original baseline (n=6 lanes, 2026-04-26) deferred the
acceptance question to instrumented A/B comparisons. Two such
experiments have now run.

### Headline

**JSON tooling is NOT load-bearing for the slices tested.** The
actual differentiators in the harder slice (handler composition)
were ASAN + compiler source reading + persistence — none of which
the bet was about. The bet's framing remains untested for one
slice (deep row-polymorphism without codegen/runtime components)
but no real feature has hit that slice cleanly.

### Experiment 1 — stdlib mirror (`Trace` effect)

| Metric | Arm A (JSON tools) | Arm B (plain text) |
|---|---:|---:|
| Wallclock to green tier1 | 13m 44s | 12m 26s |
| Compiler errors hit | 2 | 1 |
| JSON tool invocations | 3 (1 effects-json, 1 effect-holes-json, 1 types-json NOT-A-FLAG) | 0 (banned) |
| Outcome | shipped | shipped |

**Verbatim from arm A retro**: JSON invocations were "deliberate,
**for this report**" — i.e., made after the work was complete.

**Verbatim from arm B retro**: "the plain-text path was sufficient
end-to-end for this lane".

Bonus: arm A surfaced **R8** (Phase 2 unboxing × string interpolation
regression) during the lane.

Full writeup: `docs/tier3-experiment-2026-05.md` §*Experiment 1*.

### Experiment 2 — handler composition (`with_log_prefix`)

| Metric | Arm A (JSON tools) | Arm B (plain text) |
|---|---:|---:|
| Outcome | ✅ shipped | ❌ blocked, doc-only PR |
| Bugs hit | R9 + R11 | R9 + R10 |
| Real differentiator | ASAN + compiler source reading | (didn't pursue ASAN/source) |

Both arms hit the same compiler/runtime bugs. The differential was
**persistence + diagnostic tools (ASAN) + reading compiler internals**,
NOT JSON tools.

**Verbatim from arm A retro**: JSON tool invocations made
"after the fix landed".

**Verbatim from arm B retro**: "JSON would not have helped — the
bug is in codegen not in the type system" / "the experiment 2
embargo cost me [zero time]".

Bonus: experiment 2 surfaced **R9** (handler-clause closure-capture
gap), **R10** (parameterised + self-delegating handler crash), and
**R11** (state-read UAF on second op call). All three since closed
(R9 by issue #60, R10 + R11 by issue #61).

Full writeup: `docs/tier3-experiment-2026-05.md` §*Experiment 2*.

### Combined direction

The bet "JSON tooling accelerates LLM authorability of effect-typed
languages" is **not directionally supported** by the available data:

- 2/2 experiments returned negative or null direction.
- Both arms in both experiments converged on the same conclusion
  in their retros.
- The bonus value of the experiments was the surfaced bugs (R8, R9,
  R10, R11) — none of which the JSON tools would have prevented.

The bet may still be correct for an untested slice (typed-hole-
driven type discovery in larger codebases, where the agent has no
signature template and infers it from compiler feedback). A future
experiment 3 could target that slice synthetically. Until such an
experiment runs, the bet should be treated as **aspirational, not
validated**.

### Operational implications

- **Do not invest in additional JSON tooling solely on the basis of
  the bet** — the existing surface (`--effects-json`,
  `--effect-holes-json`, typed holes) is adequate for the slices
  where it's used; expanding it without evidence of demand is
  premature.
- **m11 diagnostics quality (Anga Roa)** should be justified on its
  own merit — Elm/Rust-grade error UX for human developers — not as
  bet validation.
- **Fund ASAN in CI**: `make tier1-asan` gate (cron daily.yml or
  per-PR) is the single highest-leverage operational change
  suggested by the experiments. Catches R10/R11-class bugs before
  users hit them.
- **Future agent prompts in advanced lanes** should explicitly
  suggest "read the relevant emit/perceus source if you hit codegen
  errors" — this was the load-bearing differentiator in experiment
  2.
- **`--types-json` does not exist** despite Tier 3 narrative; do not
  advertise it in agent briefs.

### Limitations of the combined finding

- n=2 experiments per slice = 1; direction-setting only.
- Same agent personality (Claude) in all four arms.
- Self-report bias acknowledged. Counter-evidence: A and B
  converged on the same conclusion across both experiments,
  suggesting low self-serving bias.
- Wallclock numbers include sleep windows where the integrator was
  unavailable; compute-time is estimated.
- One slice remains untested (deep row-polymorphism with no
  codegen/runtime component); the bet may hold there.
