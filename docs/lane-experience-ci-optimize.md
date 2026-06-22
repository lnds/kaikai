# Lane experience — CI wall-clock optimization (lane `ci-optimize`)

**Scope:** cut per-PR CI wall-clock toward ~15 min without weakening the
merge gate (tier1 is the only Required check) or dropping any coverage.
Analysis-then-refactor: measure on the real runner first, change second,
validate locally before any push. Build/CI/Makefile infra only — no
compiler, runtime, stdlib, fixture, or golden changes.

## Measured baseline (the honest number is CI, not local)

Local mac (14-core / 24 GB, no `llvm-config` → C-only, matching the tier1
runner) is fine for *relative* splits; the *absolute* PR number is CI on
`ubuntu-latest`. From the GitHub Actions REST API over 6 green `main` runs
(no outliers):

| Job | Workflow | Duration | Required | On PR critical path |
|---|---|---|---|---|
| **tier1** | tier1.yml | **~27m** | yes | **yes (the gate)** |
| tier1-native | tier1-native.yml | ~20.5m | no | no |
| tier0 | tier1.yml | ~12.5m | no | no (parallel) |
| tier1-asan | tier1-asan.yml | ~9.5m | no | no |

The brief's "~27 min" was right; the older `lane-experience-tier1-perf.md`
"18 min" is **stale** — that lane ran on self-hosted m2; `runs-on` moved to
github-hosted ubuntu in `54afa108`, and ubuntu is ~3.5× slower on this load
(`cc` kaic2 `-O2`: 55 s local → 197 s CI; bundle regen: 16 s → 45 s).

### tier1 critical-path breakdown (CI log timestamps)

| Phase | CI time |
|---|---|
| Setup + checkout + apt | ~25 s |
| stage0/1 build (`-O0`) | ~20 s |
| regen `build/stage2.c` | ~45 s |
| **`cc` kaic2 `-O2`** | **~197 s** |
| 5 costly self-compiles (`-j5`) | ~128 s |
| **~1100 light fixtures + caches** | **~900 s** |
| demos + fmt + bench + check + negative + info + audits | ~310 s |

The grout is the ~1100-fixture light pool (~15 min). 1114 fixtures run;
heaviest families stdlib (158), sugars (144), effects (114).

## What the questions resolved to (measured, not assumed)

- **Must kaic2 build at `-O2`?** Yes. `-O0` cc is 3.5 s vs 55 s, but kaic2
  then runs **2.59× slower** over the fixture pool (405 fixtures: 69 s @ -O2
  vs 179 s @ -O0; output byte-identical). Net per job: -O2 ~124 s, -O0 ~182 s.
  `-O2` stays — **not a lever**.
- **Redundant fixtures?** None deletable. The brief's `net_tcp/udp`
  duplicates are already gone (only in `examples/effects/`); the three
  `hello` fixtures are all different. A body-hash sweep found 20 same-body
  groups — every one is legitimate (fmt idempotence input≡expected, pos/neg
  pairs sharing a lib, same source different invocation mode, identical code
  anchoring distinct issues via comments). Verifies the #626 warning.
  **Fixture deletion is not a lever.**
- **Overlap?** `demos-no-regression` runs in both tier0 and tier1, and each
  job re-bootstraps kaic2 (~260 s). But tier0 is off the critical path, so
  this is wasted *compute*, not wall-clock. The lever is build-once +
  sharding, not de-duping tier0.
- **C vs native (the big one):** tier1 (C) is mostly **front-end**
  verification (AST/typer/diags/reject/fmt) — backend-agnostic by
  construction (one KIR feeds both lowerers). tier1-native already runs
  **every** entry-point fixture on BOTH backends (native target vs C
  oracle) and diffs output. They are **complementary, not redundant**: no
  same-fixture double-run exists to remove. The only redundancy is the
  **build** (three from-scratch kaic2 bootstraps). Decision: keep C as the
  oracle, do NOT add a second native front-end run (pure redundancy), and
  remove the redundant build via build-once-per-variant. native is the
  default and gets equal treatment (its own sharded gate).

## What shipped

1. **Shard the tier1 light pool across separate runners.** `-j` in one
   runner is bandwidth-capped (1.24×, per the prior retro); separate runners
   each get their own memory bus and scale near-linearly (the retro measured
   7–9× on these loops). `stage2/Makefile` gains `test-light-shard
   SHARD/SHARDS` — a round-robin partition of the single canonical
   `TEST_LIGHT_TARGETS` (proven: union of slices == full list, zero overlap).
   Root `Makefile` gains `tier1-shard-{1,2,3}` that partition the whole
   `tier1` prerequisite set (shard 1 = 4 GB self-compiles + caches kept on
   their own runner; shards 2/3 = light halves + the remaining phases).
