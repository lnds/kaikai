# Lane retro: validate residual leak hypotheses for #297 / #298

**Date**: 2026-05-07
**HEAD pin**: `75188ba` (`Merge pull request #320 from lnds/pipeline-reorder-perceus-before-tcrec`)
**VERSION pin**: `0.44.0`
**Lane branch**: `validate-rc-residual-297-298`
**Workload**: `./stage2/kaic2 stage2/compiler.kai > /dev/null`

## TL;DR

| Issue | Recommendation | Threshold crossed |
|---|---|---|
| **#298 closure capture lifecycle** | **KEEP OPEN** | Closure leak rate **27.97%** ≫ 10% (issue threshold for KEEP). Identical to pre-2026-05-06→07 baseline. The night's RC fixes did not touch closures. |
| **#297 perceus_pass last-use on pass-through** | **KEEP OPEN, REFRAMED** | Top-5 variant names by physical leak account for **66.1%** of total variant leak (≫ 30% threshold for CLOSE). No single variant dominates beyond 40%. record leak rate is **76.2%** (≫ 30%). The umbrella "pass-through last-use" hypothesis still fits the data, but the spec needs new acceptance gates against post-singleton numbers. |

Neither issue should be closed. Both still have a clear empirical signal.

## Method

Patched `stage0/runtime.h` with a `KAI_TRACE_VAR_NAMES`-gated histogram that:

1. Counts every `kai_variant(_, name, _, _)` **invocation** (invocations include singleton/immortal-cache hits).
2. Counts every **physical** allocation (the call paths that miss the singleton/immortal cache and reach `kai_alloc(KAI_VARIANT)`).
3. Counts every `kai_free_value` on a variant chunk by `variant_name` pointer.

Keyed on the string-literal pointer (`(uintptr_t) name`), which is stable across runs and immune to the tail-call ABI bug that contaminated #296's address-keyed attribution (see #300 for the postmortem).

Build: `make -C stage{1,2} clean && CFLAGS="... -DKAI_TRACE_VAR_NAMES=1" make -C stage{1,2} kaic{1,2}`. The patch is opt-in — vanilla builds compile to empty hooks and emit byte-identical IR.

For the closure column, captured `KAI_TRACE_RC=1` per-tag stats from the same workload.

## Selfhost integrity

Vanilla build (without `-DKAI_TRACE_VAR_NAMES`):

```
$ make -C stage2 selfhost
self-hosting fixed point: OK
```

`stage2.c` byte-identical between `kaic2` and `kaic2'` (the self-host of `kaic2`). The patch's default-off path does not change emitted IR.

## Variant histogram — top 30 by physical leak

```
[VAR_NAME] total distinct=184 invocations=78,729,263 real_allocs=6,243,198 frees=373,879 leak=5,869,319

  name                  invocations    real_allocs    frees       leak
  Some                  19,110,714     2,244,945      216,346     2,028,599
  EP                       642,420       642,420            0       642,420
  ECall                    486,846       486,846       16,777       470,069
  TyCon                    424,474       424,458       29,494       394,964
  Step                     350,900       350,900        2,543       348,357
  EVar                     211,775       211,775          345       211,430
  U                        190,427       190,427           88       190,339
  Arm                      181,222       181,222            0       181,222
  EBlock                   154,074       154,054            1       154,053
  EField                   115,696       115,696        5,232       110,464
  ElPlain                  112,102       112,102        5,099       107,003
  MBoxed                   100,660       100,660            0       100,660
  SLet                      98,881        98,881            0        98,881
  TyListT                  142,957        99,408        1,337        98,071
  TyFnT                     91,496        91,496       16,296        75,200
  EMatch                    70,950        70,950            0        70,950
  FI                        60,170        60,170        2,735        57,435
  EBinop                    52,152        52,152        2,370        49,782
  DFn                       51,587        51,587        2,542        49,045
  TyName                    44,229        43,962        1,506        42,456
  EIf                       44,233        44,233        1,984        42,249
  EList                     51,792        33,995        1,549        32,446
  FT                        28,368        28,368            0        28,368
  SExprStmt                 25,673        25,673            0        25,673
  IpLit                     22,644        22,644            0        22,644
  TyList                    21,431        21,431            0        21,431
  Scheme                    22,449        19,839            0        19,839
  ERecordLit                19,998        19,998          909        19,089
  EStr                      17,059        17,059            0        17,059
  AM                        16,975        16,975            0        16,975
```

**Reference points (all out of `real_allocs`)**:

