# Red-black tree insert benchmark

The canonical Perceus showcase from Reinking, Xie, de Moura,
Leijen (2021) *"Perceus: Garbage Free Reference Counting with
Reuse"* (POPL'21), §7. Every Perceus implementation cites this
workload because it pushes exactly the spine-rebuild shape that
reuse-in-place is supposed to optimise.

This document records the **first measurement** from the kaikai
runtime, taken immediately after the structural RC repairs of
Anga Roa Phase R landed (issues #293 / #294 / #295 / #301 / #304,
merged into `main` 2026-05-04 → 2026-05-06).

## Workload

- Pure-functional Okasaki-style red-black tree over `Int → Int`.
- 1,000,000 random inserts into an empty tree.
- Keys are drawn from a Numerical-Recipes LCG (multiplier 1664525,
  increment 1013904223, modulus 2^31 - 1) seeded with 1. The
  same keystream feeds every implementation, so the comparison
  is apples-to-apples down to the bit level.
- Implementation lives in
  [`examples/perceus/rb_tree.kai`](../../examples/perceus/rb_tree.kai)
  + driver
  [`examples/perceus/rb_tree_bench.kai`](../../examples/perceus/rb_tree_bench.kai).
- C reference (intrusive RB tree + `std::set<int64_t>`):
  [`examples/perceus/rb_tree_bench_c.c`](../../examples/perceus/rb_tree_bench_c.c).
- Wrapper:
  [`examples/perceus/rb_tree_bench.sh`](../../examples/perceus/rb_tree_bench.sh).

The benchmark is **not** wired into any tier — it is too slow
for tier0 / tier1 and the absolute numbers are machine-dependent.
Run locally when investigating Perceus performance.

## Headline numbers

