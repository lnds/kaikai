# HashMap (mutable) vs Map (AVL) benchmark

Compares the mutable `HashMap[k, v]` against the AVL-backed `Map[k, v]`
across N = 100, 1K, 10K, 100K. The conclusion: **the mutable HashMap is
faster than the AVL Map at every measurable scale**, roughly 2-3×, as a
default associative collection should be.

## What it measures

`hashmap_vs_map.kai` is a `main` that times with `Clock.monotonic_now()`.
It times:

- **build** — N `put`s from empty. HashMap mutates one bucket array in
  place and resizes on load factor; Map threads a fresh persistent
  value per `put`.
- **lookup** — N `get`s over a pre-built map.
- **lookup-hammer** — 1,000,000 `get`s cycling over N keys on a
  pre-built map, so the pure walk cost dominates (build excluded).

Keys and values are `Int`. Equal checksums in every row confirm both
structures hold identical contents.

## Run it

```sh
bin/kai run benchmarks/hashmap/hashmap_vs_map.kai
```

Numbers vary per run and machine. Sample from an Apple-silicon laptop:

```text
N=100  | map build 0ms lookup 0ms  | hashmap build 0ms lookup 0ms  (checksums 9900/9900)
N=1000  | map build 0ms lookup 0ms  | hashmap build 0ms lookup 0ms  (checksums 999000/999000)
N=10000  | map build 4ms lookup 0ms  | hashmap build 2ms lookup 0ms  (checksums 99990000/99990000)
N=100000  | map build 53ms lookup 12ms  | hashmap build 22ms lookup 4ms  (checksums 9999900000/9999900000)
lookup-hammer N=10000 reps=1000000  | map 96ms  | hashmap 42ms  (checksums 9999000000/9999000000)
```

## Conclusion — HashMap wins; there is no crossover to wait for

| Workload                  | Map (AVL) | HashMap (mutable) | speedup |
|---------------------------|-----------|-------------------|---------|
| build N=100,000           |    53 ms  |       22 ms       |  ~2.4×  |
| lookup N=100,000          |    12 ms  |        4 ms       |  ~3.0×  |
| 1M lookups over N=10,000  |    96 ms  |       42 ms       |  ~2.3×  |

At N ≤ 1,000 both are sub-millisecond and the choice is dominated by
semantics, not speed. From N = 10,000 up, the HashMap is clearly ahead
on both build and lookup, and the gap holds (does not invert) at
100,000.

This is the expected shape for a mutable hash table: `put` is one hash
+ one chain splice into a mutated array; `get` is one hash + one short
chain scan; resize amortises to O(1) per insert. The AVL `Map` pays an
`O(log n)` rebalanced tree walk per op and rebuilds the spine on every
persistent `put`. A pure persistent trie was measured slower than the
AVL `Map`, which is why `HashMap` is a mutable table behind the
`Mutable` effect.

## Guidance

- Use **HashMap** as the default associative collection: fastest
  insert/lookup, keys without a total order (sum types with
  `impl Hash`). It is mutable — `put`/`remove` carry `/ Mutable`.
- Use **Map** (AVL) when you need a pure/persistent value (structural
  sharing, an immutable snapshot) or ordered iteration by key.
