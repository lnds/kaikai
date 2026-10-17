# kaikai 0.162.0 vs GHC 9.14.1

`benchmarks/ghc/bench.sh` on the released `kai` (darwin-arm64 tarball, native
backend). Apple M4 Pro, macOS 26. CPU seconds (user + sys, children included),
best of 5. The machine was loaded (load average ~12 on 14 cores), so read the
ratios, not the absolutes.

| program | ghc -O2 compile | kai build compile | ghc -O0 compile | ghc -O2 run | kai build run |
|---|---:|---:|---:|---:|---:|
| hello | 0.738 | 0.227 | 0.602 | 0.006 | 0.004 |
| fib 35 | 0.504 | 0.237 | 0.429 | 0.041 | 0.024 |
| lists (3 M: map, filter, sum) | 0.554 | 0.331 | 0.563 | 0.007 | 0.255 |
| strings (join of 300 k) | 0.589 | 0.266 | 0.468 | 0.018 | 0.086 |
| rb-tree (1 M inserts) | 0.678 | 0.617 | 0.558 | 1.647 | 0.706 |

- Compile: kaikai is 1.1–3.3× faster than `ghc -O2` and faster than `ghc -O0` on every program.
- Run: kaikai wins on fib (1.7×) and the red-black tree (2.3×). It loses on
  `lists` (36×) and `strings` (4.8×): GHC fuses `map`/`filter`/`sum` and never
  builds the intermediate lists, while kaikai materialises each stage.
- The level-1 column of `bench.sh` is left out: on 0.162.0 the native shared
  core object existed only at level 2, so level-1 builds recompiled the core.
