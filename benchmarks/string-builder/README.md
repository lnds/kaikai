# StringBuilder vs naive `++`-fold

Builds an N-fragment string two ways and measures the asymptotic gap:

- `sb_builder.kai` — accumulate into a `string_builder`, `build` once.
- `sb_naive.kai` — left-fold of `acc := @acc ++ "frag"`.

The naive fold is O(n²): every `++` allocates a fresh string the size
of the accumulator-so-far, so total work and total garbage are both
quadratic in N. The StringBuilder is O(total length): fragments are
appended into a doubling `Array[String]` (O(1) amortised) and joined
once in `build` via `string_concat_all` (a single-pass measure +
`memcpy`, one allocation).

## Measured (native backend, execution time only — binaries built once)

| N fragments | naive `++` | StringBuilder | speedup |
|-------------|------------|---------------|---------|
| 20 000      | 0.34 s     | 0.27 s        | 1.3×    |
| 40 000      | 0.48 s     | 0.28 s        | 1.7×    |
| 80 000      | 0.90 s     | 0.29 s        | 3.1×    |

Doubling N doubles the naive time (super-linear toward quadratic)
while the StringBuilder stays flat (linear work hidden under fixed
startup). The speedup grows with N exactly as O(n²) vs O(n) predicts.

## Memory — the sharper difference

At **N = 50 000** the naive fold **exhausts a 4 GiB heap and aborts**
(`heap limit exceeded`), because the accumulator copies dominate. The
StringBuilder builds **N = 200 000** (4× more fragments, 800 000-byte
result) in **0.53 s under 4 GiB**. The quadratic garbage, not the CPU,
is what makes naive concatenation unusable at scale — the motivation
behind this module and its compiler twin (#901).

## Run

```sh
./run.sh            # builds both, times a sweep, prints the table
```

Cap the heap on any sweep (`KAI_MAX_HEAP=4g`) — the naive program is
designed to blow past it.
