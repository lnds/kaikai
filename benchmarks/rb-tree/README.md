# rb-tree benchmark — kaikai vs Koka vs C

A red-black tree insert benchmark that pins kaikai's runtime against two
references: **Koka** (the Perceus/RC lineage kaikai's memory model comes
from) and a **hand-written C** floor. Same algorithm, same driver, three
languages — the only variable is the compiler/runtime.

## What it measures

- **Algorithm:** Okasaki-style red-black tree (the Lean4/Koka
  `rbtree-ck` benchmark), `value : int`. Insert with balance on the way
  up; no deletion.
- **Driver:** Numerical-Recipes `ranqd1` LCG keystream
  (`s' = (s*1664525 + 1013904223) mod 2^31-1`), N inserts, then print
  the final `size`. Identical keystream across all three columns, so the
  trees are bit-identical.
- **N:** 1,000,000 by default (override: `./run.sh 100000`).

## The three sources

| Column | File | Notes |
|---|---|---|
| kaikai | `../../examples/perceus/rb_tree_bench.kai` | compiled by `stage2/kaic2` → C → `cc -O2` |
| C | `../../examples/perceus/rb_tree_bench_c.c` | hand-written RB, fixed-size node per insert |
| Koka | `rbbench.kk` | `rbtree-ck.kk` verbatim, `value:bool→int` + the LCG driver |

The kaikai and C sources live in `examples/perceus/` (they double as
Perceus fixtures); only the Koka column lives here, because Koka is an
external reference not otherwise in the tree.

## How to run

```sh
make -C stage2 kaic2          # build the kaikai compiler first
benchmarks/rb-tree/run.sh     # wall-clock (median of 7) + peak RSS
benchmarks/rb-tree/run.sh 100000   # smaller N
```

`run.sh` emits the kaikai C, compiles all three with `cc -O2`, gives each
binary one discarded warm-up run, then times seven **interleaved** rounds
(one run per column per round) and prints the median in a
`vs C` / `vs Koka` / `RSS` table. Warm-up + interleaving keeps run-to-run
noise (cold cache, CPU-frequency ramp, scheduler) from landing
systematically on whichever column is measured first, so the wall figures
are stable across re-runs. The Koka column is skipped automatically if
`koka` is not on `PATH`.

## Wall-clock vs instruction count — which to trust

**Wall-clock on a laptop is noisy** (cache misses + scheduler dominate;
runs vary ±10%). It is the number a *user* feels, so it is what `run.sh`
reports. But it is NOT what the perf lanes gate on.

`kaikai-c` and `kaikai-native` share the front-end but NOT the code
generator, and on this bench they do NOT do the same work: the C backend
retires ~2.49G instructions (N=1M) against native's ~6.27G — a ~2.5×
native-vs-C codegen gap (measured with `/usr/bin/time -l` on Darwin arm64,
kaikai `72a04181`). So the native column's larger wall is the codegen gap
surfacing, not measurement noise; closing the remainder is native-backend
perf work (tracked in the perf lanes), not a harness bug. When you want the
exact, host-independent number, read the instruction count below rather than
the wall.

> A `./bin/kai build` with no `--backend` is **native** by default, so
> timing it against `--backend=native` compares native to itself (identical
> counts) — not native to the C backend. `run.sh`'s kaikai-c column is the
> C backend (`kaic2` → C → `cc -O2`); that is the honest native-vs-C
> comparison.

For an **exact, host-independent** measurement use **callgrind in
Docker** — deterministic instruction count, the truth the perf work is
tuned against:

```sh
# emit the bench C, drop N to 100K (callgrind is ~50x slower)
stage2/kaic2 --path stdlib --path examples/perceus \
  examples/perceus/rb_tree_bench.kai > /tmp/rb.c
sed -i '' 's/kair_n = 1000000LL/kair_n = 100000LL/' /tmp/rb.c
docker run --rm -v /tmp:/w -w /w ubuntu:24.04 bash -c '
  apt-get update -qq && apt-get install -y -qq clang valgrind
  clang -O2 -g -I <runtime-inc> -o rb rb.c -lm
  valgrind --tool=callgrind --callgrind-out-file=cg.out ./rb
  callgrind_annotate --threshold=1 cg.out | grep "PROGRAM TOTALS"'
```

