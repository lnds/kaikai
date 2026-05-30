# Measuring the Perceus promise — 2026-05-29

Goal of this measurement: not "make rb-tree fast" but answer the real
question — **how much of the Perceus promise (reuse-in-place, no GC,
decent performance) does kaikai deliver today, and where is the
ceiling?** Measured on the cleanest case where reuse-in-place fires at
100%, isolated from the rb-tree's confounding factors.

No compiler changes. Pure measurement.

## The isolation benchmark

A linear in-place list transform — the canonical reuse-in-place shape:

```kaikai
fn bump(xs: [Int]) : [Int] = match xs {
  []        -> []
  [h, ...t] -> [h + 1, ...bump(t)]   # consumes [h,...t], rebuilds same shape
}
```

Under Perceus, each consumed cons cell must be REUSED in place (the list
is uniquely owned), so `bump` should allocate ZERO new cons cells. The C
reference is a linked-list walk that mutates `p->v += 1` in place — the
literal thing Perceus promises to match without GC.

Workload: 50,000-element list, 1,000 bump passes (amplified so wall is
measurable; deep recursion overflows the C stack at 1M, a fixture
limitation, not a Perceus one).

## Results (M4 Pro, -O2, median of 5)

| | kaikai | C (in-place linked list) | ratio |
| --- | ---: | ---: | ---: |
| Wall | 1.11 s | 0.04 s | **~28× C** |

## What the trace shows (KAI_TRACE_RC)

```
alloc_total=50,142,748  free_total=50,142,746  leaked=2
  tag cons   allocs=50,000        reuse_in_place=50,000,000
  tag int    allocs=50,092,745
```

**The headline finding:**

- **Cons-cell reuse-in-place WORKS PERFECTLY.** 50M cells reused, ZERO
  new cons allocations across all 1,000 passes. The list is transformed
  in place exactly as Perceus promises. `leaked=2` (essentially nothing).
- **BUT there are 50 MILLION `Int` allocations.** Every `h + 1` inside
  `bump` boxes a fresh `kai_int(...)`. The cons cell is reused; the
  integer VALUE that goes inside it is re-allocated every single time.

So the 28× gap is **entirely the boxing of `Int`**, not cell allocation.
Reuse-in-place already solved the cells. RC traffic profile confirms:
alloc 9.6% + free 10.6% + rc_traffic 37.8% of wall — all of it driven by
the 50M throwaway int boxes and their refcount churn.

## Why this is the unifying result

The two perf lanes attempted 2026-05-28 night (bind-site unboxing,
mixed-param signature unboxing) both tried to kill `Int` boxing but were
applied to the rb-tree, where the variant-rebuild alloc diluted the
effect and the added marshalling made them net-negative. This benchmark
isolates the variable: **with cell allocation eliminated by reuse, the
ONLY residual cost is `Int` boxing — and it is the whole 28×.**

The Perceus promise in kaikai today:
- **Cells: delivered.** Reuse-in-place eliminates cons (and mask-0
  variant) allocation. This is real and works.
- **Scalars: not delivered.** Every primitive value lives boxed on the
  heap; arithmetic allocates. This is the gap between kaikai and Koka,
  where a small `Int` is a value-immediate (tagged pointer), never a
  heap box.

## The honest path to the promise

To actually deliver "Perceus = no GC + decent perf", the lever is **value-
immediate / unboxed primitives that survive across the reuse boundary** —
so `h + 1` does not allocate. Concretely the `head` of a `[Int]` (and the
unboxed fields of a reused variant) must be raw scalars end to end, not
boxed `KaiValue *`. This is the same "primitive-slot extract without a
fresh `kai_int`" follow-up the Phase 4 honesty target named, now shown by
measurement to be THE dominant cost once reuse handles the cells.

This is a real lane (touches the int representation across the hot path +
both backends + selfhost), but it is now pointed at the right target,
measured, not guessed. The rb-tree's 14.78× will only move once BOTH
land: reuse-in-place for its mask-N variants (the LLVM typed-slot lane)
AND unboxed `Int` that survives the reuse boundary (this finding).

## Reproduction

```sh
# kaikai (reuse fires, int boxing dominates):
CFLAGS="... -DKAI_TRACE_RC=1" bin/kai build <bump-1000-passes>.kai -o /tmp/k
KAI_TRACE_RC=1 /tmp/k     # see reuse_in_place=50M, int allocs=50M

# C in-place reference: a Node{v,next} list, bump mutates p->v += 1.
```
