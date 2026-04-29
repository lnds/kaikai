# Testing tiers

Decision pinned 2026-04-29 (post fibers + perceus honesty-targets).
Three tiers of test coverage with explicit cadence: per-commit, per-PR,
and daily. The point is that **velocity at the lane level (parallel
agents in worktrees, tag at lane close, semver per release) does not
require running every test on every commit — it requires being honest
about which tier ran and when**.

Sister docs:
- `docs/fibers-honesty-targets.md` — what "fibers as advertised" means.
- `docs/perceus-honesty-targets.md` — what "RC as advertised" means.
- This file — what "tested as advertised" means.

## The problem

Today's session uncovered three preexisting bugs whose commit messages
all claimed `make test clean`:

- R2 — `m8x_2_yield_interleave` SIGSEGV from v0.4.0 (R2 scheduler land).
- R3 — `interp.kai` panic from `9fe6f6d` (match-scrutinee leak plug).
- R4 — `let _ = fiber_spawn(...)` deadlocks (never had a fixture).

The pattern: **the gate the commit message named was not the gate that
ran**, or the fixture for the case did not exist. `make selfhost` was
honest (byte-identical fixed point); `make test` was honest in name
but partial in practice; `make demos-no-regression` was honest about
its baseline but the baseline did not include the regressions.

The fix is not "run more tests per commit" — that murders velocity
when 510 commits land in 10 days. The fix is to **name three tiers
explicitly, run each at its own cadence, and require the commit /
PR description to declare which tier ran**.

## Tier 0 — pre-commit (every agent, every commit)

~30-60 seconds. Runs inside each worktree, before any commit.

```
make selfhost              # byte-identical fixed point (C + LLVM)
make demos-no-regression   # baseline N
```

If Tier 0 fails, the commit does not happen. This is what keeps `main`
unbroken across parallel lanes — every commit lands on a green
self-host fixed point with the demo baseline holding.

**Disciplina**: agents never push commits that fail Tier 0. The
3-level gate's "Level 1 mechanical" maps onto Tier 0.

## Tier 1 — pre-PR (gate before merge)

~2-4 minutes. Runs once per PR before opening / before merge.

```
make test    # full target — see stage2/Makefile §test
```

Includes test-tokens / test-ast / test-types / test-env / test-infer /
test-infer-check / test-dump-typed / test-dump-mono / test-check /
test-run / test-blocks / test-llvm / test-modules / test-modules-path /
test-modules-qualified / test-modules-qualified-neg / test-holes /
test-effects / test-effect-runtime / test-link-runtime / test-sugars /
test-loop / test-reader / test-writer / test-time / test-intervals /
test-stdlib / test-protocols / test-demos-core / test-aspirational
+ test-m4c.

**Disciplina**: PR description includes the trailing line of `make
test` output (or a link to a CI run on the PR branch). The reviewer
verifies it before merging. **Without that line, the merge does not
happen** — "make test clean" as prose is not the same as
`make test exit 0` as fact.

The 3-level gate's "Level 2 invariant verifier + audit" and most of
"Level 3 demo gate" map onto Tier 1.

## Tier 2 — `make daily` (end of day / cron)

~10-20 minutes. Runs once a day on `main` HEAD, not on each PR.

```
make tier1
make stress-fixtures      # see "Stress fixtures" below
make coverage-probe       # see "Coverage probe" below
make rc-budget            # leaked / RSS / wall vs baseline
make demos-extended       # demos/9d9l + demos/vs + aspirational rerun
```

If Tier 2 fails, **`main` is not broken** — Tier 0 / Tier 1 already
gated every commit. Tier 2 produces a diagnostic that opens a lane
the next morning. Velocity is preserved.

The 3-level gate's "Level 3 programa no contemplado" maps onto Tier 2:
fixtures that were not anticipated when a feature shipped, exercised
nightly to surface latent regressions.

## Stress fixtures

Live under `examples/effects/` and `examples/stress/` (the latter
created on demand). They cover patterns that the per-feature suite
does not exercise but that REAL programs hit:

- **Concurrency**: `large_concurrent_drain.kai` (100 fibers + mailboxes
  + leak budget assertion).
- **Refcount**: `rc_leak_budget.kai` (programmatic check that
  `KAI_TRACE_RC` reports leaked < N for a known-shape program).
- **Polymorphic flow-through**: `m4c_flow_through.kai`
  (`fn f[a](x) = g(x)` chain that exercises body type substitution
  after Phase 3 m4c lands).
- **Negative regressions**: `m8_fiber_discard.kai`, `interp_recursive_walk.kai`
  (each documents a closed regression with an executable repro that
  panics if the fix regresses).

A stress fixture is graduated to Tier 1 once its bug is fixed and the
fixture stops being aspirational.

## Coverage probe

A script under `tools/coverage-probe.sh` that walks every `## ` heading
in the runtime / language design docs (`docs/effects.md`, `docs/
structured-concurrency.md`, `docs/actors.md`, `docs/fibers-impl.md`,
`docs/effects-impl.md`, `docs/effects-stdlib.md`) and matches each
heading against `examples/effects/*.kai` looking for a fixture whose
prefix or comment names the section. Headings without a fixture are
reported as **coverage gaps**.

This is preventive: when a doc grows a new section but no fixture
follows, the probe catches it before someone hits the bug at runtime.
A heading is allowed to lack a fixture only if it explicitly carries
`<!-- coverage: skip -->` (e.g. design discussion, not a feature).

## RC budget probe

A script that runs `KAI_TRACE_RC=1 stage2/kaic2 stage2/compiler.kai
2>&1 | grep alloc_total` and compares the numbers against
`docs/perceus-honesty-targets.md` Tier 2 targets. If `leaked` rises
above the threshold (today: 50 M; goal: 5 M post Tier 2), alarm.

The threshold is updated when Perceus Tier 2 sub-items land. Today the
threshold is "no regression from current 46.9 M"; once perceus_pass
multi-read dup ships, threshold drops to ~32 M; etc.

## Cadence summary

| Tier | When it runs | Owner | Gate |
|---|---|---|---|
| 0 | Every commit | The committing agent / human | Commit blocked |
| 1 | Pre-PR (and pre-merge if rebased) | The PR opener | Merge blocked |
| 2 | End of day / cron | The maintainer | Diagnostic opens a lane next AM |
| (Pre-release) | Before tag bump to a *.0 | Release captain | Release blocked |

Pre-release adds nothing new yet — Tier 2 is the de-facto pre-release
gate until 1.0 puts a stricter floor in place.

## What this document is NOT

- Not a CI configuration. The cadences above are the contract; CI
  (`gh actions` etc.) implements them mechanically when the project
  publishes a public repo. Today the work is done locally.
- Not a list of all `make` targets — only the ones that map to a
  tier.
- Not an excuse to skip Tier 1 on a PR because "the change is small".
  The R2 / R3 regressions that surfaced today were each a 30-line
  diff; both passed Tier 0 but broke Tier 1 silently because Tier 1
  was not run.
