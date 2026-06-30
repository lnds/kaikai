# CI time analysis — where the PR wall-clock goes (2026-06-22)

All numbers below are **measured**, never dry-run. CI durations come from the
GitHub Actions REST API for real `main` runs on `ubuntu-latest`; local splits
come from `/usr/bin/time -p` on a 14-core / 24 GB macOS host with no
`llvm-config` in PATH (so the local build is **C-only**, matching the tier1
runner exactly). Every kaikai binary was wrapped in `KAI_MAX_HEAP` +
`timeout` per host-safety.

## 1. The honest baseline (CI, ubuntu-latest, GitHub API)

Per-PR every workflow fires at once and runs on its own runner, so the
**critical path is the slowest job, not the sum**. Measured over 6 recent
green `main` runs (no outliers; tier1 spread 25m40s–28m00s):

| Job | Workflow | Duration | Required? | On PR critical path |
|---|---|---|---|---|
| **tier1** | tier1.yml | **27m 26s** | **yes (the gate)** | **yes** |
| tier1-native | tier1-native.yml | 20m 29s | no | no |
| tier0 | tier1.yml | 12m 35s | no | no (parallel runner) |
| tier1-asan | tier1-asan.yml | 9m 21s | no (path-gated) | no |

The repo is **public**, so `ubuntu-latest` is the 4-vCPU / 16-GB standard
runner. No OOM/swap-kill in the logs, but 4 cores cap the in-job `-j`
parallelism.

`runs-on` migrated self-hosted m2 → github-hosted ubuntu in commit
`54afa108`; the older `docs/lane-experience-tier1-perf.md` describes the
**m2 world** (its 18m tier1 number is stale — the ubuntu number is 27m).

## 2. tier1 critical-path breakdown (from CI log timestamps)

Reconstructed from the `tier1` step's per-line timestamps in a real run
(job total 1641 s ≈ 27.4 min):

| Phase | CI time | Notes |
|---|---|---|
| Setup + checkout + apt clang | ~25 s | |
| stage0 + stage1 build (`-O0`) | ~20 s | kaic0 0.2 s, kaic1 ~seconds |
| regen `build/stage2.c` (`kaic1` over the 4 MB bundle) | **~45 s** | 16 s locally; ubuntu ~2.8× slower |
| **`cc` kaic2 at `-O2`** | **~197 s (3.3 m)** | 55 s locally; ubuntu ~3.6× slower |
| 5 "costly" self-compiles (`-j5`, ~4 GB each) | ~128 s (2.1 m) | `test-tokens/ast/types/env/check` over `main.kai` |
| **~1100 "light" fixtures + caches** | **~900 s (15 m)** | the dominant pool |
| demos-no-regression | ~78 s | |
| fmt fixtures + fmt-selfhost | ~60 s | |
| bench + check + negative + stdlib-modules + info + audits | ~150 s | |
| **Total** | **~1641 s** | |

**The grout (≈15 min) is the ~1100-fixture light pool.** `make test`
runs **1114 fixtures** (counted from OK/PASS log lines); the heaviest
families are `stdlib` (158), `sugars` (144), `effects` (114),
`protocols` (46).

### Per-fixture cost split (local, 50 effects fixtures)

| Step | 50 fixtures | per fixture |
|---|---|---|
| kaic2 (kai → C) | 3.33 s | ~67 ms |
| `cc` (C → bin) at `-O2` | 1.54 s | ~31 ms |
| `cc` at `-O0` | 1.51 s | identical — fixture `.c` is small, opt level irrelevant |

Each light fixture is **~100 ms of CPU-bound work** (⅔ kaic2, ⅓ cc), tiny
RSS. 1100 × 100 ms ≈ 110 s single-thread of pure compile work; CI spends
~15 min on it because of 4 cores + per-fixture shell/make overhead + a
slower ubuntu `cc`. **CPU-bound + independent ⇒ scales near-linearly across
separate runners** (the tier1-perf retro already measured 7–9× on these
loops under parallelism).

## 3. Question: must kaic2 build at `-O2`? — YES, measured

`stage2/Makefile` defaults `CFLAGS … -O2`; commit `668dd476`
(*"perf(build): compile kaic2 at -O2 (2.4x faster self-compile)"*) flipped
it from `-O0` deliberately. The trade is: pay a slower `cc` **once** so kaic2
**runs** much faster on every fixture.

| | `cc` kaic2 (build, local) | run over 405 real fixtures |
|---|---|---|
| `-O0` | 3.5 s | **178.7 s** |
| `-O1` | 45.0 s | — |
| **`-O2`** (default) | 55.6 s | **69.0 s** |

`-O2` is **2.59× faster at runtime**, output byte-identical (`diff`
confirmed). Net per job: `-O2` ≈ 55 s build + 69 s suite = ~124 s;
`-O0` ≈ 3.5 s + 179 s = ~182 s. **Dropping to `-O0` makes a job ~58 s
*slower*.** `-O2` stays — it is not a lever.

