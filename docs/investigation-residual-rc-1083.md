# Residual RC-traffic investigation (#1083)

**Status:** diagnostic spike — read-only over `stage0`/`stage2`, nothing
implemented. Base is `main` with Fix A merged (PR #1088, target-features
stamping), so the native codegen defect is already closed. This spike targets
the *residual* the codegen fix left behind: the incref/decref pairs Perceus does
not elide.

## The headline finding

The residual RC traffic is **diffuse in wall-clock, not dominated by one lever.**
The single largest *countable* source — the reuse-pair, ~47% of all incref/decref
— is **sound-elidible but nearly free in wall** (verified: eliding it cuts RC ops
in half and instructions by 4.9%, and moves wall by ~0%). The genuine
kaikai-vs-C wall gap on this workload is structural/algorithmic, not the RC
op count. This is a valid result: there is no clean "close this and the residual
collapses" lever on wall, and the report says so rather than manufacturing one.

There *is* a real, low-risk cleanup worth doing — eliding the reuse-pair — but its
justification is instruction count and RC-counter hygiene (and a likely larger
payoff on allocators that price RC ops higher, or on RC-heavier workloads), **not**
a measurable rb-tree wall win.

## The fact under investigation

rb-tree 1M, Okasaki/Lean4/Koka functional insert (byte-identical algorithm to the
Koka reference), Darwin arm64, `cc -O2`, warm interleaved medians:

| | alloc | reuse_in_place | incref = decref | decref/alloc | wall | instr |
|---|---|---|---|---|---|---|
| C-hand (imperative, parent-ptr, in-place) | 6,301,558 | — | 0 / 0 | 0 | 0.275 s | 454.6 M |
| kaikai-c | 6,301,558 | 6,301,528 | 13,317,720 | **2.11×** | 0.357 s (1.30×) | 2492.0 M (5.48×) |
| kaikai-native (post Fix A) | 6,301,552 | 6,301,528 | 13,317,720 | 2.11× | 0.636 s (2.31×) | — |

Two anchors:

