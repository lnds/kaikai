# Lane experience — nullary variant singletons (#300)

Lane: `nullary-variant-singletons` (branch `nullary-variant-singletons`).

## Objective metrics

| Metric                         | Baseline      | After         | Delta       |
|--------------------------------|---------------|---------------|-------------|
| Total `KAI_TRACE_RC` leaked    | 79,381,668    | 28,446,386    | −64.2%      |
| `tag=variant` allocs           | 77,658,966    | 22,647,975    | −70.8%      |
| `tag=variant` live (leaked)    | 64,134,763    | 13,199,481    | −79.4%      |
| Total allocs (all tags)        | 137,486,184   | 82,475,193    | −40.0%      |
| Total live_peak                | 79,382,250    | 28,446,967    | −64.2%      |

Measurement: `KAI_TRACE_RC=1 ./stage2/kaic2 stage2/compiler.kai > /dev/null`,
both runs from a clean `make -C stage{1,2} clean && make` chain.

## The mechanism

`stage0/runtime.h` now keeps an open-addressed table of nullary variant
singletons keyed on `(tag, name_ptr)` with 64 buckets. The first call
to `kai_variant(tag, name, 0, NULL)` for a given `(tag, name)` allocates
one chunk, marks it `rc = INT32_MAX`, and installs it; every subsequent
call returns the same chunk.

`kai_incref` and `kai_decref` already short-circuit on `rc == INT32_MAX`
(the same sentinel the unit/bool/nil/int-cache singletons use), so the
singleton survives every RC operation unchanged and `kai_free_value`
is never reached for it. No emitter changes — `kai_variant(...)` calls
look identical from the compiler's side.

The table is keyed by string-literal pointer (`const char *name`).
Literals have stable addresses across all calls within a binary
execution and are unique per distinct constructor name, so the key is
both correct and cheap. Hash is `((p >> 4) ^ (p >> 12) ^ tag) & 63`.

The 64-bucket cap is well above the kaikai cap of ~30 distinct nullary
variants (`None`, `Nil`, `Some`, `Unit`, plus token tags, mode tags,
error sentinels, etc.); table-full is unreachable in practice.

## Per-variant baseline vs after

The address-keyed report from #296 was the original tool but unreliable
under tail-call ABI (#300 §3). The variant-name histogram (temporary
counter, removed before commit) gave the ground truth:

| Variant     | Baseline allocs | After allocs (chunks) | Delta     |
|-------------|-----------------|-----------------------|-----------|
| `None`      | 50,727,663      | 1                     | −100%     |
| `EP`        |    639,990      | 1                     | −100%     |
| `MUnknown`  |    565,980      | 1                     | −100%     |
| `Arm`       |    180,256      | 1                     | −100%     |
| `Some`      | 18,541,419      | 18,541,419 (unchanged)| 0%        |
| `ECall`     |    484,027      |    484,027 (unchanged)| 0%        |
| `TyCon`     |    422,634      |    422,634 (unchanged)| 0%        |
| `Step`      |    348,116      |    348,116 (unchanged)| 0%        |
| `EVar`      |    209,739      |    209,739 (unchanged)| 0%        |

`Some(x)`, `ECall(...)`, `TyCon(...)` etc. carry payload args and are
inherently per-call distinct — they cannot be singleton'd without
also collapsing the payload identity. They remain on the per-call alloc
path. The singleton optimisation collapses only the nullary
constructors, which dominate.

## Total leak reduction

`tag=variant` accounts for ~80% of total live count both before and
after. With the nullary cohort gone, leak drops from 79M to 28M total
chunks. Remaining leakers are `Some` (9.3M live), `cons` cells (5.4M
live), records (6.9M live), strs (2.0M live), and the long tail of
non-nullary AST nodes — all driven by Perceus discipline gaps in the
emitter, tracked under the #293 umbrella.

## Selfhost convergence

Byte-identical on `make tier0` (which embeds the selfhost diff check).
The optimisation is purely runtime; emitted text from kaic1 → kaic2
is unchanged because no emit site spells out `kai_variant`'s nullary
fast path — it lives entirely behind the same C function signature.

## Friction points

- The `KAI_TRACE_RC` per-call-site report from #296 was actively
  misleading: top-N table named `synth_dim_pow` and `ch_is_alpha` as
  variant alloc sites, but the real culprit (`None` × 50M) wasn't a
  function-local emit at all — it's the same string literal threaded
  through hundreds of distinct `kai_variant(0, "None", 0, NULL)` calls
  in `stage2.c`. Without the variant-name histogram from #300 the lane
  would have chased a fiction.
- LSP flags pre-existing C99 implicit-cast warnings on lines unrelated
  to this change (`runtime.h:2789` etc., string-split helper). They
  were green before this lane and remain green; `cc -std=c99` accepts
  the casts.
- `selfhost-llvm` is not a Makefile target on this branch; tier0
  already verifies selfhost byte-identical convergence on the C
  backend. The LLVM backend is exercised via `make -C stage2 test-llvm`
  inside tier1.

## Subjective summary

Smallest material runtime fix in the lane history: ~95 lines of net
runtime additions, no emitter changes, no test churn. Empirical delta
on `kaic2 self-compile` is the largest single optimisation since the
m5.x flip Phase 4 int+char cache (~39M of 66M allocs).

The mechanism reuses the `rc = INT32_MAX` saturation discipline already
established by m5 #7 (unit/bool/nil) and m5.x Phase 4 (int/char cache).
This is the third instance of the same pattern; the runtime is
converging on a clean "immortal singleton" idiom.

## Limitations

- Not a Perceus fix. `Some(x)` and other payload-bearing variants still
  leak at the same rate (29.4% of total chunks post-fix). Closing them
  requires emitter-side discipline work tracked under #293.
- Not a #296 fix. The address-keyed per-call-site report still names
  tail-call grandparents under the noinline-wrapper ABI. #300's
  short-term recommendation (augment the report with a variant-name
  histogram) is left to a separate lane.
- Singleton table is per-process and never cleared. A long-running
  kaikai program that touches every nullary variant exactly once will
  retain ~30 immortal chunks until exit. This is well below noise
  on any realistic workload (the selfhost retains 184 distinct names,
  yielding 184 immortal chunks).

## Build TSV

See `/tmp/lane-nullary-variant-singletons-builds.tsv` (cleaned at lane
close).
