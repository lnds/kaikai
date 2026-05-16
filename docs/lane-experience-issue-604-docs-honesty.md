# Lane retro — issue #604 (docs honesty audit)

Closed 2026-05-16. Pure-docs lane: audited and aligned the three
honesty docs (`perceus-honesty-targets.md`,
`fibers-honesty-targets.md`, `cache-design.md`) plus four primary
catalog/roadmap docs (`stdlib-roadmap.md`, `stdlib-layout.md`,
`effects-stdlib.md`, `roadmap.md`) against the v0.69.0 reality.

## Scope as planned vs. as shipped

The brief enumerated 7 docs in obligatory scope. All 7 touched.
No new docs created. No lane-experience-*.md edited (bitácora
stays intact per the brief's out-of-scope rule). `CLAUDE.md` and
`docs/design.md` untouched (no factual stale claim found there
during this pass — both already reference primary docs rather
than embedding state).

Diff stats: 7 files, +140 / −52 lines net.

| Doc                          | Touched lines | Stalest claim found                                                        |
|------------------------------|--------------:|----------------------------------------------------------------------------|
| perceus-honesty-targets.md   |           +63 | "Wall time has not moved" — invalidated by Phase 3 unboxing (#383)         |
| fibers-honesty-targets.md    |            +8 | None major — doc was already current after #630 closed the same morning   |
| cache-design.md              |           +33 | "#597 (open) — `lower_protocols` boundary destruction" — #597 closed −1 d  |
| stdlib-roadmap.md            |            +9 | None — doc was current after the 2026-05-08 #367 refresh                  |
| stdlib-layout.md             |            +4 | `net/tcp.kai`: "blocking, no reactor" — R2 (#630) flipped it 2026-05-16    |
| effects-stdlib.md            |           +21 | Process `wait_or_kill` sidebar said "blocks the OS thread" — R1 (#611)    |
| roadmap.md                   |           +12 | "HEAD 0.54.3" — actual was 0.69.0 (~15 minor versions stale)               |

## Stalest claims (top 5, ranked by reader impact)

1. **`cache-design.md` — "#597 (open)" was the A.1 blocker.**
   #597 closed 2026-05-15 (one day before this lane ran). Without
   the fix, any reader planning A.1 would have over-estimated the
   prerequisite chain by ~300-500 LOC of unscoped work.
2. **`perceus-honesty-targets.md` — "Wall time has not moved".**
   Quoted the post-flip 5.74 s / 3.02 GB figures as current.
   Phase 3 unboxing (#383, v0.45.x) plus #82 sweep (#123) cut
   peak RSS by ~83% (3.02 GB → ~530 MB at the cc-link step) and
   moved the wall regression in the same direction. The original
   note framed the open debt as load-bearing; today it is closed.
3. **`effects-stdlib.md` — `wait_or_kill` "waitpid blocks OS thread".**
   The R1 reactor (#611) flipped `kai_default_process_wait` to
   park the fiber on a SIGCHLD self-pipe. The helper still does
   not ship — but for a different reason now (cancel mid-wait is
   not unwound; that's R2-in-Orongo, not R1 territory).
4. **`roadmap.md` — "HEAD 0.54.3" + "post #543 closure".**
   The Status snapshot lagged ~15 minor versions. Anyone reading
   for "what shipped last week" would have missed the entire
   reactor wave (R1+R2+R3), the cache chain (#452+#592+#597), and
   the Hanga Roa pre-condition fixes (#643/#644/#645).
5. **`effects-stdlib.md` — Signal §"Both points disappear once
   the m8.x scheduler lands".** m8.x scheduler shipped in v0.4.0;
   the reactor wave also shipped — but Signal was deliberately
   excluded from R1/R2/R3 (it needs an SA_RESTART-safe self-pipe
   shape + the `on_cancel(sig)` redesign from #107). The corrected
   sidebar names Orongo as the next opportunity, not "once m8.x
   lands".

## Pattern observed

**Lane retros + closing PRs form the correct chain of evidence;
the gap is between when a lane closes and when the relevant
honesty target / catalog gets the corresponding update.** The
`CLAUDE.md` §"Doc discipline" rule already says the closing PR
must update catalog docs, but the *secondary* docs (cache-design,
honesty targets) get less attention because they read like
narrative prose and the change is conceptual ("the blocker is
gone") rather than mechanical ("the row was missing").

Concrete example from this lane: `docs/cache-design.md`'s "#597
(open)" line was written on 2026-05-14 and was already wrong on
2026-05-15 when #597 closed; the #597 lane retro accurately
documented the close but the parent design doc kept the stale
banner because no rule pointed the #597 PR at this file. The
"#604 Doc discipline" section in CLAUDE.md catches catalogs
(stdlib-layout, stdlib-roadmap, effects-stdlib) but not design
docs.

## Method

For each doc I:

1. Read end-to-end, marking every `#N` citation and every
   "shipped"/"open"/"blocks"/"deferred" claim.
2. Verified `#N` state via `gh issue view N --json state,title`
   (29 issues / PRs verified in a single batch — see the lane's
   first Bash call for the list).
3. Cross-referenced runtime-touching claims against the relevant
   lane retros (`lane-experience-issue-611`, `-620`, `-630`,
   `-570`, `-582`, `-587`) and stdlib catalog file mtimes.
4. Strikethrough-d closed items with `~~old text~~ (shipped #N,
   YYYY-MM-DD)` form. Preserved history per the convention.

## Fixtures + coverage

No fixtures added — pure-docs lane. The acceptance gate is doc
correctness, not runtime behaviour. tier0 + tier1 are run as a
convention check (the workflow file's `paths-ignore` should skip
docs-only diffs but `make tier0/tier1` locally still runs).

## Real cost vs estimate

Estimate (brief): 0.3 day. Real: ~1.5 hours wall — closer to 0.2
day. The audit was faster than expected because (a) the
honesty-targets docs were already partly maintained by their
respective closing lanes (e.g. fibers-honesty had the R1/R2/R3
bullets in place before this lane ran), and (b) most catalog drift
the original #604 issue suspected had been pre-empted by the
2026-05-08 #367 reconciliation.

The 25-minute selfhost rebuild was the longest single step — and
unnecessary for the final report (kaic2 cc-link RSS was enough to
verify the perceus claim).

## Follow-ups left for next lanes

- **Process-cancel-aware wait.** R1's `kai_default_process_wait`
  parks on SIGCHLD only; Cancel mid-wait does NOT unwind. Queued
  for R2-in-Orongo per the corrected `wait_or_kill` sidebar.
- **Signal reactor port.** `Signal.await` still blocks the OS
  thread. Needs an SA_RESTART-safe self-pipe + `on_cancel(sig)`
  redesign. Orongo territory.
- **Cache layer A.1.** Both pre-blockers (#574 via #578, #597
  via the boundary-tagging lane) closed; A.1 is unblocked and can
  start whenever the cache lane resumes.
- **Doc-discipline rule extension.** The `CLAUDE.md` §"Doc
  discipline" enumeration of catalog docs to flip on close
  should grow to cover the three honesty targets +
  `cache-design.md`. Otherwise the same drift recurs next time a
  design-doc-shaped claim closes. Filed as a follow-up note —
  not edited in this lane to avoid scope creep.

## What this lane did NOT do

- Did not file a new issue for the doc-discipline rule
  extension. Belongs in a separate small PR or an inline
  CLAUDE.md edit lane.
- Did not normalise the inconsistent ~~strikethrough~~ vs
  deletion convention across the seven docs. Some sections
  delete-and-summarise (the fibers-honesty Tier 2 table), others
  strikethrough-and-annotate (the perceus-honesty Tier 2 table).
  Both are valid per the brief; chose to add new strikethroughs
  rather than retroactively change old prose.
- Did not re-run `make demos` or any runtime fixture. The brief
  permitted relying on existing lane retros + closing PRs as
  evidence, which I did for every claim flipped.

## Verification trace

The brief's pre-flight obligated reading:

- `gh issue view 604` ✓
- `docs/perceus-honesty-targets.md` ✓
- `docs/fibers-honesty-targets.md` ✓
- `docs/cache-design.md` ✓
- `docs/stdlib-roadmap.md` ✓
- `docs/stdlib-layout.md` ✓
- `docs/effects-stdlib.md` ✓
- `docs/roadmap.md` ✓
- `docs/editions.md` ✓ (no edits needed — current)
- `lane-experience-issue-611-reactor-r1.md` ✓
- `lane-experience-issue-620-reactor-r3.md` ✓
- `lane-experience-issue-630-reactor-r2.md` ✓ (verified existence)

Issues verified via `gh issue view`: #82, #123, #357, #440,
#452, #460, #461, #471, #568, #570, #574, #575, #578, #582,
#587, #592, #597, #599, #605, #611, #613, #620, #630, #643,
#644, #645, #647, #648, #649. 29 total in a single batched call.
