# Lane experience — bulk retro 2026-04-29 → 2026-05-01

Pinned 2026-05-01. Retroactive instrumentation report covering 17 PRs
that closed without per-lane `docs/lane-experience-*.md` discipline.
The `lane-instrumentation.md` template existed since the m12.5 era
but stopped being applied after the `m14-v1` lane (2026-04-28). This
doc reconstructs what GitHub records can tell us, calls out the
data the discipline was supposed to capture but lost, and pins the
recovery protocol.

## Methodology

Source data: `gh pr view --json` plus `gh api
repos/lnds/kaikai/issues/<n>/events` per PR. What GitHub records:

- **Wall-clock open → merged** — the visible PR window. Note that
  this overestimates "agent dev time" when the agent develops
  locally and only pushes near the end, and underestimates it when
  the integrator delays the merge for unrelated reasons.
- **Commit count + first-commit / last-commit timestamps** — bounds
  the agent's push pacing. For lanes pushed in a single batch
  (`first == last`), zero internal pacing is recorded.
- **`additions` / `deletions` / `changedFiles`** — per-PR diff size.
- **Force-push events** — `head_ref_force_pushed_event` in the
  timeline; signal of intra-lane rework (history rewritten, e.g.
  for fixup commits or to resolve review feedback).
- **Comments and review events** — review activity. For this
  project the integrator gave verbal feedback in the conversation
  rather than as PR comments, so this signal is near-zero by
  design and not informative.

Source data NOT available from GitHub:

- Compile errors hit during development, number of make/test
  invocations, time spent stuck on a particular bug.
- The agent's full transcript, intermediate code paths abandoned,
  pivots from initial plan.
- Whether structured JSON outputs (`--effects-json`,
  `--effect-holes-json`, typed-holes JSON) were used by the agent
  to recover from errors, or whether plain text error messages
  carried the work.
- Token / cost data for the agent runs.

## Per-PR data

| PR | Date | Lane | Files | Δ lines | Commits | Open→Merged | First→Last commit | Force-push | Comments |
|----|------|------|------:|--------:|--------:|------------:|------------------:|-----------:|---------:|
| #22 | 04-29 | r4-mc-specialisation (m4c Phase 1) | 10 | +1184/-275 | 5 | 0h24m | 1h26m | 0 | 0 |
| #23 | 04-29 | r4-mc-callsite-rewrite (m4c Phase 2) | 5 | +345/-70 | 2 | 0h08m | 0h00m | 0 | 0 |
| #24 | 04-29 | perceus-stage0-deeper (Tier 2) | 4 | +323/-86 | 2 | 0h14m | 0h03m | 0 | 0 |
| #25 | 04-29 | fibers-tier1-discard (R4) | 12 | +273/-51 | 1 | 0h09m | 0h00m | 0 | 0 |
| #26 | 04-29 | tooling-cli-syntax | 6 | +593/-31 | 3 | 0h32m | 0h01m | 0 | 0 |
| #27 | 04-29 | r4-m4c-body-subst (Phase 3 — close lie #3) | 14 | +2037/-385 | 4 | 0h13m | 2h13m | 0 | 0 |
| #28 | 04-29 | stack-guard-pages (Fibers Tier 1 #2) | 6 | +271/-31 | 1 | 0h20m | 0h00m | 1 | 0 |
| #29 | 04-30 | trap-exit (Fibers Tier 2 prep) | 11 | +388/-27 | 1 | 1h44m | 0h00m | 0 | 0 |
| #30 | 04-30 | perceus-tier-2 (close 3 items) | 8 | +731/-91 | 3 | 1h56m | 0h13m | 0 | 0 |
| #31 | 04-30 | m13-bit-ops (math/bits intrinsics) | 8 | +350/-1 | 1 | 0h15m | 0h00m | 0 | 0 |
| #32 | 04-30 | fibers-tier-2 (close 4 items) | 13 | +899/-121 | 4 | 6h23m | 0h00m | 0 | 0 |
| #33 | 04-30 | ci-tier1 (CI bootstrap + integrator workflow B) | 5 | +223/-27 | 5 | 0h27m | 0h21m | 0 | 0 |
| #35 | 04-30 | ci-daily (tier 2 cron) | 1 | +80/-0 | 1 | 0h05m | 0h00m | 0 | 0 |
| #36 | 04-30 | r5-euler4-linux-stack-overflow (band-aid) | 4 | +142/-15 | 1 | 0h09m | 0h00m | 0 | 0 |
| #38 | 04-30 | unboxing-phase2 (Tier 2.5) | 9 | +1170/-47 | 7 | 0h46m | 0h01m | 0 | 0 |
| #39 | 05-01 | m13-bit-dotted (dotted bit.* surface) | 8 | +159/-13 | 1 | 12h41m | 0h00m | 0 | 0 |
| #41 | 05-01 | r37-emitter-tco-goto (issue #37) | 9 | +772/-29 | 5 | 12h35m | 0h01m | 2 | 0 |

