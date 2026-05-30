# Lane experience — perceus-int-cache (Phase 1.A: widen small-int cache)

**Date:** 2026-05-29
**Branch:** `perceus-int-cache`
**Scope tier:** runtime-only perf lane (3 `#define` + warm strategy)

## Scope as planned vs as shipped

**Planned (brief, Phase 1.A only):** widen the small-int cache range
from `[-128, 127]` / 256 entries to `[-65536, 65535]` / 131072 entries
in `stage0/runtime.h`. Runtime-only, no ABI change, no emitter change,
selfhost byte-identical. Explicit out-of-scope: slot-extract raw
(Phase 1.B), tagged pointers, emitter edits.

**Shipped:** exactly that range change, PLUS a warm-strategy change the
brief invited ("decide and justify the warm"). The original cache
warmed all entries eagerly on the first in-range `kai_int`. At 256
entries that was free; at 131072 entries × `sizeof(KaiValue)` = 48 B =
6 MB, the eager warm became a 6 MB RSS hit on the FIRST in-range int —
paid even by a hello world. Measured: hello RSS jumped 1.54 MB → 6.78 MB
under eager warm. So Phase 1.A also switched the int cache to
**per-entry lazy warm**: a cold `.bss` slot has `rc == 0 != INT32_MAX`,
so `kai_int` warms each slot the first time its exact value is asked for.
RSS then scales with the program's working set of small ints, not with
the cache range. Hello RSS with lazy warm: 1.50 MB (+32 KB over the
1.54 MB baseline, vs +5.24 MB under eager warm).

## The measured result (the point of the lane)

Reproduced the `bump` benchmark from
`docs/benchmarks/perceus_promise_2026-05-29.md` (50k-element list,
1000 passes, threaded linearly so reuse-in-place fires at 100%):

| metric | baseline `[-128,127]` | widened `[-65536,65535]` |
|---|---:|---:|
| `tag int` allocs | 50,092,490 | **0** |
| `alloc_total` | 50,142,495 | 50,005 |
| `reuse_in_place` | 50,000,000 | 50,000,000 |
| wall (median of 5, -O2) | 1.12 s | **0.56 s** |
| leaked | 50,002 | 3 |
| hello RSS | 1.54 MB | 1.50 MB (lazy warm) |

The cache does exactly what was predicted for the alloc: the 50M int
boxes (one per `h + 1`, all values 0..51000 land in the new range)
collapse to zero. Reuse-in-place stays 100%.

## Design decisions and alternatives considered

