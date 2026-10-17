# kaikai vs GHC

The same five programs in kaikai and Haskell, compiled and run on one machine.
`bench.sh <kai>` prints compile and run CPU time (user + sys, children included,
best of `N`, default 5) for `ghc -O2`, `ghc -O0`, `kai build` (level 2) and
`kai build` at level 1 (what `kai run` uses), and checks both binaries print the same.

```sh
benchmarks/ghc/bench.sh path/to/kai
```

Results per release live in `docs/benchmarks/ghc-<version>.md`.
