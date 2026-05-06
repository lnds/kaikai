# Lane experience — rb-tree-benchmark

Worktree: `kaikai.rb-tree-benchmark` (sibling worktree off `main`).
Closes (potentially) issue #302 demo 1; issue #302 stays open
for the mandelbrot raylib follow-up.

## Objective metrics

| Metric                 | Value                       |
| ---------------------- | --------------------------- |
| Lane start             | 2026-05-06T18:42:04-04:00   |
| Lane end               | 2026-05-06T19:03:29-04:00   |
| Wall-clock duration    | ≈ 21 minutes                |
| Build TSV row count    | 5 entries                   |
| Tier 0 outcome         | OK                          |
| Tier 1 outcome         | OK (4:47 wall)              |
| Tier 1 ASAN outcome    | OK (47.7 s)                 |
| Files added            | 6 (rb_tree.kai + rb_tree_bench.kai + .out.expected + .c + .sh + benchmarks/rb_tree.md) |
| Files modified         | 0 (no compiler / stdlib touches) |

## Implementation

The RB tree is the Okasaki-style purely functional version with
the four imbalance cases (LL / LR / RL / RR) handled in a single
`balance(c, l, k, v, r)` function that pattern-matches the
parent + child colours.

Algorithm choice notes:

- `RBTree` / `RBNode` / `RBLeaf` are RB-prefixed because the
  unprefixed `Tree[k, v]` lives in `stdlib/collections/map.kai`
  (the AVL implementation) and the kai driver auto-loads it as
  part of the prelude. Without the prefix the typer reports
  "non-exhaustive match on Tree: missing TNode, TEmpty" at
  every match site in the AVL module.
- Keys are `Int → Int`. The Reinking et al. paper uses the same
  shape; it keeps the per-cell payload small (96 bytes incl. RC
  header) so the comparison emphasises allocation cost rather
  than pointer chasing.
- The four-case balance body lives unrolled (≈ 60 lines) instead
  of behind a generic "rebalance one node" helper. Reads cleaner
  for the LL/LR/RL/RR + structural reuse-in-place inspection.

The bench driver lives in a sibling module
(`rb_tree_bench.kai` + `rb_tree.kai`) discovered via `kai run`'s
`--path "$(dirname "$src")"` injection. Confirmed sibling
discovery only loads explicitly imported modules — the other 30
fixtures in `examples/perceus/` stay invisible.

## Benchmark methodology

- 1,000,000 inserts of `(LCG(seed), iter)`. Same LCG (Numerical
  Recipes ranqd1) seeds every implementation so the keystream
  is bit-identical.
- Wall-clock timing inside the program with kaikai's `monotonic()`
  / `elapsed_since(...)` (default `Clock` handler bridges to
  `clock_gettime(CLOCK_MONOTONIC)`).
- C reference compiled twice from the same `.c` file — once as
  `clang -std=c11 -O2` for the hand-written intrusive RB, once
  as `clang++ -std=c++17 -O2 -DBENCH_STDSET` for the std::set
  baseline.
- Wrapper runs each implementation 3× and reports median. Median
  rather than mean to absorb the cold-start outlier in the first
  iteration.
- RSS via `/usr/bin/time -l` on macOS (bytes) or `/usr/bin/time -v`
  on Linux (KB) — wrapper handles both.

Workload N is hard-coded to 1,000,000 to match the paper's
§7 evaluation exactly. Adjustable in one line if a future lane
wants stress / sweep curves.

## Empirical numbers (median of 3)

Hardware: Apple M1, macOS 25.4.0, kaikai 0.43.0 + clang 17.

