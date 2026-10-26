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
make kaic2                    # build the kaikai compiler first
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
reports.

`kaikai-c` and `kaikai-native` share the front-end but NOT the code
generator, and on this bench they do not do the same work: the native
backend retires more instructions than the C backend (see the table
below). When you want an exact number, read the instruction count rather
than the wall.

> A `./bin/kai build` with no `--backend` is **native** by default, so
> timing it against `--backend=native` compares native to itself (identical
> counts) — not native to the C backend. `run.sh`'s kaikai-c column is the
> C backend (`kaic2` → C → `cc -O2`); that is the native-vs-C comparison.

For a **host-independent** measurement of the C column use **callgrind in
Docker** — a deterministic instruction count:

```sh
# emit the bench C, drop N to 100K (callgrind is ~50x slower)
stage2/kaic2 --path stdlib --path examples/perceus \
  examples/perceus/rb_tree_bench.kai > /tmp/rb.c
sed -i '' 's/kair_n_0 = 1000000LL/kair_n_0 = 100000LL/' /tmp/rb.c
docker run --rm -v /tmp:/w -w /w ubuntu:24.04 bash -c '
  apt-get update -qq && apt-get install -y -qq clang valgrind
  clang -O2 -g -I <runtime-inc> -o rb rb.c -lm
  valgrind --tool=callgrind --callgrind-out-file=cg.out ./rb
  callgrind_annotate --threshold=1 cg.out | grep "PROGRAM TOTALS"'
```

The Koka reference for that comparison is **130.38M @ 100K** (Koka 3.2.3,
same Docker image, amalgamated `kklib` + `std_core`). The native backend
emits an object in-process, so this recipe does not apply to it.
Wall-clock and instruction-count ratios differ — cache and branch effects
that callgrind ignores hit the RC traffic. Quote whichever you mean, and
say which.

## Last recorded result

**Wall-clock, Mac (Darwin arm64), `cc -O2`, N=1,000,000, median of 7
(`run.sh`, warm-up + interleaved), kaikai 0.178.0:**

| column | wall (median) | vs C | vs Koka | RSS (MB) |
|---|---:|---:|---:|---:|
| C            | 0.33s | 1.00x | 0.97x | 47.4 |
| Koka         | 0.34s | 1.03x | 1.00x | 48.1 |
| kaikai-c     | 0.49s | 1.48x | 1.44x | 49.6 |
| **kaikai-native** | **0.50s** | **1.52x** | **1.47x** | **49.9** |

`kaikai-native` is the *default* backend (`kai build` with no
`--backend`); `kaikai-c` is the portable C-text backend (`--backend=c`).

**Instructions retired, `/usr/bin/time -l`, Darwin arm64, N=1,000,000:**

| column | instructions @ 1M | vs kaikai-c |
|---|---:|---:|
| kaikai-c     | ~2.53G | 1.00x |
| **kaikai-native** | **~3.56G** | **~1.40x** |

The gap to Koka is shared by both backends — a front-end / language cost
(Perceus RC, the zipper descent, layout), not code generation.

> Numbers drift as the runtime evolves. Re-run `run.sh` and update the
> "Last recorded result" tables (and the version) when you take a new
> measurement — do not trust a stale row.