(The fixture-stage `cc -O0` vs `-O2` is a wash — fixture `.c` is small — so
the test-stage `cc` opt level is not a lever either.)

## 4. Question: redundant tests / fixtures / demos? — essentially NONE deletable

The brief's candidates do not hold in the current checkout:

- **`net_tcp_localhost` / `net_udp_localhost`**: present **only** in
  `examples/effects/`, **not** in `demos/`. The duplication was already
  resolved; nothing to remove.
- **The three `hello` fixtures** (`phase4/`, `minimal/`, `quickstart/01_`)
  are all **different** (md5 distinct: 246 / 42 / 39 bytes). Not duplicates.

Systematic body-hash sweep (strip comments + blanks, hash, find cross-file
collisions) found 20 same-body groups. Every one is **legitimate**
redundancy:

- `examples/fmt/*.input.kai ≡ *.expected.kai` — the **idempotence** test
  (already-formatted input must round-trip unchanged). Not redundant.
- `pub_enforced_*/lib.kai`, `unstable/pub_fn_*/`, `negative/.../priv_field_*`
  — shared lib, **opposite** contract in the caller / opt-in / `.err.expected`.
- `modules-qualified/basic/arith ≡ unknown_export/arith` — same module,
  positive vs negative case.
- `library_mode/type_at_basic ≡ lsp/hover_basic` — same source, different
  invocation **mode** (library-mode vs LSP hover).
- `unions/match_variant_basic ≡ simple_union`, `sugars/m7b_16 ≡ m7b_4`,
  `sequence/pipe_map_basic ≡ sugars/map_pipe_basic` — **identical code body,
  different issue/phase** documented in comments; each is the regression
  anchor for a distinct issue (#187 phase 2 vs 3; m7b #16 vs #4; #201 vs
  m7b #9). Deleting either drops an issue's anchor.

A fixture's `.kai` body can be byte-identical while what it **exercises**
differs (the caller, the flag, the expected diagnostic, the invocation mode).
This matches the #626 retro warning that "redundant-looking" fixtures were
live positives. **Verdict: no fixtures are safely deletable; the pool is
already clean. Fixture deletion is not a lever.**

## 5. Question: overlap between phases?

- **`demos-no-regression` runs in BOTH `tier0` and `tier1`** (~78 s each),
  and **each job re-bootstraps kaic2 from scratch** (~260 s: regen 45 s +
  `cc -O2` 197 s + stage0/1 20 s). That is the largest overlap.
- **But `tier0` is not on the PR critical path** — it runs in parallel on
  its own runner and finishes at 12.5 min, well under tier1's 27 min.
  Removing tier0's duplicate work saves **compute**, not PR wall-clock.
- The lever that *does* move wall-clock is **sharing one pre-built kaic2**
  across jobs so no job pays the ~260 s bootstrap twice, then **sharding the
  tier1 light pool** so the 15-min grout runs on N runners at once.
- tier1-asan re-runs demos under ASAN — that is **distinct** memory-shape
  coverage; keep it.

## 6. Question (the big one): C backend vs native default

Today's split:

- **tier1 (Required)** compiles the whole suite with the **C backend**. But
  `make test` is mostly **front-end** verification — `--ast`, `--types`,
  `--diags-json`, `--library-mode`, negative-space **rejections**, fmt
  idempotence — i.e. checks that are backend-agnostic or use C only as the
  emission path. Only a subset is compile→run→diff of program output.
- **tier1-native (not Required)** already builds **every** entry-point
  fixture with **both** backends (`KAI_BACKEND=native` target vs
  `KAI_BACKEND=c` oracle) and diffs stdout + exit code, over ~620 fixtures
  across `examples/{effects,actors,spawn,perceus,refinements,llvm,packages,
  minimal,quickstart,stdlib,attributes,unstable}` + `demos`.

So **the C backend is already run as the parity oracle inside tier1-native**,
and tier1's `make test` already exercises the C output path. The two are
**complementary, not redundant**: tier1 covers the front-end + reject space
that parity never touches; tier1-native covers backend output equivalence
that `make test` never touches.

**Coverage finding:** the **default backend (native) has LESS coverage than
the oracle**. native is exercised only by tier1-native, and only as a
*parity diff vs C* — never by the front-end battery (AST/typer/diags/reject)
that tier1 runs exclusively against the C path. This is the inverse of what
the default deserves.

**Decision (coverage-correct, time-minimal):**

1. **Keep C as the oracle + bootstrap path** — it validates native and is the
   only `cc`-portable backend; never weaken it.
2. **Do NOT add a second from-scratch native run of the front-end battery.**
   The front-end (parse/resolve/infer/reject) is **backend-independent by
   construction** — the same KIR feeds both lowerers — so running the typer
   battery twice would be pure redundancy, not coverage. The right native
   coverage is **output parity**, which tier1-native already provides.
3. **Remove the redundant double-bootstrap, not a backend run.** The genuine
   waste is that tier1, tier0, and tier1-native each build a kaic2 from
   scratch. C-only kaic2 is shared by tier0 + tier1; the native-capable
   (libLLVM) and ASAN kaic2 are genuinely distinct variants and must stay
   separate builds — but each should be built **once per variant** and
   reused across that variant's steps (tier1-native already does this
   internally; tier1+tier0 do not).