- **Allocation is at parity** (6,301,558 both; closed in #1053). The residual is
  not alloc count.
- **RC traffic is backend-independent.** Native emits the *identical*
  13,317,720 incref/decref count — it is a front-end/Perceus property, so any RC
  lever improves both backends. (Native's per-op cost is separate and was the
  Fix-A codegen defect.)

## Where the 13.3 M incref come from (measured, dynamic, exact)

Method: the emitted C is a temporary artefact (not `stage0`/`stage2` source), so
it can be instrumented freely. Per-site counters + a renamed-wrapper technique
give exact dynamic op counts; `KAI_TRACE_RC` gives the totals; a benchmark
variant with the read-only traversals removed isolates their share.

| source | incref | % of total | note |
|---|---|---|---|
| **reuse-pair** (incref reused cell) — balance | 5,301,528 | 39.8% | paired w/ match-exit decref, aliased |
| **reuse-pair** (incref reused cell) — rb_insert | 1,000,000 | 7.5% | same pattern |
| **balance dup(r)** — retained passed-in child | 5,301,528 | 39.8% | 1 per balance call, semantically required |
| **rb_size + rb_height** read-only borrow | 2,000,006 | 15.0% | 100% elidable in principle |

(The reuse-pair and balance-dup(r) rows each equal the number of balance calls,
5,301,528, plus the 1 M top-level inserts — the counts are exact, not estimated.)

### 1. The reuse-pair (dominant, 47% — incref-then-decref on the SAME cell)

Every `match`-on-an-owned-value that reuses its cell emits a pair on the *same*
reused pointer:

- `_r = kai_incref(_scr)` at the end of the reuse arm — the reuse helper increfs
  the rewritten cell so it survives the exit drop;
- `kai_decref(_scr)` at the match exit — the generic scrutinee drop that
  `emit_match_default` always emits.

Because reuse-in-place makes the result cell == the scrutinee cell, these are
incref-then-decref on one pointer → **net zero**. Perceus emits them as
independent obligations: the exit drop is static (every match has it), the incref
is added by the runtime reuse helper only after its dynamic `rc==1` check.

This is an **artefact of design, not correctness.** `emit_c.kai:2197-2199`
confesses it: `__pcs_scr` lowers to the match emitter's `_scr`
*"sidestepping the need to teach `emit_match_default` about reuse-aware decref"*.
The incref exists only to survive a decref the compiler itself emits. Sites:
`runtime.h` reuse helpers (the `return kai_incref(_scr)` inside the
`kai_check_unique` branch) + `emit_c.kai:2192-2260` (lowering) +
`perceus.kai:1588-1671` (the recogniser, where a drop-consume fix would live).

**Verified sound-elidible.** Eliding it in the emitted C — in the reuse branch
only, `_r = kai_incref(_scr)` → `_r = _scr; _scr = NULL` so the exit
`kai_decref(NULL)` no-ops — produces **byte-identical output** (`size 1000000`,
`height 29`), unchanged `leaked` (no new leaks, no double-free), and:

| | incref = decref | decref/alloc | instr | wall (median) |
|---|---|---|---|---|
| baseline kaikai-c | 13,317,720 | 2.11× | 2492.0 M | 0.357 s |
| reuse-pair elided | 7,016,192 | **1.11×** | 2369.9 M (−4.9%) | **0.365 s (no change)** |

−6,301,528 incref/decref exactly (= number of reused nodes). **The wall does not
move.** An incref+decref on a cell that was just rewritten in place is hot in
cache — a load/add/store/branch the superscalar CPU absorbs almost for free. It
dominates the *count* and ~5% of *instructions*, but ~0% of *wall*.

### 2. balance dup(r) (40%) — semantically required, NOT a redundant pair

Each `balance_left`/`balance_right` call (5,301,528 total; 88% the non-rotating
recolor arm) does exactly one `kai_internal_dup(kai_r)`: `r` is a borrow of the
insert's scrutinee (the child *not* on the descent path), passed into balance as a
parameter and retained in the result tree, so it needs an owned ref. Probing
removal (drop the dup) **corrupts the tree** (`panic: non-exhaustive match`,
incref collapses to 75) — confirming the dup is required under the current RC
model. Removing it is not an elision; it needs a model change (move-into-
consuming-slot), the #1054/#1069/#1074 bug class.

### 3. rb_size + rb_height read-only borrow (15%) — elidable but cheap

`rb_size(t) = match t { RBNode(_, l, _, _, r) -> 1 + rb_size(l) + rb_size(r) }`.
The arm binds `l`,`r` and passes them to a recursive call that returns `Int` and
never retains them — a pure read-only traversal. The emitted C does
`incref(l); incref(r); …; decref(_scr)` per node. Perceus's borrow pre-pass
(`pcs_borrow_params`, perceus.kai:615-644) is **local**: it marks a param borrowed
only when the match arms *bind nothing* (covers `is_red`; misses `rb_size`, whose
arm binds children). There is no transitive fixpoint ("binder passed only to
non-consuming slots"). The consume-map (`pcs_build_consume_map`, perceus.kai:646+)
already computes the dual bit, so the infrastructure exists.

Removing the two traversals cuts 2.0 M incref (measured: full 13.32 M →
insert-only 11.32 M). Wall impact: full ~0.45 s vs insert-only ~0.40 s ≈ 11% of
wall for 15% of ops — cheaper-than-average ops (sequential reads, Int math), so
op-count share overstates wall share here too.

## Why the wall gap survives every RC lever

Even with the **full** reuse-pair elided, kaikai-c is still **5.2× C-hand in
instructions** and ~1.3× in wall. The instruction gap is dominated by non-RC
work, and C-hand is not even the same algorithm:

- C-hand is **imperative with parent-pointers** (`struct rb_node { …; *left,
  *right, *parent; }`), rotating in place with zero spine reconstruction.
- kaikai is **functional Okasaki modulo-cons**: each node's 5 slots are rewritten
  per descent level (even with reuse), plus `kai_check_unique` + an rc-branch per
  reuse, over a boxed `KaiValue` (tag+rc+union header) vs C's flat struct.

So a chunk of the 1.30× kaikai-c-vs-C-hand wall is **algorithmic + layout**, not
RC-op count. RC-op elision (reuse-pair, borrow) reduces the *count* and
*instructions* but barely touches wall because those ops are cache-hot and the
CPU pipelines them. The honest picture: **the residual is diffuse; no single RC
lever is a wall win on rb-tree.**

## Repair plan (impact × 1/risk order; NOTHING implemented)

Ranked by asu design review (validated against the measurements above). Note the
tension: ranked by *soundness+cleanliness* the reuse-pair is the clear
quick-win; ranked by *measured rb-tree wall* nothing here is a win. Both are
stated so the integrator authorizes with eyes open.

### Lever 1 (recommended quick-win) — elide the reuse-pair via drop-consume

Make the reuse recogniser mark the scrutinee **drop-consumed** by the donation
(Koka drop-guided reuse, Perceus PLDI'21): don't emit the match-exit
`kai_decref(_scr)` and don't incref the reused cell, in the unique branch. This
removes the entire class, not one case. The alternative — a runtime-local patch
that skips the incref and nulls `_scr` — is the same effect but per-helper; the
frontend drop-consume is cleaner and subsumes the rb_insert 1 M and balance 5.3 M
in one place. Locus: `perceus.kai:1588-1671` (recogniser) +
`emit_c.kai:2192-2260` / `emit_match_default` (teach it reuse-aware decref, the
sidestep the confession admits was skipped).

- **Impact:** −6.3 M incref + −6.3 M decref (−47% RC traffic, decref/alloc
  2.11×→1.11×); −4.9% instructions. **rb-tree wall: ~0% (measured).** Likely a
  real win on RC-heavier workloads and on allocators that price RC ops higher;
  a real instruction-count and RC-counter-hygiene win regardless.
- **Risk: LOW-MED.** LOW in bug class — it is rc-neutral *by definition* (the
  pair nets zero on the unique branch; the shared branch, where the decref is
  genuinely needed, is untouched). It does **not** touch the borrow-analysis or
  the reuse-donation ownership decision (the #1054/#1069/#1074 family): all
  ownership is decided upstream; this lives downstream on a cell already proven
  unique and already rewritten in place. It is "I already moved, drop the
  ceremony incref+decref", not "can I move without incref?". The `-MED` is purely
  mechanical: two coordinated sites (runtime helper + match-exit emitter), both in
  the unique path. **If done wrong the failure is a leak, not a UAF** — the safe
  direction, caught by `KAI_TRACE_RC` imbalance + ASAN-on-selfhost.
- **Gate:** selfhost byte-id (emitted program changes), serial backend parity
  (`BACKEND_PARITY_JOBS=1`; parallel false-greens per memory), ASAN-on-selfhost
  (#1042), `KAI_TRACE_RC` balanced (incref==decref, leaked unchanged), + a fixture
  asserting the reuse-pair count drops.

### Lever 2 — transitive borrow-elision for read-only traversals

Extend `pcs_borrow_params` with a fixpoint: mark a param borrowed when its
child-binders flow only into non-consuming slots (the consume-map already
computes non-consuming positions). Generalises beyond rb_size to any read-only
fold/aggregate.

- **Impact:** −2.0 M incref on rb-tree (−15% RC ops, ~11% wall on the size/height
  phase). Broader than rb-tree: every read-only recursive traversal today pays
  dup/drop per node.
- **Risk: MED.** More than the reuse-pair because it changes an *ownership*
  classification (owned→borrowed). The danger asu flags (perceus.kai:632-636) is
  escape of a *child* via a path the local rule misses — a binder that reaches a
  closure, an effect op, or aliasing the fixpoint doesn't model. Conservative
  toward owned is safe (misses an optimisation); conservative toward borrowed is a
  UAF. The fixpoint must be provably escape-free, and it is a leak-if-too-borrowed
  / UAF-if-too-owned axis — so it needs the same ASAN-selfhost gate. Still
  bounded and separable from Lever 3.

### Lever 3 — reduce balance dup(r) (the big, dangerous one)

Let balance receive `r` in a slot Perceus knows is move-only (the insert no longer
uses `r` after passing it), avoiding the dup. This is the **#1054/#1069/#1074 bug
class directly**: move-without-incref assuming a unique consumer, exactly the
reuse-donation soundness boundary (perceus.kai:1718-1723). Probe-removing the dup
corrupts the tree, confirming it is load-bearing under today's model.

- **Impact:** up to −5.3 M incref (40%). The largest *count* lever after the
  reuse-pair — but see the wall caveat: RC-op elision has not moved rb-tree wall
  in any probe here.
- **Risk: HIGH.** Same soundness class as the shipped UAFs; reopens double-frees
  if the move-analysis is wrong. Not to be attempted without the full
  ASAN-on-selfhost + serial-parity discipline, and only after Lever 1 (which is
  orthogonal and safe).

### Not the lever (reordered from the earlier issue verdict)

- **Drop-specialization** was the earlier #1083 verdict's headline lever. It drops
  in priority: **the teardown barely counts on this workload.** The final tree's
  children are all unique, so `kai_free_value` routes them through the fused
  spine-free (#1084) which does *not* count decref; the counters confirm the
  teardown contributes ≈0 to the 13.3 M. Drop-spec would help teardown-heavy /
  shared-child workloads, not rb-tree.
- **Flat-layout** (#1053 typed-slot already inlines primitives) — downstream;
  helps the structural instruction gap, not the RC pairs.
- **TRMC-unroll** — not sound without drop-spec first, and not the lever
  (unchanged from the issue verdict).

## Method notes (rigor)

- All wall measured warm + interleaved, explicit backend, medians over 7–11 runs;
  surprises re-measured (the reuse-pair no-wall-change result was re-run with an
  instruction proxy — `/usr/bin/time -l` instructions-retired — and 11-round
  medians before being reported).
- RC op attribution is *dynamic exact counts*, not static symbol counts (the
  #1053 lesson: static objdump ≠ runtime). Instrumentation lives in the emitted C
  (a temporary artefact), never in `stage0`/`stage2`.
- The reuse-pair soundness claim is backed by an actual elision probe verified
  byte-identical + leak-balanced, not by reasoning alone.
- Base confirmed to carry Fix A: native measures 2.31× C (not the pre-#1088
  3.84×).