2. **build-once kaic2 per variant.** `tier1.yml` gains a `build` job that
   bootstraps the C-only kaic2 once and uploads the dependency chain
   (kaic0/kaic1/kaic2 + stage1.c/bundle.kai/stage2.c — NOT the
   hundreds-of-MB fixture `.c` litter). tier0 + the three shards download it
   and run `tools/ci-touch-build.sh`, which touches the chain newer than all
   sources so `make` rebuilds nothing (validated: `make: 'kaic2' is up to
   date.`). Same pattern for tier1-native (`build-native` uploads the
   libLLVM kaic2 + `runtime_llvm.bc`).
3. **Aggregator gate keeps the Required name.** A `tier1` job `needs:` all
   three shards and is green iff all succeed — the Required check is still
   `tier1`, so branch protection needs no edit. Same for `tier1-native`.
4. **Native ratchet sharded by `$BACKEND_PARITY_DIRS`** (newly overridable;
   default unchanged). Two disjoint DIR subsets whose union is the full
   corpus (proven). Safe only while the parity baseline is empty (the
   new-gap check is per-fixture; the closed-gap suggestion compares the full
   baseline) — documented in the script.

## The mtime trap (the one real sharp edge)

After `download-artifact`, a naïve `touch kaic2` is not enough: the
checked-out `compiler/*.kai` sources are newer than the extracted binaries,
so `make` regenerates the whole 260 s chain. Fix: touch
`kaic0 → stage1.c → kaic1 → bundle.kai → stage2.c → kaic2` with the current
time (newer than every source), in dependency order. Validated against the
real trap (touched a source newer, confirmed make wanted a rebuild, ran the
script, confirmed `up to date`). The script self-checks with `make -q` and
fails loud if a rebuild would still fire — a silent 260 s rebuild would
defeat the artifact and read as "build-once works" when it doesn't.

Note: the root `kaic2` target is `.PHONY`, so it always recurses into
`make -C stage2 kaic2`; that recursion is a cheap no-op iff the stage2
mtimes are correct, which the touch guarantees.

## Coverage invariants (do not break)

- `tier1-shard-1 ∪ tier1-shard-2 ∪ tier1-shard-3` == the original `tier1`
  prerequisite set, exactly. Adding a phase to `tier1` means adding it to a
  shard, or the aggregator goes green while a phase never ran.
- `light(1/2) ∪ light(2/2)` == `TEST_LIGHT_TARGETS` (round-robin partition).
- native shard DIRS union == the full `test-backend-parity.sh` corpus.
- The aggregator gates on `result == success` for every shard (not
  `!= failure`), so `skipped`/`cancelled` shards fail the gate — a build-job
  failure cannot leak a green `tier1`.

## Before / after (expected, to confirm on the PR's own CI run)

| | Before (Required `tier1`) | After (Required `tier1` = aggregator) |
|---|---|---|
| Critical path | ~27 min (one job: bootstrap + full suite) | build (~4 min) → max(shard) (~8–10 min) ≈ **~13–14 min** |
| Bootstraps per PR | 3 (tier0, tier1, native each from scratch) | 2 (one C-only `build`, one `build-native`), reused by all consumers |

Numbers above are projected from the measured per-phase splits; the PR's own
CI run is the real confirmation (re-measure, do not cite projections as
fact). The residual floor is the `build` job (~4 min bootstrap, on the
critical path since the shards `needs:` it) plus the slowest shard; pushing
below ~13 min would need either a faster kaic2 cc (it is the bootstrap, not
a lever we touch here) or more/finer shards (diminishing returns vs runner
startup + artifact transfer overhead).

## Cost vs estimate

The analysis dominated the lane (measuring the real runner, disproving the
brief's redundancy + tier0-overlap leads, finding the build-once + shard
levers). The implementation is small (two Makefiles, two workflows, one
helper script). The surprise: the brief's headline redundancies
(`net_*`/`hello` duplicates) were already fixed — the actual lever was
structural (build-once + shard), not deletion.

## Follow-ups

- **Finer light sharding** (SHARDS=3+) if the slowest shard stays the
  critical path after this lands — trivial via the existing SHARD/SHARDS
  knob, bounded by runner-startup + artifact-transfer overhead.
- **build-once across workflows** — the C-only `build` artifact could also
  feed tier1-asan (it needs an ASAN-instrumented kaic2, a third variant, so
  not directly; left out).
- **tier0 off the gate entirely?** It duplicates demos + selfhost that the
  shards already cover except selfhost-determinism; could fold selfhost into
  a shard and drop tier0. Deferred — tier0 is off the critical path, so the
  win is compute, not wall-clock.
