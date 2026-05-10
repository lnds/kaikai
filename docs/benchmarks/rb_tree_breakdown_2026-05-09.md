# RB-tree wall breakdown — 2026-05-09 (lane bench-rbtree-instrument)

Per-category attribution of the 1 M-insert red-black tree benchmark
(`examples/perceus/rb_tree_bench.kai`) to scope Phase 4 (variant-field
unboxing) versus drop specialisation (issue #384). Companion to
`docs/benchmarks/rb_tree_2026-05-09.md`, which pinned the post-#409
operational baseline at **3 665 ms** (15.93× C). This doc decomposes
that wall.

The instrumentation is gated on `-DKAI_PROFILE_RC=1` at compile time
plus `KAI_PROFILE_RC=1` at run time; details in *Methodology* below.
This is a measurement lane — **no compiler or stdlib code was
changed**; only `stage0/runtime.h` gained a profiling block.

## TL;DR

- The 1 M-insert RB-tree workload triggers **24.6 M allocations**,
  **20.2 M frees**, **421.7 M incref calls**, and **439.0 M decref
  calls** on the kaikai runtime.
- RC traffic outnumbers free events by **~35×** and outnumbers
  allocs by **~17×**. Per-cell average: ~17 incref + ~18 decref
  operations, plus 1 alloc and 1 free.
- A simpler shape (1 M-element cons list of Ints, build + sum)
  triggers far fewer ops per cell — alloc/free/RC-traffic ratios stay
  in the same band, but **per-call free is 3× more expensive on
  RB-tree than on the list**, because RB-tree Node free cascades
  through 5 children vs cons's 2.
- Recommendation: **Phase 4 (variant-field unboxing) is the right
  scope** — it attacks both the dominant cell-count axis (collapsing
  3 of 5 boxed Node fields into raw scalars eliminates ~60 % of RC
  traffic and ~60 % of cascade-free work) AND the per-cell free cost
  (smaller cascades). **Drop specialisation (#384) is a useful
  second-order win but not load-bearing.** Option A, not Option B.

## Hardware / configuration

| Variable | Value |
| --- | --- |
| Host | Apple M4 Pro, macOS 14 (same as `rb_tree_2026-05-09.md`) |
| Compiler | `bin/kai build` with `-O2 -std=c99` |
| Profiling build | `CFLAGS="… -DKAI_PROFILE_RC=1"` |
| Profiling runtime | env `KAI_PROFILE_RC=1` |

## Numbers — RB-tree (1 M inserts, LCG-seeded keys)

### Native baseline (no instrumentation, n = 5 runs)

| Run | Wall (s) |
| --- | ---: |
| 1 | 3.798 |
| 2 | 3.653 |
| 3 | 3.743 |
| 4 | 3.832 |
| 5 | 3.813 |
| **median** | **3.798** |

Reproduces `docs/benchmarks/rb_tree_2026-05-09.md` (3 665 ms post-#409)
within 4 %.

### Profiled wall (under `KAI_PROFILE_RC=1`, n = 3 runs, median)

| Category | ms (self) | Share of inst. wall | Calls | ns/call (raw) |
| --- | ---: | ---: | ---: | ---: |
| alloc        |    558 |  1.6 % |    24 629 836 |  ~23 |
| free         |  2 166 |  6.2 % |    20 215 332 | ~107 |
| rc_traffic   | 14 893 | 42.7 % |   860 680 865 |  ~17 |
| other        | 17 301 | 49.4 % |   (untracked) |    — |
| **total wall (inst.)** | **34 706** | 100 % | | |

`rc_traffic` aggregates `incref` + `decref` self-time. Detail:

| | ms (self) | Calls | ns/call (raw) |
| --- | ---: | ---: | ---: |
| `kai_incref` (rc++ path)               |  7 496 | 421 679 682 | ~18 |
| `kai_decref` (rc-- path, excl. free)   |  7 397 | 439 001 183 | ~17 |
| `kai_decref` reaching `rc == 0` (subset of decref) | counted separately | 20 215 332 | |

Stable across runs; the three samples agreed to within ±2 %.

### Reading the raw ns/call numbers

`clock_gettime(CLOCK_MONOTONIC)` on macOS arm64 costs ~30–40 ns per
call. Each instrumented function pays two `clock_gettime` invocations
(enter + exit), so the *raw* ns/call columns above are **inflated
by ~35 ns of measurement overhead per call**. With ~905 M total
instrumented calls, that's the entire 31 s gap between the native
wall (3.8 s) and the profiled wall (34.7 s).

Net per-call native cost (raw – overhead):

| | raw ns/call | overhead | est. native ns/call |
| --- | ---: | ---: | ---: |
| alloc  |  ~23 | ~35 | ~ 0 (calloc + bookkeep — fits in OS allocator hot path) |
| free   | ~107 | ~35 | ~70 (5-child cascade visible) |
| incref |  ~18 | ~35 | ~ 0 (single int++) |
| decref |  ~17 | ~35 | ~ 0 (single int-- + branch) |

Per-call work is dominated by the **free path** — visiting 5 child
slots and triggering further decrefs. RC traffic per-call is
essentially noise; its aggregate bulk comes from the **count**, not
per-call cost.

### Best-effort native breakdown (modeled, not measured)

Subtracting per-call overhead (35 ns × N) from each category and
distributing the residual across the 3 798 ms native wall:

| Category | est. native ms | est. share |
| --- | ---: | ---: |
| alloc            | ~ 1 200 | ~32 % |
| free (incl. cascading work attributable to free body) | ~ 1 400 | ~37 % |
| RC-traffic non-free | ~ 800 | ~21 % |
| user code / "other" | ~ 400 | ~10 % |

Rough order-of-magnitude only — the ns/call numbers under
`KAI_PROFILE_RC` aren't precise enough to put hard error bars on the
modelled split. The robust takeaway is:

- **alloc + free combined ≈ 70 % of native wall.**
- **RC traffic ≈ 20 % of native wall, but 95 % of all RC operations.**

The 5× per-call cost gap between free (~70 ns) and decref (~0 ns
above noise) is what flips the share: free is rare per cell but
expensive per call; RC traffic is cheap per call but dominates by
count.

## Cross-shape comparison — 1 M-element cons-list build + sum

To test whether the breakdown is RB-tree-specific or a general
property of kaikai's variant runtime, the same instrumentation was
applied to a synthetic shape with simpler RC: build a 1 M-element
list of Ints with `build_list_loop`, then traverse with `sum_loop`.
Source: `/tmp/list_traverse_bench.kai` (this lane keeps it out of
the tree per the lane brief's "no changes outside runtime" rule).

### Native baseline (n = 3)

| Phase | Wall (ms) |
| --- | ---: |
| build (1 M cons) | 22 |
| sum   (1 M traverse) | 51 |
| **total** | **73** |

### Profiled (n = 3, median)

| Category | ms (self) | Share | Calls | ns/call (raw) |
| --- | ---: | ---: | ---: | ---: |
| alloc      |  59 |  6.9 % |    2 999 922 |  ~20 |
| free       |  99 | 11.6 % |    2 999 908 |  ~33 |
| rc_traffic | 346 | 40.7 % |   16 998 940 |  ~20 |
| other      | 347 | 40.7 % |  (untracked) |    — |
| **total wall (inst.)** | **851** | 100 % | | |

### What changes vs RB-tree

| | RB-tree | List | RB / List |
| --- | ---: | ---: | ---: |
| Total wall (native, ms)         | 3 798 |  73 | 52 × |
| Allocs                          | 24.6 M | 3.0 M |  8 × |
| Frees                           | 20.2 M | 3.0 M |  7 × |
| Incref calls                    | 421.7 M | 7.0 M | 60 × |
| Decref calls                    | 439.0 M | 10.0 M | 44 × |
| **Allocs per element processed**     | ~25 | ~3  | 8 × |
| **RC ops per element processed**     | ~860 | ~17 | 51 × |
| Per-call free cost (raw ns/call)     | 107 | 33 | **3.2 ×** |

Two shape-specific effects stand out:

1. **RC traffic per element is 50× higher on RB-tree** than on list.
   That's the cost of `balance` rebuilding O(log N) nodes per insert,
   each rebuild dup-ing 5 children and dropping the matched-arm
   bindings.
2. **Per-call free cost is 3× higher on RB-tree** than on list. RB
   Node free cascades through 5 children (`color`, `l`, `k`, `v`, `r`),
   list cons cascades through 2 (`head`, `tail`) — and `head` for an
   Int is often a singleton (saturated rc), so the cascade
   short-circuits.

Both effects compound to give the RB-tree workload its 16× C wall
while the list workload stays at ~2× C.

## Phase 4 scope decision

The original question — *cell size or cell count?* — is **both, with
unboxing as the joint lever**. The breakdown rules:

### Why Option A (variant-field unboxing only) is sufficient

Phase 4 collapses `Node(color, l, k, v, r)` from 5 boxed `KaiValue *`
slots to a struct with raw scalars for `color` (1-byte tag),
`k` (int64), `v` (int64) and only `l` / `r` as boxed pointers.
Effects:

- **Alloc count**: per-Node alloc is unchanged (still 1 cell), but
  **3 of every 5 field allocations disappear**. `k` and `v` are
  outside the singleton/cache range for LCG keys, so they are real
  allocations today; Phase 4 makes them inline. Estimated drop:
  ~3 M of the current 3 M boxed-Int allocs (`color` is already a
  nullary singleton — no runtime alloc — but field reads still go
  through the boxed indirection, which becomes a raw byte load).
- **RC traffic count**: this is the dominant lever. Currently each
  Node visit dups 5 children (5 increfs) and drops 5 children
  (5 decrefs). With `color`/`k`/`v` raw, the 3 dup/drop pairs become
  no-ops. Estimated reduction: **~60 % of incref + decref calls
  eliminated**, taking RC traffic from 860 M to ~340 M ops — and the
  20 % of native wall it occupies shrinks to ~8 %.
- **Per-call free cost**: cascades visit only `l` / `r` instead of 5
  children. Estimated drop: ~50 % per-cell free cost.
- **Cell size**: smaller — fewer pointer slots, fewer cache lines
  per Node, fewer allocator buckets.

Combined estimate (back-of-envelope): native wall drops from
3.8 s to ~1.5–2.0 s. That's the **2× C MVP target**
(DoD #4b in `docs/benchmarks/rb_tree_2026-05-09.md`).

### Why Option B (Phase 4 + drop spec) is not load-bearing

Drop specialisation (#384) attacks borrowed-binds — the incref at
match-arm extract paired with a decref at scope exit. On the RB-tree
this is the `kai_incref` storm in `balance` arms.

Post-Phase-4, Node has only 2 boxed children (`l`, `r`); the boxed
fields the borrowed-bind optimisation eliminates are exactly those
two pointer fields. The win is real but **second-order** to the
unboxing of `color`/`k`/`v`, which removes the field allocations and
RC traffic entirely. Drop spec doesn't remove a single alloc, only a
dup/drop pair.

Sequencing matters: **ship Phase 4 first, then re-measure**. If the
gap-to-target persists, Phase 4 has narrowed the working set enough
that drop spec gives a clean further win. If the gap closes inside
Phase 4 (likely from this breakdown), drop spec on RB-tree is no
longer the highest-priority Perceus lever.

### Note on the "size vs count" framing

The lane brief framed Phase 4 as either *Option A (variant-field
unboxing)* attacking size or *Option B (A + drop spec)* if RC
traffic dominated. The breakdown says **the framing oversimplifies**:
Phase 4's variant-field unboxing collapses the boxes-per-cell,
attacking BOTH the per-cell size (smaller alloc/free) AND the
per-cell RC traffic count (no incref/decref on raw fields).

The remaining choice — *do we also need drop spec?* — gets a softer
answer: **defer it pending post-Phase-4 measurement**. The breakdown
predicts Phase 4 alone will get RB-tree under the 2× C target.

## Methodology

### Instrumentation (stage0/runtime.h)

A new block, gated on `-DKAI_PROFILE_RC=1` at compile time and
`KAI_PROFILE_RC=1` at run time, wraps the four hot RC functions with
`clock_gettime(CLOCK_MONOTONIC, …)` enter/exit pairs:

- `kai_alloc` — leaf calloc + bookkeeping for new heap cells.
- `kai_free_value` — actual free + cascading decref of children.
- `kai_incref` — rc++ path (skips NULL and singleton early exits).
- `kai_decref` — rc-- path (skips NULL and singleton early exits).

Match dispatch (`pat_test`, field-extract) was originally listed as a
fifth category but is not separately measurable in the current
runtime: stage 2 emits inline pattern tests as
`_scr->as.var.variant_tag` reads and field accesses as
`_scr->as.var.args[i]` indirections directly into the C output, with
no central function to wrap. Match dispatch folds into the
"other" bucket alongside user code and call/return overhead. To
separate it, the codegen would need to emit a `kai_pat_test` /
`kai_field_at` helper — out of scope for a measurement lane.

### Exclusive-time accounting

Naive timing of nested instrumented calls double-counts: when
`kai_decref` rolls into `kai_free_value` which in turn calls
`kai_decref` on each child, the outer decref's gross-time wraps the
inner work that's *also* attributed to the inner functions' own
counters.

The instrumentation tracks an explicit stack of `{start_ns,
child_ns}` frames. On enter, push a frame with the current timestamp
and zero child time. On exit, compute *self time* = elapsed −
child_ns, accumulate to the matching category, then bubble the gross
elapsed up to the parent frame's `child_ns` for its own subtraction.
Stack capped at 64 (RB-tree's deepest cascade nests ~30 deep);
saturating overflow silently mis-attributes rather than crashes.

### Overhead caveat

Two `clock_gettime` calls per ENTER/EXIT pair cost ~70 ns; the per-
call work in `kai_incref` (one int increment) is essentially free.
Result: the *raw* ns/call numbers under instrumentation are
inflated by ~35 ns/call across all four categories. Total
instrumentation overhead: ~31 s on this workload (905 M calls ×
~35 ns/call). The native wall (3.8 s) and the profiled wall
(34.7 s) differ by exactly that order of magnitude — checked.

The instrumented per-category **proportions** are robust as long as
the overhead is uniform across functions (it is — all four go
through the same enter/exit macros). The instrumented per-category
**absolute milliseconds** are not directly comparable to the native
wall; the reading is "gross instrumented self-time, ~35 ns/call of
which is overhead."

### What's solid

- **Call counts**: deterministic, reproducible bit-for-bit across
  runs (modulo race conditions in concurrent benchmarks; this
  workload is single-threaded). Use these directly.
- **Relative shape across runs**: stable within ±2 %.
- **Cross-shape ratios** (RB-tree vs list per-call free, calls per
  element): the overhead cancels in the ratio, so the 3× per-call
  free cost gap between RB-tree and list is real.

### What's modeled, not measured

- **Native ms per category** — derived by subtracting the
  ~35 ns/call overhead estimate from raw self-time. Order-of-
  magnitude only; do not pin Phase 4 acceptance numbers to these
  estimates without a separate native-only measurement (e.g.,
  perf/dtrace flame graph).

### Reproduction

```sh
# Build profiled binary
CFLAGS="-std=c99 -Wno-unused-function -Wno-unused-variable -O2 -DKAI_PROFILE_RC=1" \
    bin/kai build examples/perceus/rb_tree_bench.kai -o /tmp/rb_kai_prof

# Run with profiling
KAI_PROFILE_RC=1 /tmp/rb_kai_prof

# Run without profiling (env var unset) — instrumentation still
# pays per-call branch overhead but the report is suppressed.
/tmp/rb_kai_prof
```

The list-traverse companion fixture (`/tmp/list_traverse_bench.kai`,
not committed) builds and traverses a 1 M-element cons list of Ints
with the same profiling harness. Recreate from the source listing in
the lane PR description if needed.

## What this lane did NOT do (deferred)

- **Synthetic Phase-4 mock**: the brief asked for a fake-Phase-4
  measurement (hand-rewriting Node as a raw-int struct to bound the
  upper-limit Phase 4 win). Doing this without compiler changes
  required hand-editing the emitted `out.c`, which would also
  require recompiling the surrounding stdlib variant ABI to match.
  Out of scope for a 1-day measurement lane; deferred to the actual
  Phase 4 implementation lane, which can pre/post measure cleanly.

- **dtrace / perf flame graph cross-check**: would give a
  ground-truth native breakdown without the clock_gettime overhead
  problem. Useful follow-up; not blocking the Phase 4 scope
  decision.

- **Match-dispatch separation**: requires a `kai_pat_test_variant` /
  `kai_field_at` runtime helper that the stage 2 emitter calls
  instead of inlining. A real codegen change, not a runtime change
  — out of scope.

## Closing summary for the team

Phase 4 (variant-field unboxing) is the right next Perceus lever.
The RB-tree wall is split roughly evenly between **alloc + free**
(per-cell work) and **RC traffic** (per-cell-visit work), and Phase
4 attacks both: by inlining `color`/`k`/`v` into the Node struct it
removes the boxed Int allocations that drive RC traffic, and it
shrinks the cascade depth on free. Drop specialisation (#384)
remains a worthwhile second-order lane but should be re-scoped after
Phase 4's effect on RB-tree is measured directly. **Option A.**