Aggregate: 17 PRs, **+10,140 / -1,400 lines** across **133 files**,
47 commits total, **3 force-push events** (rework signals: #28 once,
#41 twice), zero PR-level comments. Average open→merged: ~2h. With
two outliers removed (#39 and #41 both at 12h+, which include
overnight delay), median is closer to 24 minutes.

## Aggregate observations

### 1. Parallelism worked under CI gating

Until 2026-04-30 04:25 the project had no CI; merges were
gated by hand. From PR #29 onwards the CI workflow `tier1.yml`
landed (PR #33), and from PR #29 onwards `branches up to date with
main` was enforced (admin bypass available for the integrator).

Evidence of parallelism in the timestamps:

- 04-29 evening: PRs #22 → #28 closed within ~3h of each other,
  with up to 4 agents in different worktrees. Force-push count
  stayed low (1 event in #28) — agents did not collide on shared
  files.
- 04-30 morning: PRs #29 + #30 + #31 + #32 closed within 4h of
  each other, with parallel work on `compiler.kai` (Perceus),
  fibers, and stdlib. Zero force-pushes on those four.
- 05-01: PR #41 (TCO) had 2 force-pushes — the only PR in the
  set where intra-lane iteration is visible from GitHub data
  alone.

### 2. Force-pushes as the cleanest "rework" signal

Out of 17 PRs, only 3 force-push events fired (PR #28, PR #41×2).
Two interpretations:

- (a) Agents iterated cleanly on the first push attempt and the
  PR landed without history rewrites. This matches what we see
  in PR descriptions: structured M1-M7 milestone breakdowns,
  diagnosis trails written first time.
- (b) Agents iterated locally before the first push (squashing
  errors), so the rework happened pre-PR. This matches the
  `llm-authorship-baseline.md` (n=6, 04-26) observation: "All 6
  lanes have linear 'code → docs → done' commit sequences. Either
  agents are encountering no errors, or they are iterating
  privately and only pushing the green state."

Without per-lane TSV logs of make/test invocations, we cannot
discriminate between (a) and (b). Both are plausible. The 0
PR-level review comments are consistent with both: feedback was
verbal during the integrator session, not asynchronous on the
PR page.

### 3. Diagnosis trails captured in PR descriptions (when present)

PR #36 (R5 fix) is the canonical example. The PR description
records the full diagnosis trail:

- H1 (stack guard pages, PR #28 interaction) — ruled out: euler4
  spawns no fibers, signal handler is never installed
- H2 (KaiFiber kai_main_fiber initializer braces) — cosmetic
  warning only; C99 zero-fills missing fields
- H3 (`__builtin_*` ABI difference) — did not apply; arithmetic-
  only path
- H4 (real cause): emitter wraps self-tail-calls in stmt-expr
  with Perceus drops *after* the call → C compiler can't TCO →
  ~1M-deep recursion blows 8 MiB glibc default stack on Linux

This is what `lane-experience-*.md` was supposed to capture, and
it did land — just in the PR description rather than a separate
file in `docs/`. PR #41 follows the same pattern (M1-M6 milestone
breakdown, conservative bail-out reasoning, dropmask rule
justification).

PRs without a notable diagnosis trail are typically the
mechanically simple ones: #25 (single-commit fiber RC fix), #31
(intrinsic table additions), #35 (single-file workflow add), #39
(dotted surface alias rewrite). For those the lane-experience
template would have been mostly empty anyway.

### 4. Productivity rate — calibrated rough bounds

Apparent rate (Δ lines per visible Δt) varies wildly by lane
type. Bounds, not point estimates:

| Lane class | Examples | Apparent lines/min (open→merged) |
|---|---|---:|
| Mechanical: intrinsic table additions | #31, #39 | ~10–25 |
| CI infra / single-file workflow | #33, #35 | ~8–16 |
| Compiler-internal, contained | #24, #25, #28, #30 | ~14–40 |
| Compiler-internal, deep | #22, #27, #38 | ~25–50 |
| Compiler-internal, design-heavy | #32, #41 | hours-to-days |

These numbers are **not** sustained throughput. They reflect
"lines of new code that pushed at PR-merge time / minutes between
open and merge". The actual agent work (read context, plan, test,
iterate) is bounded above but otherwise unobservable.

The two longest open→merged windows (PR #39 and PR #41 at ~12h
each) cover overnight delays where the integrator was asleep.
Strictly: the agent finished, the human approved later. These
do not reflect agent work time.

## What this retro is missing

The `lane-instrumentation.md` template asks for these fields per
lane. From GitHub alone, the ones we cannot recover:

| Field from template | Recoverable from GitHub? |
|---|---|
| Wall-clock from shell timestamps | partial (commit timestamps only) |
| Build/test invocations from TSV log | **NO** — no agents wrote the log |
| Compile errors hit during development | NO |
| Number of compile-fix iterations | NO (only force-pushes are visible) |
| Pivots from initial plan | partial — only when the PR description records them |
| Use of `--effects-json` / `--effect-holes-json` | NO |
| Whether typed-holes JSON ever fed the agent's loop | NO |
| Token/cost accounting | NO |

The Tier 3 LLM-authorability bet
(`docs/llm-authorship-baseline.md`) cannot be evaluated against
the 17 PRs in this set. The bet's acceptance criterion ("an LLM
with JSON access completes the top 80% of typical functions
within one round of compilation") presupposes per-round
accounting that nobody captured. The compiler emits the JSON
endpoints, but whether agents *consumed* them is unknown.

## Recovery protocol — going forward

Pinned 2026-05-01 in this doc; should be folded into
`docs/lane-instrumentation.md` as the new operational standard.

### Per-lane requirements (mandatory for new PRs)

1. **Agent prompt embeds the snippet from
   `docs/lane-instrumentation.md`** (TSV logging at every `make`
   invocation, lane-experience-*.md output at end). No exceptions
   for "small lanes".
2. **Lane-experience output is part of the PR scope**, not a
   follow-up. The PR is not "done" until `docs/lane-experience-
   <lane>.md` is committed alongside the feature work.
3. **Specifically capture the LLM-friendly bet evidence**: in the
   lane-experience output, the agent must answer the prompt:
   *"At any point during this lane, would `--effects-json` or
   `--effect-holes-json` have helped you recover from a typing or
   effect-row error? Did you actually use them, or did the plain-
   text compiler output carry the work? Be concrete about which
   case."*

### Retroactive request (Wave 1 in flight)

The three Wave 1 agents (`tco-stage1-mirror`,
`tco-dropmask-regression`, `kai-fmt`) received a tmux-delivered
message on 2026-05-01 ~17:30 local asking them to write
lane-experience for their own lanes as part of cleanup. Their
output should land alongside their PRs (#45, #46, and the
not-yet-opened third). This is the bridge from "discipline
dropped" back to "discipline restored".

### LLM-friendly bet validation lane (post-Tongariki)

Already pinned in the kai.md session-7 plan:

> Tier 3 validation lane explícita en Anga Roa: instrument a NEW
> lane (parser-feature-medium-size) end-to-end. Two arms:
>   Arm A: agente con `--effects-json` / `--effect-holes-json`
>   instructed
>   Arm B: agente sin esas herramientas (solo text)
> Compare: rounds of compilation, errors hit, success rate.

This is the only rigorous way to close the bet. The data from
the 17 PRs in this retro is suggestive (agents shipped, the
project advanced), but does not isolate whether the JSON
contract or general LLM competence carried the work.

## Update to `docs/llm-authorship-baseline.md`

The baseline doc was n=6 lanes (2026-04-26). With this retro, the
baseline can be augmented to n=23 lanes through 2026-05-01, but
with the same caveat: from git+GitHub alone we measure visible
push windows and force-push counts, not agent dev time. A fresh
acceptance-criterion measurement requires the validation lane
above; the 17 lanes covered here are not measurable against the
Tier 3 acceptance criterion as recorded.

The 17 PRs in this retro shipped without regressions (all CI
green at merge time once CI existed; selfhost byte-identical at
each step before that). That is the empirical claim we can make
honestly: **17 LLM-authored compiler PRs, 17 successful merges,
0 regressions detected at landing time, with the caveat that
post-merge defects might surface later** (e.g., R5 was caught
by CI's first run on 2026-04-30, two days after PR #28 landed
the stack guard pages that the agent for R5 incorrectly
hypothesized as the SEGV cause).

Productivity-rate language must stay in the "apparent rate
(open→merged)" framing — never "lines per minute the agent
actually worked".

## What this doc is NOT

- Not a substitute for the per-lane retros that should have been
  written. Each of those would have richer data than this bulk
  reconstruction.
- Not a quality assessment of the work in the 17 PRs. The PRs
  shipped, the demos pass, the honesty targets hold. Those are
  separate measurements.
- Not a replacement for `docs/lane-instrumentation.md`. That
  doc is the forward-looking template; this doc is the backward-
  looking gap audit.