The Koka reference for the instruction-count comparison is **130.38M @
100K** (Koka 3.2.3, same Docker image, amalgamated `kklib` + `std_core`).
Wall-clock and instruction-count ratios differ — wall is always worse
for kaikai because cache/branch effects that callgrind ignores hit the
RC traffic. Quote whichever you mean, and say which.

## Last recorded result

**Wall-clock, Mac (Darwin arm64), `cc -O2`, N=1,000,000, median of 7
(`run.sh`, warm-up + interleaved):**

| column | wall (median) | vs C | vs Koka | RSS (MB) |
|---|---:|---:|---:|---:|
| C            | 0.25s | 1.00x | 1.00x | 47.4 |
| Koka         | 0.25s | 1.00x | 1.00x | 48.1 |
| kaikai-c     | 0.40s | 1.60x | 1.60x | 55.2 |
| **kaikai-native** | **0.48s** | **1.92x** | **1.92x** | **55.2** |

> **Historical (kaikai-llvm column).** An earlier table here recorded the
> now-retired llvm-text backend (`--emit=llvm` + clang -O2 +
> `runtime_llvm.c`), removed 2026-06-16. `run.sh` measures the
> `kaikai-native` column above instead — the in-process libLLVM backend
> (`--backend=native`, no `.ll` text, no clang) that replaced it.

`kaikai-native` is the *default* backend (`kai build` with no `--backend`
is native since the Lane 1.5 flip); `kaikai-c` is the portable C-text
backend (`--backend=c`). Both share the front-end, so the ~1.2x wall / ~2.5x
instruction gap between them is pure code-generation, not a measurement
artefact — closing the remainder is native-backend perf work.

**Instructions retired, `/usr/bin/time -l`, Darwin arm64, N=1,000,000
(median of 3):**

| column | instructions @ 1M | vs kaikai-c |
|---|---:|---:|
| kaikai-c     | ~2.49G | 1.00x |
| **kaikai-native** | **~6.27G** | **~2.51x** |

The native-vs-C instruction gap is ~2.5×, down from 2.73× before the
native perf arc (#1088 target-features, #1092 fast-entry, #1100 i64-inline
construction, #1102 KProjBorrow pointer read). The residual is the `kaix_*`
shim call boundary (the native backend keeps `%KaiValue` opaque and routes
cell access through an external shim, where the C backend inlines the whole
runtime in one TU) plus the deferred kind-1 Int raw binder read (the lever
to ~1.08× pure-lookup, reverted in #1102 because it SIGSEGVs the native
self-host gate — #709 phantom-box; needs border-reboxing to ship). Tracked
in the Koka-parity issue.

> **Host-independent number (Docker callgrind) not re-measured here.** The
> earlier callgrind table measured the now-retired llvm-**text** backend
> (`--emit=llvm`, removed 2026-06-16), which emitted a `.ll` file callgrind
> could compile with `-g`. The current in-process native backend emits an
> object directly, so the old llvm-text callgrind recipe no longer applies
> to it. The kaikai-c column is still callgrind-able; Koka reference was
> 130.38M @ 100K (Koka 3.2.3, same Docker image). The `/usr/bin/time -l`
> counts above are host-dependent but reproducible on this machine.

Recorded at **kaikai `72a04181`** (v0.99.0, native perf arc closed
2026-07-06).

The remaining gap to Koka is shared by BOTH backends (kaikai-c 1.60× Koka
wall, kaikai-native 1.92×) — a front-end / language cost (Perceus RC,
zipper/descent machinery `cctx_apply_linear` / `is_red` / `check_unique`,
layout), NOT code generation. That is the Koka-parity work, tracked
separately.

> Numbers drift as the runtime evolves. Re-run `run.sh` and update the
> "Last recorded result" tables (and the commit hash) when you take a new
> measurement — do not trust a stale row.
