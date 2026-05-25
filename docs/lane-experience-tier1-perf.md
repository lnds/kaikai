# Lane experience — tier1 wall-clock perf (lane `tier1-perf`)

**Scope:** speed up `make tier1` without touching the compiler, runtime,
stdlib, fixtures, or goldens — Makefile / workflow / testing-script infra
only. No coverage loss, no invariant change.

## Baseline & measurement

CI is the honest number: the `tier1` job ran **18m 09s** on the
self-hosted m2 runner (`shaka`) for the last green main run (PR #684).
The `tier0` job ran in parallel on a *second* m2 runner (`shaka-2`,
4m 16s) — so the CI critical path is the `tier1` job alone, not the sum.
(The brief's "shaka has a single registration" sharp edge is stale:
`gh api .../runners` now shows `shaka` + `shaka-2`, both labelled
`self-hosted macOS ARM64 m2`, plus a Linux `nibiru`.)

Local measurement was **contaminated by memory pressure**: a parallel
`resolve-extract` Claude lane was running in another tmux session, and
each `kaic2 main.kai` compile is ~4 GB RSS. The machine went into swap
(4.6 GB used of 6 GB), so absolute local wall-clock was unreliable
(a first `make tier1` clocked 900 s but that run included a cold build
+ swap thrash). What stayed reliable was the **relative** breakdown and
the **per-phase serial-vs-parallel deltas**, measured back to back so
both halves saw the same contention.

### Per-phase breakdown (kaic2 pre-cooked, serial)

| phase | s | | phase | s |
|---|---|---|---|---|
| stage2 `test` | 247 | | test-negative | 25.1 |
| └ 5 "costly" subtargets | 106 | | test-stdlib-modules | 19.7 |
| demos-no-regression | 35.9 | | audit-private-types | 10.0 |
| test-info (blocks) | 31.5 | | audit-private-records | 6.5 |
| | | | packages | 10.3 |

**The brief's a-priori bet was wrong.** It predicted stage2 `test` ≥80%
of tier1. In reality stage2 `test` is ~247 s and the *other* tier1
phases sum to ~196 s — roughly half the cost lives outside stage2
`test`. Optimising only stage2 `test` would have left the bigger pool
untouched.

## What worked: parallelise the short independent kaic2 loops

The decisive measurement: stage2 `test` under `make -j` gives a **terrible**
return — serial 247 s, `-j8` 212 s (1.17×), `-j14` 199 s (1.24×). Compiling
`main.kai` is **memory-bandwidth-bound** (4 GB RSS each); running several
in parallel saturates memory and barely helps (user-time 2.4× but
real barely moves), and `make -j` over the ~100 stage2 subtargets has
real artifact races (`build/$$name.c`, `.out`, etc. are written by 8–12
different targets — safe today only because fixture basenames happen to
be disjoint, not by construction).

By contrast the phases built from **many short, independent kaic2
invocations on small fixtures** are CPU-bound and scale almost linearly:

| phase | serial | parallel | speedup |
|---|---|---|---|
| test-negative (120 fixtures) | 25.5 s | 2.9 s | **8.9×** |
| test-info-blocks (51 blocks) | 30.9 s | 3.9 s | **7.9×** |
| test-stdlib-modules (50 mods) | 20.2 s | 2.5 s | **8.1×** |
| audit-private-types (14) | 10.5 s | 1.4 s | **7.7×** |
| audit-private-records (9) | 7.0 s | 1.1 s | **6.6×** |
| stage2 5 costly subtargets | 106 s | 72 s | **1.47×** |

Total saved ≈ **116 s** of the ~443 s clean serial tier1 test cost
(~26%), with the per-phase pass/fail counts byte-identical before/after
(104, 51, 50, 14, 9 respectively).

### Implementation pattern

Every parallelised script uses the same **worker-mode** shape, chosen so
the failure contract is provably unchanged:

1. A discovery phase (cheap, serial) builds a worklist of independent units.
2. `find ... -print0 | xargs -0 -P$JOBS -nK "$self" __worker ...` fans the
   units out; each worker compiles **one** unit and prints exactly one
   `OK`/`PASS`/`FAIL …`/`MISS …` line to stdout.
3. The orchestrator recomputes pass/fail counts from the collected lines
   and decides the exit code — the same "recount from a results file"
   discipline `test-negative.sh` already used to survive subshell counter
   loss. Parallelism is therefore invisible to the contract.

`JOBS` = `KAI_TEST_JOBS` if set, else `sysctl -n hw.ncpu` (macOS) ||
`nproc` (Linux) || 4. **`KAI_TEST_JOBS=1` restores fully serial
behaviour** — this is how every script was validated to produce
identical counts before being trusted in parallel.

The five stage2 "costly" subtargets (`test-tokens/ast/types/env/check`,
each compiling `main.kai` in a different dump mode) write **disjoint
output suffixes** (`.tok/.ast/.types/.env`; check writes nothing), so
they are race-free under `-j`. A new `test-costly-parallel` target
depends on `$(TARGET) build` (kaic2 fully cooked) and then
`$(MAKE) -j5` the five — the children find kaic2 up-to-date and never
race to rebuild it (the brief's "two parallel invocations rebuild the
binary" sharp edge).

## What was discarded

- **`make -j` over the whole stage2 `test`** — 1.24× ceiling
  (memory-bound) *and* real `build/$$name.*` artifact races. Bad
  risk/reward; the win is concentrated in 5 targets that we parallelise
  in isolation instead.
- **`-j` on the top-level `make tier1`** — would race the ~95 cheap
  stage2 subtargets and over-subscribe cores against the per-script
  `xargs -P`. We deliberately keep `make` serial and put all parallelism
  *inside* controlled scripts + the one costly target, so the speedup
  lands identically on CI (which runs `make tier1` with no `-j`) without
  editing the workflow at all.
- **test-packages** — 9-category matrix with shared git-repo / manifest
  state and inter-category ordering, not a homogeneous independent loop.
  ~8 s of potential saving against real race risk in bash + xargs over
  stateful fixtures. Not worth it.
- **CI matrix sharding across runners** — there are two m2 runners, but
  tier0 already occupies one in parallel; sharding tier1 across both
  would need disjoint subset targets + a join job, far more machinery
  than the in-process `xargs -P` win, and the runners are shared with
  other lanes' jobs. Deferred.
- **stdlib AOT pre-parse cache** — would touch the compiler (out of
  scope) and is the proper home of a separate cache lane (#452 family).

## Cost vs estimate

Estimated a paralleliseable pool of ~100 s; delivered ~116 s of
measured per-phase savings. The surprise that ate time was the
**memory-pressure contamination** from the parallel lane — it forced a
pivot from "measure absolute wall-clock" to "measure relative per-phase
deltas back-to-back", which is actually the more defensible methodology
anyway.

## Follow-ups

- **CI matrix sharding** across `shaka` + `shaka-2` for tier1 once the
  in-process wins are banked — only worth it if tier1 stays the critical
  path after this lane.
- **demos-no-regression (36 s)** is the largest single un-parallelised
  phase left; `demos/Makefile no-regression` builds each demo on both
  backends serially and is a candidate for the same `xargs -P` treatment
  in a follow-up (left out here to keep the lane to testing-infra scripts
  and avoid touching the demos build harness blind).
- The `build/$$name.*` artifact-name collisions in stage2 `test` are a
  latent `-j` hazard; if anyone ever wants full `-j` on stage2 `test`,
  the fix is per-target build subdirectories (~30 recipes), tracked as a
  separate refactor, not this lane.
