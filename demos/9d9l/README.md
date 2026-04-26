# 9d9l — kaikai

kaikai implementations of the challenges from
[lnds/9d9l](https://github.com/lnds/9d9l) ("9 desafíos en 9 lenguajes
de programación"), written in **target syntax** (post-m7e/m12.5/m12.6).
Most do not compile today; they are probes against the language we are
designing.

The original repo implements each challenge in 9 languages (Clojure,
Erlang, F#, Go, Haskell, Kotlin, Rust, Scala, Swift). This directory
adds a kaikai column.

## Challenges

| # | Challenge | Demo | Status |
|---|---|---|---|
| 1 | Toque y Fama (Mastermind digits) | `toquefama/` | needs `Random` effect (post-m14) |
| 2 | Weather (concurrent HTTP) | `weather/` | needs m8 (Spawn/nursery) + Net + JSON |
| 3 | Vectores (file processing) | `vectores/` | needs File + Clock + sort (m5-stdlib) |
| 4 | Huffman compression | `huffman/` | needs File + sum types + assoc lists |
| 5 | Blackjack | — | shares with `demos/blackjack/` (TBD upstream) |
| 6-9 | TBD | — | not specified upstream |

## Run

```sh
make -C demos/9d9l verify    # try all, print PASS/FAIL/DIFF table
make -C demos/9d9l <topic>
make -C demos/9d9l list
make -C demos/9d9l clean
```

Same conventions as `demos/Makefile` (target-syntax probes, never
aborts, kaic2 directly).
