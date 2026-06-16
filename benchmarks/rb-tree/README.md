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
benchmarks/rb-tree/run.sh     # wall-clock (median of 5) + peak RSS
benchmarks/rb-tree/run.sh 100000   # smaller N
```

`run.sh` emits the kaikai C, compiles all three with `cc -O2`, runs each
five times, and prints a `vs C` / `vs Koka` / `RSS` table. The Koka
column is skipped automatically if `koka` is not on `PATH`.

## Wall-clock vs instruction count — which to trust

**Wall-clock on a laptop is noisy** (cache misses + scheduler dominate;
runs vary ±10%). It is the number a *user* feels, so it is what `run.sh`
reports. But it is NOT what the perf lanes gate on.

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

**Wall-clock, Mac (Darwin arm64), `cc -O2`, N=1,000,000, median of 5
(`run.sh`):**

| column | wall (median) | vs C | vs Koka | RSS (MB) |
|---|---:|---:|---:|---:|
| C            | 0.26s | 1.00x | 1.04x | 47.4 |
| Koka         | 0.25s | 0.96x | 1.00x | 48.1 |
| kaikai-c     | 0.41s | 1.58x | 1.64x | 55.2 |
| **kaikai-llvm** | **0.79s** | **3.04x** | **3.16x** | **55.4** |

> **Historical (kaikai-llvm column).** The `kaikai-llvm` rows below recorded
> the now-retired llvm-text backend (`--emit=llvm` + clang -O2 +
> `runtime_llvm.c`), removed 2026-06-16. `run.sh` now measures a
> `kaikai-native` column instead — the in-process libLLVM backend
> (`--backend=native`, no `.ll` text, no clang) that replaced it. The rows
> here are kept as a point-in-time record; re-run `run.sh` for current
> native-vs-C numbers.

`kaikai-c` is the default C backend; the two columns share the front-end, so
the gap is pure code-generation.

**Instruction count, Docker callgrind, N=100,000 (the perf-lane truth):**

| column | instructions @ 100K | vs kaikai-c | vs Koka |
|---|---:|---:|---:|
| Koka         | 130.38M | 0.71x | 1.00x |
| kaikai-c     | 184.27M | 1.00x | 1.41x |
| **kaikai-llvm (separate `-c` compile, production)** | **897.15M** | **4.87x** | **6.88x** |
| kaikai-llvm (`llvm-link` merged module) | 534.87M | 2.90x | 4.10x |

The LLVM column at #747-close: i64-inline (Int variant slots ride raw
`i64`), the fast variant ctor path (`kai_variant_u_fast` + startup
payload-ctor / nullary seed), and reuse-in-place all ported. That took the
LLVM backend from **1,331.93M (7.23x C)** to **897.15M (4.87x C)** in the
production separate-compile path, and to **534.87M (2.90x C)** when the
shim TU is merged with the program IR (`llvm-link` / `-flto`). The residual
gap is the `kaix_*` shim call boundary (the LLVM backend keeps `%KaiValue`
opaque and routes every cell access through an external shim, where the C
backend has the whole runtime inlined in one TU) plus the structurally
heavier codegen of the hot `insert_loop` body. Closing it needs the
production link to merge the shim module (`-flto` / `llvm-link`) — a
build-chain lane, not a codegen change — and is tracked separately.

Recorded at **kaikai `8ea608c`** (Lane B i64-inline shipped, 2026-06-05).
History: the rb-tree started this perf arc at ~404M instructions
(~3.7x Koka); the i64-inline lever (kind-1 raw variant Int slots) was the
last big sound win, taking it 208M → 184M (1.60x → 1.41x).

The remaining gap to ~1.2x Koka is NOT in Int boxing (the field is fully
raw now) — it is in the zipper/descent machinery (`cctx_apply_linear`,
`is_red`, `check_unique`). Those levers were measured and refuted in the
2026-06-05 session; closing the gap needs a zipper redesign, not more
i64-inline.

> Numbers drift as the runtime evolves. Re-run `run.sh` and update the
> "Last recorded result" tables (and the commit hash) when you take a new
> measurement — do not trust a stale row.