Hardware: Apple M1, macOS 25.4.0, kaikai @ `rb-tree-benchmark`
(off `main` post-#304, kaikai 0.43.0, clang 17, `-O2`).
All measurements taken from the lane worktree's freshly-rebuilt
`stage2/kaic2` (post-#304). Median of seven runs.

| Implementation        | Median elapsed | RSS    | Notes                                          |
| --------------------- | -------------: | -----: | ---------------------------------------------- |
| kaikai (this)         |       3 001 ms |1 929 MB| Functional Okasaki RB; reuse-in-place fires    |
| C (hand-written RB)   |         310 ms |  47 MB | Intrusive RB with parent pointers              |
| C++ `std::set<int64>` |         293 ms |  47 MB | libc++ RB tree                                 |

**Ratios**:

- kaikai / C: **9.68× wall-clock, 41.0× RSS**
- kaikai / C++: **10.24× wall-clock, 41.0× RSS**

Anga Roa Definition-of-Done #4 target: ≤ 1.7× stretch / ≤ 2.0×
relaxed. **Both gates fail.**

## Reuse-in-place trace

Re-running with `KAI_TRACE_RC=1`:

```
[KAI_TRACE_RC] alloc_total=28629837 free_total=6673403 leaked=21956434 live_peak=21956449
[KAI_TRACE_RC]   tag int     allocs=7017419
[KAI_TRACE_RC]   tag str     allocs=19
[KAI_TRACE_RC]   tag record  allocs=3
[KAI_TRACE_RC]   tag variant allocs=21612396
[KAI_TRACE_RC]   reuse_in_place=1000000
```

Two facts dominate the analysis:

1. **`reuse_in_place=1,000,000`** — exactly one reuse per insert.
   That is the `rb_insert` root-repaint site (the
   `RBNode(_, l, k0, v0, r) -> RBNode(Black, l, k0, v0, r)` arm)
   firing on a uniquely-owned cell. The shape predicate (issue
   #210, PR #289) admits the rewrite and the runtime helper
   `kai_reuse_or_alloc_variant` reuses the cell in place. Good.

2. **21.6M variant allocations vs 1M inserts → ≈ 21.6 alloc per
   insert.** The `balance(...)` body unconditionally rebuilds
   three new `RBNode` cells on every spine step (the Okasaki four
   cases all `RBNode(Red, RBNode(Black, ...), k, v, RBNode(Black, ...))`).
   The shape predicate sees a **structural reorganisation** —
   the new tree's left-child key is the old node's left-grandchild
   key — so the reuse-in-place pass conservatively declines the
   rewrite even when the scrutinee is unique. Result: every spine
   level pays one fresh `kai_variant` alloc per node touched.

The 21.6 number itself decomposes as approximately:
  - 1.0 alloc per insert at the root repaint (covered by reuse)
  - ~22 allocs along the spine of length ≈ log2(N) = 20 (one
    `balance(...)` call per level, mostly hitting the `_ -> RBNode(c, l, k, v, r)`
    fall-through arm — which is also a fresh alloc, not a reuse,
    because it constructs a new node from variables rather than
    consuming a scrutinee).

## RSS gap

`alloc_total - free_total = 21.96M cells leaked at end of run`.
Each variant cell is ~96 bytes (header + 5 fields including the
boxed `Int` payloads), so the residual heap is ≈ 2.0 GB —
matching the observed 1929 MB RSS within rounding.

The leak source is **not** the reuse pass; the trace says reuse
itself fires on every eligible site. The leak source is the
**spine rebuild** in `balance(...)`. Each insert allocates ~22
fresh variant cells on the way down + the closure-captured `t`
parameter holds the old spine alive until the call returns. RC
should free those old cells when `t` drops, but `free_total`
shows only ~6.7M frees vs 28.6M allocs — i.e. the old spines
are leaking, not just sitting briefly live. This is the same
"emitter has no RC discipline" pattern catalogued in the
runtime-debt audit (2026-04-28).

So the headline 41.0× RSS gap is actually two effects compounding:
1. The spine rebuild allocates more cells than the C version
   even in the best case (every `balance` rebuilds 3 cells; the
   intrusive C version mutates 0–1 cells per rotation, plus 1
   for the new node).
2. The compiler doesn't emit decrefs for the dropped intermediate
   trees — they stay live forever, even after `fill_loop`'s tail
   call has consumed them.

## Why the time gap is also large

A tight inner loop in C compiles to ~10 instructions of pointer
chasing per insert + occasional rotation. The kaikai version
allocates 22 variant cells per insert through `kai_variant`,
each of which is a `malloc` + tag-set + payload-init. Per-alloc
cost dominates by a factor of ~30, and 22 allocs × 30 ns ≈ 660
ns per insert × 1M = 660 ms — but the measured 3.00 s says we are
also paying RC traffic (incref / decref pairs that never reduce
the live count to zero) on the boxed `Int` payloads. The
`tag int allocs=7M` row confirms: every `k`, `v`, `n` value
inside `balance(...)` is being heap-allocated because parameters
are kept boxed (Phase 2 unboxing — issue #267 — only fires on
local arithmetic chains, not on parameters).

## Gap analysis vs Anga Roa DoD #4

DoD #4 target was set in `docs/roadmap.md` *before* the runtime
debt audit (2026-04-28) reformulated the roadmap into Phase R
(R1: Perceus flip → R2: m8.x scheduler) **before** the m11/m13/m14
tiers. Phase R's Phase 3 (PR landed 2026-04-28) cut RSS by 52%
and `live_peak` by 63% on the selfhost workload, which made this
benchmark *measurable* — but the per-insert allocation cost on
this workload is still dominated by:

- The variant-cell allocator (`kai_variant`) takes ~30 ns per
  call; libc malloc would be ~20 ns. The 50% overhead is the
  RC header bookkeeping.
- Boxed `Int` parameters cost an extra `kai_int` alloc each.
  Issue #251/#252 unboxing-on-parameters would eliminate the
  7M extra allocs but is post-MVP.
- The four-case `balance` body declines reuse-in-place because
  the shape predicate is conservative on rotations. Tightening
  it to admit "same arity, same type, scrutinee unique, output
  cell can be in-place patched" would, in principle, drop the
  variant alloc count from 22M to ~1M (one per root repaint
  + one per leaf insert).

None of these is a "fix the benchmark" change — they are
load-bearing runtime work that belongs in m5.x and m12.x lanes
respectively. **Filing the gaps as issues is the right next
step**, not band-aiding the benchmark fixture.

## Reproduction

```sh
git checkout rb-tree-benchmark
make tier0                        # ensures kai is current
./examples/perceus/rb_tree_bench.sh
```

Output is the table above (machine-dependent absolute numbers,
implementation-comparable ratios).

For the RC trace:

```sh
bin/kai build examples/perceus/rb_tree_bench.kai -o /tmp/rb_kai_bench
KAI_TRACE_RC=1 /tmp/rb_kai_bench
```

## Honesty

This benchmark is published with the gap, not despite it. The
Anga Roa #4 number we have today is **9.68×, not 1.7×**. That is
useful because:

- It pins the *current* state at a defensible reference point.
  Future Perceus work can cite "we were at 9.68× on rb_tree, we
  are now at N×".
- It identifies the specific load-bearing bottlenecks (spine
  rebuild + parameter boxing + RC discipline) instead of leaving
  them as folk knowledge.
- It validates that the *shape predicate* is correctly firing
  where it is allowed to fire (1M reuses, exactly as expected
  for the root repaint). The remaining gap is structural, not
  a bug in the reuse pass.

DoD #4 is not met. The roadmap path forward (Phase R Phase 4 +
post-Phase-R unboxing-on-parameters) is what closes it; the
benchmark above is the empirical evidence we use to decide
whether each of those lanes lands the expected improvement.
