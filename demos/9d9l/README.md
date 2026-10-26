# 9d9l — kaikai

kaikai implementations of the challenges from
[lnds/9d9l](https://github.com/lnds/9d9l) ("9 desafíos en 9 lenguajes
de programación").

The original repo implements each challenge in 9 languages (Clojure,
Erlang, F#, Go, Haskell, Kotlin, Rust, Scala, Swift). This directory
adds a kaikai column.

## Challenges

| # | Challenge | Demo |
|---|---|---|
| 1 | Toque y Fama (Mastermind digits) | `demos/toquefama/` |
| 2 | Weather (concurrent HTTP + JSON) | `weather/` |
| 3 | Vectores (file processing) | `vectores/` |
| 4 | Huffman compression | `huffman/` |
| 5 | Blackjack | `demos/blackjack/` |

## Run

```sh
make -C demos/9d9l verify    # compile + run all, print PASS/FAIL/DIFF table
make -C demos/9d9l <topic>
make -C demos/9d9l list
make -C demos/9d9l clean
```

Same conventions as `demos/Makefile`: never aborts, uses `stage2/kaic2`
directly. `weather` needs network access.