There is **no redundant double-running of the same fixtures across C and
native** to remove: tier1 = C front-end battery, tier1-native = native-vs-C
output parity. They do not overlap on the same check. The only redundancy is
the **build**, addressed by build-once-per-variant.

### tier1-native per-step breakdown (CI, 1229 s ≈ 20.5 min)

| Step | CI time |
|---|---|
| Install toolchain (clang + llvm-dev) | ~10 s |
| Assert P2 bitcode (**includes the libLLVM kaic2 build**) | ~237 s |
| Smoke: every `examples/native` fixture (emit→link→run→diff) | ~242 s |
| **Ratchet: full corpus native-vs-C** | **~733 s** |
| #860 cons-RC ledger gate | ~1 s |

The ratchet (~620 fixtures × 2 backends) is the dominant cost and the shard
target. The libLLVM kaic2 build (~237 s, inside the Assert step) is paid once
per shard today; build-once shares it. Sharding the ratchet across two
disjoint DIR subsets (~289 vs ~257 entry points) plus moving the smoke onto
the lighter shard brings each native shard under ~14 min.

## 7. Levers, in ROI order

1. **Shard the tier1 light pool across separate runners** (CPU-bound,
   independent ⇒ near-linear; `-j` in one runner is bandwidth-bound at 1.24×
   per the tier1-perf retro — separate runners each get their own memory
   bus). This is THE 27m→15m lever. Aggregator job keeps the Required check
   named `tier1` so branch protection is untouched.
2. **build-once kaic2 per variant** — a build job emits the C-only kaic2 (+
   stage0/1 + `build/` artifacts) once; tier0 and every tier1 shard consume
   it via `needs:` + artifact download + an mtime touch (binaries touched
   **newer than all sources**, in dependency order, so `make` treats the
   chain as up-to-date and rebuilds nothing — validated: `make: 'kaic2' is
   up to date.`).
3. **Apply the same build-once + shard to tier1-native** so the default
   backend's gate also closes < 15 min.
4. Fixture deletion: **not available** (§4). `-O0`: **counterproductive**
   (§3). In-job `-j`: **bandwidth-capped** (retro).

## 8. The mtime trap (for the artifact handoff)

`build/stage2.c` depends on `build/bundle.kai` + `kaic1`; `bundle.kai`
depends on the `compiler/*.kai` sources. After downloading the artifact, a
naïve `touch kaic2` is **not enough** — if any `compiler/*.kai` source is
newer than `build/bundle.kai`, make regenerates the whole chain (the 45 s
regen + 197 s `cc`). The fix that works (validated locally with a real
`make` run, not dry-run): touch the build chain
`kaic0 → kaic1 → bundle.kai → stage2.c → kaic2` with the **current time**,
which is newer than every checked-out source, in dependency order. `make`
then reports every target up-to-date and the test job pays **0 s** of
rebuild instead of ~260 s.

Note: the root `kaic2` target is `.PHONY`, so it always recurses into
`make -C stage2 kaic2`; that recursion is cheap (a no-op) **iff** the stage2
mtimes are correct — the touch above makes it so.

## 9. Local shard validation (proof the partition is green + balanced)

All three shards run on a pre-built kaic2 (build-once simulated via the
touch), C-only, `KAI_MAX_HEAP=12g`:

| Shard | Contents | Local wall | Result |
|---|---|---|---|
| tier1-shard-1 | 5 costly self-compiles + heap-limit + user/core caches | 67 s | OK |
| tier1-shard-2 | light slice 1/2 + demos-no-regression | 166 s | OK |
| tier1-shard-3 | light slice 2/2 + fmt/bench/check/negative/stdlib-modules/packages/audits/info/doc | 263 s | OK |

Union == the original `tier1` (every phase ran, none twice). The slowest
shard (shard-3) sets the critical path; it carries the heavier non-light
tail. A finer split (SHARDS=3, or moving the non-light tail off shard-3) is
the obvious next tuning if CI shows shard-3 still dominating — left as a
follow-up since the knob already exists and the gate is green and under
budget as-is. The `cc` opt level was confirmed a wash for fixtures; none of
the shards rebuild kaic2 (verified: `make[2]: 'kaic2' is up to date.` in
each shard's log).
