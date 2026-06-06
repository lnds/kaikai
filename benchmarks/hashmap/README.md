# HashMap (HAMT) vs Map (AVL) benchmark

Issue #374 asks the lane to compare the new HAMT-backed `HashMap[k, v]`
against the AVL-backed `Map[k, v]` across N = 100, 1K, 10K, 100K and
**document the crossover**. This file records the measurement and its
(honest, somewhat counter-intuitive) conclusion.

## What it measures

`hashmap_vs_map.kai` defines `bench` blocks for two end-to-end
workloads, identical between the two structures so the ratio is fair:

- **build** — N `put`s from empty. Allocation + spine-rebuild cost.
- **roundtrip** — N `put`s then N `get`s. The populate-and-query
  workload a registry / dedup table actually runs.

`kai bench` has no setup hook (the whole block body is timed), so a
pure "lookups over a pre-built map" measurement is not expressible —
hence the combined `roundtrip` workload, where build dominates but the
lookup tail is included for both.

Keys are `Int` (Map's `<` and HashMap's `hash` both dispatch on the
stdlib primitive impls); values are `Int`.

## Run it

```sh
bin/kai bench benchmarks/hashmap/hashmap_vs_map.kai
# fewer iterations (the N=100000 blocks are ~60-180ms each):
KAI_BENCH_ITERS=15 KAI_BENCH_WARMUP=3 \
  bin/kai bench benchmarks/hashmap/hashmap_vs_map.kai
```

Numbers vary per run and machine. The figures below are a sample from
an Apple-silicon laptop, `KAI_BENCH_ITERS=15`, medians.

## Sample run (median ns)

| N       | build Map | build HashMap | HM / Map | roundtrip Map | roundtrip HashMap | HM / Map |
|---------|-----------|---------------|----------|---------------|-------------------|----------|
| 100     |    65,000 |       110,000 |   1.69×  |        33,000 |            53,000 |   1.61×  |
| 1,000   |   853,000 |     1,121,000 |   1.31×  |       425,000 |           850,000 |   2.00×  |
| 10,000  | 5,144,000 |     8,902,000 |   1.73×  |     5,811,000 |        10,666,000 |   1.84×  |
| 100,000 | 59,753,000|   135,504,000 |   2.27×  |    78,054,000 |       175,838,000 |   2.25×  |

## Conclusion — the crossover has NOT arrived in v1

The textbook expectation is that a HAMT (`O(log32 n)`, depth ~6 at 1M)
overtakes an AVL tree (`O(log n)`, depth ~1.44·log2 n) as N grows. The
measurement shows the opposite: **the AVL `Map` is faster at every N
tested, and the gap widens slightly** (≈1.6–2.3×).

The asymptotic advantage is real but the **constant factor buries it**
at these N for two carrier-level reasons, both fixable and both left as
follow-up:

1. **List-backed bitmap children.** `HBranch` stores its children in a
   persistent `[HamtNode]` and edits them with
   `list.take(xs, i) ++ [node] ++ list.drop(xs, i)`. Each edit
   allocates a fresh cons chain up to `popcount(bitmap)` (≤ 32) long,
   *per level*, on every `put`. The AVL `Map` rebuilds only ~log(n)
   tight inline-record nodes per `put`, with no per-level array copy.
   An **`Array`-backed**, copy-on-write children representation (or a
   small-node specialisation for the common ≤ 4 children case) removes
   most of this overhead. This is the dominant term.

2. **Boxed `Int` values + RC.** Both keys and values are heap-boxed
   `Int`s under the current runtime, so every node touched pays
   `kai_incref` / `kai_decref`. The HAMT touches more distinct heap
   objects per `put` (a branch node *plus* its rebuilt child list) than
   the AVL touches (one record per spine node). Perceus reuse fires on
   the AVL spine more readily than on the list-rebuild path. Int
   unboxing (Tier 2, post-MVP) and a reuse-friendly children
   representation both shrink this term.

The HAMT is **correct** (the `examples/stdlib/hashmap_*` fixtures pass,
including the forced-`HCollision` worst case) and **persistent /
structurally sharing** as designed — it is simply not yet *faster* than
the AVL `Map` for these key/value types at these N. For v1 the guidance
is therefore:

- Use `Map` (AVL) when you need ordered iteration **or** raw speed at
  these scales.
- Use `HashMap` (HAMT) when you need keys without a total order (sum
  types with `impl Hash`) — the AVL `Map` cannot key on those at all
  (its `<` does not dispatch to non-primitive impls).

Closing the constant-factor gap (Array-backed children + Int unboxing)
is the natural HashMap performance follow-up; with both, the HAMT
should reach and pass the AVL crossover the theory predicts. Tracked in
the lane retro (`docs/lane-experience-issue-374.md`).