- `None`: invocations=51,243,223, **real_allocs=1**, frees=0. The #301 nullary singleton works as designed — 51M invocations collapse to one immortal chunk. (Not in top 30 by leak: leak=1.)
- `Some`: invocations=19.1M, real_allocs=2.24M, leak=2.03M. The immortal-payload path (#304) eats 88% of `Some` invocations; the residual 2.24M physical allocs leak at 90.4%.
- Singleton + immortal-payload paths together absorb 78.73M − 6.24M = **72.5M invocations (92.1%)**, leaving 6.24M real allocs.

This validates that the night-of-2026-05-06→07 patches (#301/#304) are doing their structural job. The remaining leak (5.87M chunks) is **not** dominated by the constructors that singletons could absorb.

## Closure column — KAI_TRACE_RC

Same workload, vanilla `KAI_TRACE_RC=1` build:

```
[KAI_TRACE_RC] STRICT alloc_total=59,123,295 free_total=39,453,841 leaked=19,669,454 live_peak=19,670,004
[KAI_TRACE_RC] tag=closure allocs=1,742,333 frees=1,254,943 live=487,390 LEAK
```

**Closure leak rate = 487,390 / 1,742,333 = 27.97%.**

Pre-night reference (#293 Phase 1 retro, cited in #298): **27.9%**.

Identical to two decimal places. The night's fixes shifted the variant column substantially (None 50M → 1, Some immortal-payload absorption) but did not touch closures. The 27.9% rate that motivated #298 remains exactly as the issue describes.

## record column — KAI_TRACE_RC

```
[KAI_TRACE_RC] tag=record  allocs=9,084,019 frees=2,167,686 live=6,916,333 LEAK
```

**Record leak rate = 6,916,333 / 9,084,019 = 76.1%.** The umbrella "pass-through" hypothesis from #297 was framed against variants, but records show the same shape and a higher rate — they should be in scope.

## Per-issue decision

### #298 — Closure capture lifecycle (KEEP)

- **Threshold**: closure leak rate < 5% to close, > 10% to keep open.
- **Observed**: 27.97%.
- **Crossed**: KEEP.
- **No spec change required** — the existing acceptance gate (`< 5%` post-fix) is still the right target. The issue body's premise has not been invalidated by the night's work.

### #297 — perceus_pass last-use on pass-through (KEEP, REFRAMED)

- **Threshold A** (close): top-5 variant names by leak < 30% of total variant leak.
- **Observed**: top-5 (`Some`, `EP`, `ECall`, `TyCon`, `Step`) = 3,883,068 / 5,869,319 = **66.1%**. Far above 30%, well into KEEP.
- **Threshold B** (single-variant audit): one name > 40% of total leak.
- **Observed**: `Some` is 34.6% — close but does not cross. The leak is distributed across many AST/typer constructors, not a single hotspot.
- **Threshold C** (records): record leak rate > 30%.
- **Observed**: 76.1%. Records belong in scope.

**Refreshed spec for #297** (proposed; integrator may absorb verbatim into the issue body):

> ## Spec (post-#320 baseline; updated 2026-05-07)
>
> 1. Use `-DKAI_TRACE_VAR_NAMES=1` (now in `stage0/runtime.h`) to identify physical-leak hotspots by variant name. Singleton (#301) and immortal-payload (#304) paths absorb 92% of `kai_variant` invocations; only the residual `real_allocs` column matters.
> 2. **Top-5 variant leakers** (`Some`, `EP`, `ECall`, `TyCon`, `Step`) account for 66% of variant leak. Audit `perceus_pass` against the call sites that produce these:
>    - `Some`: 370 emit sites in stage2.c; immortal-payload absorbs 88% of invocations but the runtime-payload form (a literal `kai_int`/`kai_str` of a non-cached arg) still leaks 2M chunks. Look for last-use mis-detection on `Some(value)` patterns where `value` is freshly produced.
>    - `EP`: **single emit site** — the prelude builtin table (a 50-element cons list of `kai_variant(0, "EP", 3, ...)` calls). 642K leak, 0 frees. The prelude table is constructed once but never released; flag for reuse-in-place or perceus discipline on the typer's prelude path.
>    - `ECall`/`TyCon`/`Step`: typer/synthesise pipeline constructors. These are exactly the "pass-through" shape — values flow through `infer_call`/`synth_*` without being structurally consumed. Audit `perceus_pass` last-use rules against these emit sites.
> 3. **Records dominate over variants** at 76.1% leak rate (vs 94% for variants but only 6.24M chunks; records are 6.92M live). Issue scope should explicitly include records.
>
> ## Acceptance gate (refreshed)
>
> - Top-5 variant names by leak account for < 30% of total variant leak (currently 66.1%).
> - Record leak rate ≤ 30% in kaic2 self-compile (currently 76.1%).
> - Variant leak rate ≤ 40% (currently variant_live / variant_allocs = 5.87M / 6.24M = 94%; aim ≤ 40%).
> - Closure work (#298) is independent and not gated by #297.
> - Tier 0 + Tier 1 + Tier 1-ASAN green; selfhost byte-identical.
>
> ## Cost re-estimate
>
> 5–8h. The single-emit-site EP leak alone may close ~10% of variant leak with one fix. Pass-through audit on Some/ECall/TyCon/Step is the bulk of the work.

## Cross-references

- **#293** — umbrella RC discipline.
- **#296** — superseded by #300.
- **#300** — recommended `variant_name` histogram; this lane implements it as opt-in instrumentation.
- **#301** — nullary singletons (None=1 chunk: confirmed working).
- **#304** — immortal-payload variants (absorbs 88% of `Some` invocations: confirmed working).
- **#307** — string interning.
- **#313** — tcrec _scr decref.
- **#320** — pipeline reorder perceus-before-tcrec (the structural change that motivated this validation).

## Reproduction

```sh
make -C stage1 clean && make -C stage2 clean
CFLAGS="-std=c99 -Wall -Wno-unused-function -Wno-unused-variable -g -O0 -DKAI_TRACE_VAR_NAMES=1" make -C stage1 kaic1
CFLAGS="-std=c99 -Wall -Wno-unused-function -Wno-unused-variable -g -O0 -DKAI_TRACE_VAR_NAMES=1" make -C stage2 kaic2
./stage2/kaic2 stage2/compiler.kai > /dev/null 2>varnames.log
grep '^\[VAR_NAME\]' varnames.log | head -32
```

`KAI_TRACE_RC_QUIET=1` suppresses the report (shared with the address-keyed tracer's quiet flag).
