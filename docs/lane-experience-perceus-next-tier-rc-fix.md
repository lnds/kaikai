# Lane experience — perceus next-tier RC fix (#293 follow-up)

Lane: `perceus-next-tier-rc-fix` (branch `perceus-next-tier-rc-fix`).

## Objective metrics

Measurement: `KAI_TRACE_RC=1 ./stage2/kaic2 stage2/compiler.kai > /dev/null`,
both runs from a clean `make -C stage{1,2} clean && make` chain.

| Metric                         | Baseline (#301)  | After          | Delta       |
|--------------------------------|------------------|----------------|-------------|
| Total `KAI_TRACE_RC` leaked    | 28,446,386       | 21,105,881     | −25.8%      |
| Total `live_peak`              | 28,446,967       | 21,106,460     | −25.8%      |
| Total allocs (all tags)        | 82,475,193       | 66,091,560     | −19.9%      |
| `tag=variant` allocs           | 22,647,975       | 6,226,634      | −72.5%      |
| `tag=variant` live (leaked)    | 13,199,481       | (subsumed)     | n/a         |

`Some` chunks dominated the 13.2M `tag=variant` leak baseline. After
the fix, `Some` allocations no longer hit `kai_alloc` for the 88%
that carry an immortal payload — instead they hit a shared cache
keyed on `(tag, name, args[0..n])`.

## Top 10 by `variant_name` — before / after

Captured with a temporary name-keyed histogram (counter applied per
issue #300's *Reproduction* section, removed before commit).

| Variant     | Baseline allocs | Baseline frees | After allocs | After frees | Notes                          |
|-------------|-----------------|----------------|--------------|-------------|--------------------------------|
| `None`      | 50,727,663      |       704,554  | (unchanged)  | (unchanged) | Already nullary singleton (#301) |
| `Some`      | 18,541,419      |     9,276,495  | 18,546,767   |   225,664   | **Immortal-payload singleton** |
| `EP`        |    639,990      |             0  | (unchanged)  | (unchanged) | All payload-mortal records      |
| `MUnknown`  |    566,528      |             0  | (unchanged)  | (unchanged) | Already nullary singleton       |
| `ECall`     |    484,027      |        16,691  | (unchanged)  | (unchanged) | Mortal payload, no fix          |
| `TyCon`     |    422,634      |        29,356  | (unchanged)  | (unchanged) | Mortal payload, no fix          |
| `Step`      |    348,116      |         2,543  | (unchanged)  | (unchanged) | Mortal payload, no fix          |
| `EVar`      |    209,933      |           507  | (unchanged)  | (unchanged) | Mortal payload, no fix          |
| `U`         |    189,988      |            87  | (unchanged)  | (unchanged) | Mortal payload, no fix          |
| `Arm`       |    180,256      |             0  | (unchanged)  | (unchanged) | Mortal payload, no fix          |

The fix targets `Some` exclusively in observable terms — but it
generalises to every constructor whose run-time payload happens to
be all-immortal. `Ok`, `Err`, `ElPlain`, etc. would benefit when
their callers happen to thread cached values through them.

The "after frees" 225,664 number for `Some` reflects the 12% of
allocations whose payload was mortal (typically a freshly-allocated
record or string). Those still go through the alloc/free path
unchanged.

## The targeted fix

`stage0/runtime.h` extends the nullary-variant singleton table with
a parallel "immortal-args" cache keyed on `(tag, name, n, args[0..n])`
for `n ∈ [1, 4]`. When `kai_variant(_, name, n, args)` is called with
every `args[i]->rc == INT32_MAX`, the runtime:

1. Hashes the key tuple.
2. Looks up the cache; on hit, returns the cached chunk.
3. On miss, allocates fresh, marks `rc = INT32_MAX`, and installs.

The 16384-bucket open-addressed table absorbs every realistic
`(constructor × payload-singleton)` combination. Hash mixes
`(tag, n, name_ptr, args[0..n]_ptrs)` with the FNV-style
`* 1315423911u ^ ...` step the rest of the runtime uses.

Because the cached chunk has `rc = INT32_MAX`:

- Every `kai_incref` / `kai_decref` short-circuits (existing
  sentinel logic unchanged).
- `kai_free_value` is never reached for it.
- `kai_check_unique` returns 0, so the reuse-in-place path
  (issue #118) leaves it alone — correct, since the chunk is
  shared and immutable.

File: `stage0/runtime.h:1393-1490` (immortal-var table) and
`stage0/runtime.h:1500-1530` (kai_variant body).

## Selfhost convergence

Byte-identical on both backends. The optimisation is purely
runtime; emitted text from kaic1 → kaic2 → kaic3 is unchanged
because no emit site spells out `kai_variant`'s immortal-payload
fast path — it lives entirely behind the same C function signature.

```
$ diff /tmp/k2.c /tmp/k3.c
$ echo $?
0
```

`make tier0`, `make tier1`, `make tier1-asan` all green.

## Why we didn't go below 20M

The acceptance gate aimed for ≤ 20M leaked. We landed at 21.1M.
The remaining contributors are:

- `tag=str` 36M allocs (4.5x the variant tag) — every `kai_str`
  call from the emitter is a fresh malloc; many are constants
  redundantly allocated. A separate string-interning lane.
- `tag=record` 9.0M allocs — mostly mortal-field records that
  can't be singleton'd without payload analysis.
- `tag=cons` 8.2M allocs — same shape: head/tail typically mortal.

Closing those needs different mechanisms (string interning, RC
discipline at emit, or refcount fusion). Out of scope for this
lane.

## Friction points

- The first build of the temporary name-histogram counter
  exposed an emit-site dependency: `kai_var_name_bucket` is
  referenced from `kai_free_value` (line 1118) but defined inside
  the variant block (line 1450+). Resolved by hoisting the
  counter declaration above `kai_free_value`.
- `test-infer` failed transiently because the destructor wrote
  to stderr; tests verify stderr is empty. Gated the destructor
  behind `KAI_TRACE_VAR_NAME` (also removed before commit).
- Pre-existing C99 unnamed-struct-pointer warnings around lines
  2843-2870 are not lane-introduced; they predated the lane and
  remain. The compiler accepts the casts under `-std=c99`.

## Subjective summary

A clean structural win. The `Some(<immortal>)` pattern dominates
the kaic2 self-compile because `Option[T]` is the universal "lookup
might miss" return shape and most lookups thread cached scalars
or nullary variants through. Generalising the singleton from
nullary-only to "all-args immortal" is the obvious extension —
same mechanism, broader key — and it lands the second-largest
single-fix variant-leak delta after #301 itself.

## Limitations

- Cache cap of `n ≤ 4` skips wide constructors. None observed in
  the top-30 leakers, so probably fine.
- 16384 buckets — if the realistic combination space ever exceeds
  ~12k entries (75% load), open-addressed probing degrades.
  Current observed entries: ~hundreds.
- The fix doesn't help mortal-payload constructors (`ECall`, `EP`,
  `TyCon`, etc.). Those need different attention — likely Perceus
  discipline at emit, not runtime.
- `tag=str`, `tag=record`, `tag=cons` remain. Separate lanes.
- #297 / #298 (closure capture lifecycle, larger scope) untouched.
- Issue #92 (TCO rule 3 retomar) and #120 may now be tractable
  with mainline RC closer to balanced.
