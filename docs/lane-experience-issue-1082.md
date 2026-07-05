# Lane experience — issue #1082 (rb-tree harness: native column measurement)

## Scope as planned vs as shipped

**Planned (per the issue):** the `benchmarks/rb-tree/run.sh` table reported
`kaikai-native` at 3.9× C while `kaikai-c` sat at 1.7× C, yet the issue's
instruction-count measurement showed the two backends "at parity"
(1.00008×). The brief read this as a pure measurement artefact — cold cache
+ scheduler noise amplified by measuring each column's 5 runs back-to-back —
and asked for warm-up + interleaving so the table would stop lying.

**Shipped:** warm-up + interleaving + RUNS 5→7, plus honest notes in
`run.sh` and README. But the premise was half-wrong, and finding out why was
the whole lane.

## The real finding: the gap is real; the issue's repro measured native-vs-native

The issue's repro built *both* columns with `./bin/kai build`. Since the
Lane 1.5 flip (#851) `kai build` with no `--backend` is **native by
default** (documented at `bin/kai:729`). So the "1.00008× parity" was native
compared to native — it never touched the C backend. Verified, N=1M, all
fresh binaries, unique paths (stale-binary reuse burned two measurements
mid-lane — always use unique output paths when comparing):

| binary | instructions | vs C |
|---|---:|---:|
| C (hand-written) | 0.45G | 1.00× |
| kaikai `--backend=c` (`kaic2` → C → `cc -O2`) | 2.51G | 5.5× |
| kaikai `--backend=native` | 15.03G | 33× |

`run.sh` builds its `kaikai-c` column with `kaic2` → C → `cc -O2` (the real
C backend, 2.51G) and its `kaikai-native` column with `--backend=native`
(15.03G). The ~6× instruction gap and ~2.3× wall gap are a **real native
codegen gap**, not a harness artefact. Cross-check: the same emitted `.c`
compiled by `run.sh`'s `cc` line and by `bin/kai`'s `cc` line gives an
identical 2.51G, so the build path is not the culprit — the native code
generator is.

`run.sh` was reporting the truth. There was no artificial gap to close.

## What warm-up + interleaving actually fixed

The *run-to-run instability* was real even if the backend gap was not an
artefact. Measuring column A's 5 runs, then column B's, makes whichever
column is timed first pay cold cache / cold CPU-frequency while the rest run
warm — that is what produced the issue's one-off 0.39s kaikai-c reading that
would not reproduce. The fix: one discarded warm-up run per binary, then
interleave the timed rounds (one run per column per round) so noise spreads
evenly. Across 3 back-to-back `run.sh` invocations the columns are now
stable — kaikai-c 1.60–1.68×, native 3.80–3.88× — instead of swinging.

## Structural surprises

- **`declare -A` is bash 4+.** `/usr/bin/env bash` on this Mac resolves to
  `/bin/bash` = 3.2.57, which has no associative arrays. The first draft
  used `declare -A SAMPLES`; rewrote with parallel indexed arrays
  (`PATHS[]` + `SAMPLES[]` keyed by a fixed column order). Smoke-tested the
  index mapping under `/bin/bash` explicitly, with and without the
  native/Koka columns present.
- **The default-backend confusion is a doc trap.** The old README line
  "`kaikai-c` is the default C backend" was false since #851 and is exactly
  the mental model that produced the bad repro. Corrected in place.

## Fixtures / coverage

Bench-and-doc-only lane: touches `benchmarks/rb-tree/run.sh` and
`benchmarks/rb-tree/README.md`, nothing in `stage*/` or `stdlib/`. No
compiler tier is triggered. The "fixture" is the run-protocol change itself,
validated by 3 stable `run.sh` invocations pasted into the PR. The native
codegen gap (the real one) is tracked in the perf lanes (#1083).

## Follow-ups

- The 3.9× native-vs-C column is genuine native-codegen work, owned by the
  perf lanes (#1083), not this harness lane.
- README's callgrind instruction-count table still cites the retired
  llvm-text column at 100K; a future perf lane should re-measure the
  in-process native column under callgrind and replace it.