1. **Range = `[-65536, 65535]` (asu's proposal), kept.** The `bump`
   bench mints values up to ~51000, so the range must be ≥ 51000 for
   that workload to hit zero int allocs. A `[-32768, 32767]` half-size
   table (3 MB) would have left the bench's high half allocating. 64Ki
   covers loop indices, list lengths, AST tag ints, and modest compute.

2. **Per-entry lazy warm over eager warm or eager `.bss` static init.**
   Eager warm: simple but 6 MB RSS on first int. Static C initializer
   for all 131072 entries: bloats the binary's data section and the
   `.c`/`.ll` text — would have broken byte-identical selfhost. Per-entry
   lazy: one extra predictable branch per in-range `kai_int`, no emitted-
   text change, RSS proportional to working set. CPython warms its
   `-5..256` cache eagerly; we warm lazily precisely because our range
   (and 48 B entry size) is far larger.

3. **Why the cache, not tagged pointers.** Tagged-pointer / value-
   immediate scalars (Phase 1.B) are the real end state — they kill the
   `kai_int` AND the incref/decref traffic on the scalar. But they touch
   the int representation across the hot path + both backends + selfhost
   (a multi-day lane). The cache delivers the alloc win for the measured
   typical case with a 3-line range change + a warm tweak, runtime-only,
   selfhost byte-identical. It is the cheap, honest first step, not the
   destination.

## Structural surprises the brief did not anticipate

- **The wall gate (≤0.10 s) was an over-prediction and was retired.**
  The brief's gate assumed the int boxing was the ENTIRE 28× gap. It is
  not. With the alloc eliminated, wall fell to 0.56 s (~13× C), not
  ≤0.10 s. The residual is (a) RC traffic — 50M reuse-in-place ops each
  incref/decref the pinned head int + store, and (b) the control-flow
  overhead of `bump`'s NON-tail recursion (depth 50k × 1000 calls).
  Neither is touched by widening the cache. Consulted asu, who confirmed
  the over-prediction (precedent: CPython's small-int cache eliminates
  the int alloc but never the INCREF/DECREF traffic — exactly this
  situation) and that the honest Phase 1.A gate is "int allocs of the
  typical case → ~0" + selfhost byte-identical, NOT a wall number. The
  RC-on-scalar residual is precisely what Phase 1.B is designed to kill.

- **`sizeof(KaiValue)` is 48 B, not the ~32 B napkin estimate** (the
  union's `rec` substructure of 4 pointers + padding dominates). 131072
  × 48 = 6.00 MB exactly. This is what made the eager-warm RSS hit large
  enough to force the lazy-warm decision.

## Fixtures added and coverage gaps

- `examples/perceus/int_cache_reuse_bump.kai` (+ `.out.expected`) — the
  canonical reuse-in-place `bump` shape at N=10k (moderate; `bump` is
  intentionally non-tail so 1M would overflow the C stack). Golden is
  the functional head value; under `-DKAI_TRACE_RC=1` it shows int
  allocs=0, cons reuse_in_place = passes×length.
- Wired into `stage2/Makefile` as `test-perceus-int-cache` (in the
  `test:` aggregator) and `test-perceus-int-cache-asan` (in the root
  `Makefile`'s `tier1-asan` target). The ASAN sibling guards the pinned-
  slot `rc=INT32_MAX` contract: a regression that frees a pinned slot or
  indexes out of bounds trips the sanitizer.
- **Gap:** the fixture asserts the functional output, not the alloc
  count directly (the trace profile is documented here + in the honesty
  targets, not machine-checked in CI). A trace-count assertion would be
  stronger but the existing perceus fixtures follow the functional-golden
  convention, so this matches precedent.

## Cost vs estimate

Roughly as estimated for the core change (3 `#define`). The
unanticipated work was the RSS measurement → lazy-warm pivot (the brief
asked to measure RSS and decide the warm; the measurement made the
decision non-optional). Two full selfhost + selfhost-llvm + tier0 cycles
(~5 min each) dominated wall-clock — once for eager warm, once for the
lazy-warm final.

## Gates (all green)

- int allocs: 50M → 0 (gate: <100k). ✅
- leaked: 3 (gate: ~2). ✅
- wall: 1.12 s → 0.56 s, −50% (wall ≤0.10 s gate retired per asu; see
  surprises). ✅ for the honest gate.
- `make selfhost`: byte-identical (kaic1b.c==kaic1c.c, kaic2b.c==kaic2c.c). ✅
- `make -C stage2 selfhost-llvm`: byte-identical (s1.ll==s2.ll). ✅
- `make tier0`: green (selfhost + demos 34/34). ✅
- hello RSS: 1.50 MB vs 1.54 MB baseline (+32 KB, lazy warm). ✅
- new fixture + ASAN sibling: pass. ✅

## Follow-ups left for next lanes

- **Phase 1.B — tagged pointers / raw-slot extract.** The real lever for
  full scalar Perceus: kills both the `kai_int` box AND the incref/decref
  traffic on the scalar across the reuse boundary. Touches the int
  representation + both backends + selfhost. This is where the residual
  0.56 s wall (RC-on-pinned-scalar half) goes to die.
- **Non-tail `bump` recursion overhead** is structural (C call frames),
  out of scope for any marshalling phase; not a follow-up for this track.
- The rb-tree's 14.78× C only moves once BOTH land: reuse-in-place for
  its mask-N variants (the LLVM typed-slot lane) AND Phase 1.B unboxed
  ints surviving the reuse boundary.
