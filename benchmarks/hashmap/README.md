# HashMap (mutable) vs Map (AVL) benchmark

Issue #374 asks the lane to compare the new `HashMap[k, v]` against the
AVL-backed `Map[k, v]` across N = 100, 1K, 10K, 100K and document the
crossover. This records the measurement and the conclusion: **the
mutable HashMap is faster than the AVL Map at every measurable scale**,
roughly 2-3×, as a default associative collection should be.

## What it measures

`hashmap_vs_map.kai` is a `main` + `Clock` timing program (not a
`kai bench` fixture — see the note below). It times:

- **build** — N `put`s from empty. HashMap mutates one bucket array in
  place and resizes on load factor; Map threads a fresh persistent
  value per `put`.
- **lookup** — N `get`s over a pre-built map.
- **lookup-hammer** — 1,000,000 `get`s cycling over N keys on a
  pre-built map, so the pure walk cost dominates (build excluded).

Keys and values are `Int`. Equal checksums in every row confirm both
structures hold identical contents.

> **Why not `kai bench`?** The `kai bench` harness currently
> **segfaults on any block body that carries the `Mutable` effect** —
> even a trivial `bench "x" { Mutable.ref_make(0) }` crashes it. That
> is a pre-existing bench/effect-runtime bug, unrelated to this
> carrier (HashMap is mutable, so every op carries `/ Mutable`).
> Reported in `docs/lane-experience-issue-374.md`. The monotonic clock
> path works fine under `Mutable`, so the benchmark times with it.

## Run it

```sh
bin/kai run benchmarks/hashmap/hashmap_vs_map.kai
```

Numbers vary per run and machine. Sample from an Apple-silicon laptop:

```
N=100      | map build 0ms  lookup 0ms  | hashmap build 0ms  lookup 0ms
N=1000     | map build 0ms  lookup 0ms  | hashmap build 0ms  lookup 0ms
N=10000    | map build 5ms  lookup 1ms  | hashmap build 1ms  lookup 0ms
N=100000   | map build 65ms lookup 16ms | hashmap build 22ms lookup 5ms
lookup-hammer N=10000 reps=1000000 | map 103ms | hashmap 53ms
```

## Conclusion — HashMap wins; there is no crossover to wait for

| Workload                  | Map (AVL) | HashMap (mutable) | speedup |
|---------------------------|-----------|-------------------|---------|
| build N=100,000           |    65 ms  |       22 ms       |  ~3.0×  |
| lookup N=100,000          |    16 ms  |        5 ms       |  ~3.2×  |
| 1M lookups over N=10,000  |   103 ms  |       53 ms       |  ~1.9×  |

At N ≤ 1,000 both are sub-millisecond and the choice is dominated by
semantics, not speed. From N = 10,000 up, the HashMap is clearly ahead
on both build and lookup, and the gap holds (does not invert) at
100,000.

This is the expected shape for a mutable hash table: `put` is one hash
+ one chain splice into a mutated array; `get` is one hash + one short
chain scan; resize amortises to O(1) per insert. The AVL `Map` pays an
`O(log n)` rebalanced tree walk per op and rebuilds the spine on every
persistent `put`.

### How this lane got here

The first cut of #374 shipped a **pure persistent HAMT** (the data
structure the issue body specified). Measured, it was ~1.6-2.3×
*slower* than the AVL `Map` it was meant to replace — a pure trie pays
allocation + structure-rebuild on every `put` and a list-indexed walk
on every level of every `get`. A HashMap slower than the tree defeats
its purpose. The carrier was redesigned to a **mutable hash table
behind the `Mutable` effect**, which is what these numbers measure.
Full rationale in `docs/lane-experience-issue-374.md`.

## Guidance

- Use **HashMap** as the default associative collection: fastest
  insert/lookup, keys without a total order (sum types with
  `impl Hash`). It is mutable — `put`/`remove` carry `/ Mutable`.
- Use **Map** (AVL) when you need a pure/persistent value (structural
  sharing, an immutable snapshot) or ordered iteration by key.