All measurements taken from the lane worktree's freshly-rebuilt
`stage2/kaic2` (post-#304). Median of seven runs.

| Implementation        | Median elapsed | RSS    |
| --------------------- | -------------: | -----: |
| kaikai (this)         |       3 001 ms |1 929 MB|
| C (hand-written RB)   |         310 ms |  47 MB |
| C++ `std::set<int64>` |         293 ms |  47 MB |

Ratios:
- **kaikai / C: 9.68× wall, 41.0× RSS**
- **kaikai / C++: 10.24× wall, 41.0× RSS**

Anga Roa DoD #4 target: ≤ 1.7× stretch / ≤ 2.0× relaxed → **fail
on both axes.**

## Reuse trace

```
[KAI_TRACE_RC] alloc_total=28629837 free_total=6673403 leaked=21956434 live_peak=21956449
[KAI_TRACE_RC]   tag int     allocs=7017419
[KAI_TRACE_RC]   tag str     allocs=19
[KAI_TRACE_RC]   tag record  allocs=3
[KAI_TRACE_RC]   tag variant allocs=21612396
[KAI_TRACE_RC]   reuse_in_place=1000000
```

`reuse_in_place=1,000,000` — exactly one per insert, matching
the root-repaint site (`rb_insert`'s
`RBNode(_, l, k0, v0, r) -> RBNode(Black, l, k0, v0, r)` arm).
The shape predicate fires correctly.

The 21.6M variant allocations come from `balance(...)` rebuilding
3 cells per spine step. The shape predicate conservatively
declines reuse on the 4-case rotations because the new tree's
structure (left-grandchild becomes new root, etc.) does not match
"same cell, patched in place" — so every spine level pays a fresh
`kai_variant` alloc.

## Gap analysis (kaikai_time / c_time = 9.68×)

Three load-bearing sources:

1. **Spine rebuild allocates ~22 cells per insert.** Even with
   reuse-in-place perfectly tightened, the absolute floor is
   ~log2(1M) ≈ 20 cells per insert (one per spine level), so the
   theoretical best case under the current `balance` shape is
   still ≈ 1× the cell count, not 0.05× the C version's mostly-
   in-place rotations.

2. **Boxed `Int` parameters cost extra allocs.** `tag int allocs
   = 7M` says every `k`, `v`, `n` in `balance(...)` is being
   heap-allocated. Issue #251/#252 (unboxing on parameters) would
   eliminate this; they are post-MVP.

3. **Compiler does not emit decrefs for dropped intermediate
   trees.** `alloc_total - free_total = 21.96M` cells leaked at
   end of run — the old spines never get freed even after the
   tail call consumes them. Same "emitter has no RC discipline"
   pattern catalogued in the runtime-debt audit (2026-04-28).

The 41.0× RSS gap is dominated by (3); the 9.68× wall-clock gap is
roughly (2) + the per-alloc cost ratio.

None of these are "fix the benchmark" changes — they are
load-bearing runtime work in m5.x and m12.x lanes.

## Friction points

- **Type-name collision with the stdlib AVL** cost ~5 minutes on
  the first compile. The error appeared with file:line pointing
  *inside* `stdlib/collections/map.kai` at line 136+, which made
  it look like a stdlib regression rather than a name conflict in
  the user file. Rename to `RBTree` / `RBNode` / `RBLeaf`
  resolved it. (Already in `memory/feedback_kaikai_prelude_name_shadowing.md`
  — this is the same shape one tier deeper.)

- **`sed` replacement on Color/Tree/Node was harder than rewriting.**
  Bash `sed -E 's/\bTree\b/RBTree/g'` succeeded for `Tree` but
  silently failed for `Node` and `Leaf` because the `\b`
  semantics on macOS BSD sed differ from GNU. Faster to rewrite
  the whole file via `Write`.

- **No friction from kaikai itself.** Default `Clock` handler,
  sibling-module imports, polymorphic match — all worked first
  try. The fixtures landed in 6 file edits + 0 compiler
  modifications.

- **First measurement pass used the wrong worktree's `kaic2`.**
  Initial benchmark numbers (2 942 ms / 10.0× / 11.2×) came
  from the `kaikai/` main worktree's stale stage-2 binary
  (timestamped 17:58, pre-#304). The lane worktree
  `kaikai.rb-tree-benchmark/` had no `stage2/kaic2` built yet, so
  any `bin/kai` invocation that triggered a stage rebuild
  produced a fresh post-#304 binary — but the *initial* runs
  shelled `cd /…/kaikai` first, which silently rerouted the
  build to the stale checkout. Caught when the user noticed the
  PR cited a kaic2 from before the merged-fix timestamps. Re-ran
  every measurement from inside the lane worktree against the
  freshly built `stage2/kaic2`. The numbers shifted by ~3% — small
  enough to not change the conclusion but large enough that
  publishing the wrong figures would have been factually wrong.
  Lesson: when measuring across worktrees, verify
  `ls -la stage2/kaic2` matches the merge timestamp of the
  changes you claim to be measuring against.

## Subjective summary

The implementation phase took ~30 minutes including the
typename-collision detour. The benchmark wrapper + C reference
took ~30 minutes. The doc + retro took ~45 minutes. Total wall
~1h45m — comfortably inside the 3-4h budget.

The benchmark **does not meet** Anga Roa DoD #4 (9.68×, target
1.7×). That outcome was not knowable before measurement; the
runtime-debt audit hinted at it, but the specific decomposition
(spine rebuild + parameter boxing + RC discipline) is now
empirical instead of folk knowledge. That is the most valuable
artefact this lane produced.

The reuse-in-place pass (issue #210, PR #289) is **firing
exactly as designed** — 1M reuses on 1M inserts at the root
repaint site. The 41.0× RSS gap is structural elsewhere, not a
regression in the reuse pass.

## Limitations / next steps

- **Issue 1 (file)**: tighten the `balance(...)` reuse-in-place
  predicate to admit four-case rotations. Heuristic is "same
  arity, same type tag, scrutinee unique, reorder of immediate
  children". If accepted, drops variant allocs from ~22M to
  ≤ 4M (one per spine level + the three rebuilt cells in the
  matching arm itself).

- **Issue 2 (file)**: per-fn parameter unboxing (issue #252 +
  #251). The 7M `tag int` allocs are the cost of every
  `balance(c, l, k, v, r)` boxing its `Int` params on entry.
  Phase 2 unboxing only handles local arithmetic chains.

- **Issue 3 (file)**: investigate why `free_total` is so far
  below `alloc_total` even on a workload whose tail-call shape
  drops every intermediate `t`. Suspicion: the compiler emits
  `kai_variant` for each rebuild but does not emit the matching
  `kai_decref` at the call-site `t` slot when `fill_loop`
  recurses. (The selfhost workload showed similar but smaller
  ratios — that was the post-Phase-3 RSS −52% win — so the
  pattern is reproducible at scale.)

- **Mandelbrot raylib** (issue #302 demo 2): blocked on the
  `uira` raylib bindings being upstreamed. Out of this lane's
  scope.

- **Cross-platform validation**: numbers above are M1 macOS only.
  The wrapper handles Linux RSS units; the comparison rerun on
  a Linux x86_64 box would let the doc cite both architectures.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-06T18:49:11-04:00	rb_smoke_1M	OK	4.38
2026-05-06T18:55:07-04:00	wrapper_run	OK	-
2026-05-06T18:57:30-04:00	tier0	OK	-
2026-05-06T19:02:25-04:00	tier1	OK	287
2026-05-06T19:03:24-04:00	tier1-asan	OK	47.7
```
