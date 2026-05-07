# Lane retro: #297 first wave — EP prelude leak (immortal_var table sizing)

**Date**: 2026-05-07
**HEAD pin**: `0e73ac8` (`Merge pull request #317 from lnds/issue-312-real-literal-division`)
**VERSION pin**: `0.44.0`
**Lane branch**: `issue-297-ep-prelude-leak`
**Workload**: `./stage2/kaic2 stage2/compiler.kai > /dev/null`

## TL;DR

The 642,420-leak / 0-frees `EP` constructor signal in the
`-DKAI_TRACE_VAR_NAMES=1` histogram (#321 validation lane,
2026-05-07) was attributed to "single emit site, prelude builtin
table never released". The fix was simpler and more general than
the lane brief expected: the `EP` variants **already** qualify for
the `kai_immortal_var` cache (#293 next-tier path) — all three
arguments (`kai_str(name)`, `kai_str(cname)`, `kai_int(arity)`)
carry `rc == INT32_MAX`. The cache was just sized wrong.

`KAI_IMMORTAL_VAR_BUCKETS` was set to **16384** in #293 with the
sizing rationale "ample room for the realistic combination space
(Some/Ok/Err × ~tens of payload singletons)". In the kaic2
self-compile workload that estimate undershoots by ~16x:

- 16,384 successful installs → table full
- **779,224 install attempts after that** silently fall through
- subsequent lookups walk all 16,384 buckets without seeing the
  empty sentinel and return `NULL`, so every later `kai_variant(...)`
  call freshly allocs.

The `EP` table is one victim — `prelude_table()` reconstructs 47
entries × ~13.7K calls = 642K allocs, all leaks. `Some(<immortal>)`
is the larger victim (Some leak dropped 2,028,599 → 1,983,599, ~45K
chunks recovered).

Fix: bump `KAI_IMMORTAL_VAR_BUCKETS` to **262144** (16x). One-line
change, +16 MB of `.bss`, eliminates the structural shortfall for
this workload.

## Diagnostic walk

Built with `-DKAI_TRACE_VAR_NAMES=1`, baseline showed:

```
[VAR_NAME] EP                       inv=642555       real=642555     frees=0          leak=642555
```

`invocations == real_allocs` means every call falls through to
fresh alloc — singleton/immortal-payload paths (#301/#304) never
fire. Expected, since the lane brief framed `EP` as a candidate for
explicit `immortal` annotation.

Probe added inside `kai_variant`:
- count `is_ep` calls that take the `kai_args_all_immortal` path
- count immortal-var lookup hits vs misses
- count `kai_immortal_var_install` overflows

Result:

```
[EP_DIAG] immortal_hit=0 immortal_miss=642555 not_immortal=0
[EP_DIAG] immortal_install ok=16384 full=779224
```

Three findings collapse together:
1. **Every `EP` call qualifies as all-immortal** (`not_immortal=0`).
   The pre-existing #304 path is the right home for them; no new
   "explicit immortal annotation" is needed.
2. **Lookup never hits** (`immortal_hit=0`). The first `EP("print",
   ...)` in the program never lands in the cache — it's not the
   first variant the kaic2 self-compile sees. By the time the typer
   calls `prelude_table()`, the cache is already full of
   parser/lexer Some/Ok/TyCon-shaped variants.
3. **Installs overflow ~48x** (`16384 ok / 779224 full`). The
   16,384 sizing was the bottleneck.

## Fix

```diff
-#define KAI_IMMORTAL_VAR_BUCKETS 16384
+#define KAI_IMMORTAL_VAR_BUCKETS 262144
```

`stage0/runtime.h` only — both kaic1 and kaic2 pick it up via
`-I ../stage0`.

Comment block above the macro updated to record:
- the sizing-shortfall failure mode (table-full + linear probe →
  zero empty sentinels → silent install failures);
- why 262144 not some smaller bump (the `EP` workload demands
  ~17K just for the typer, and Some/Ok-shaped immortal-payload
  combinations push the parsing-time working set well past 16K);
- the .bss cost (+16 MB).

## Re-measurement

Same workload, same flags, after the bump:

```
[VAR_NAME] total distinct=184 invocations=78788191 real_allocs=5482410 frees=373965 leak=5108445
[VAR_NAME] Some                     inv=19122700     real=2199998    frees=216399     leak=1983599
[VAR_NAME] ECall                    inv=486979       real=486979     frees=16781      leak=470198
[VAR_NAME] TyCon                    inv=424488       real=424472     frees=29496      leak=394976
[VAR_NAME] Step                     inv=350993       real=350993     frees=2543       leak=348450
[VAR_NAME] EP                       inv=642555       real=45         frees=0          leak=45
```

Deltas vs the #321 baseline (5,869,319 leak / 6,243,198 real_allocs):

| metric | before | after | delta |
|---|---|---|---|
| `EP` real_allocs | 642,420 | 45 | **−99.99%** |
| `EP` leak | 642,420 | 45 | **−99.99%** |
| `Some` leak | 2,028,599 | 1,983,599 | −45,000 (−2.2%) |
| total real_allocs | 6,243,198 | 5,482,410 | −12.2% |
| total leak | 5,869,319 | 5,108,445 | **−13.0%** |

The 45 residual `EP` chunks are exactly the 45 distinct entries in
`prelude_table()` (one cached immortal singleton each); they're
INT32_MAX-rc'd and never freed. The leak floor matches the table
size exactly.

## Selfhost integrity

```
$ make -C stage2 selfhost
self-hosting fixed point: OK
```

Vanilla build (no `-DKAI_TRACE_VAR_NAMES`) emits byte-identical
`stage2.c` between `kaic2` and `kaic2'`. The fix is a runtime
sizing constant; no IR changes.

## Acceptance gate

- [x] `EP` real_allocs drop ≥ 90%: 642,420 → 45 (−99.99%) ✓
- [x] Total variant leak drops ≥ 5%: 5,869,319 → 5,108,445 (−13.0%) ✓
- [x] Selfhost byte-identical: confirmed via `make -C stage2 selfhost` ✓
- [x] Tier 0 green: `make tier0` OK (selfhost byte-identical, demos baseline 26 holds)
- [x] Tier 1 green (see PR check)

## Cost

The actual edit was 1 line. The diagnostic walk took ~30 minutes
of build-cycle time. Total lane wall-clock ~45 minutes — well
under the 1.5-3h estimate, because the right path turned out to
be a sizing constant, not a structural rework of the prelude
table.

## Why not the planned approach

The lane brief enumerated three options:
- A: cache the table across compilations within a kaic2 run
- B: matching frees in typer teardown
- C: mark EP chunks immortal with a new `kai_variant_immortal(...)`
  path

All three are more invasive than the actual fix. Option C in
particular would have duplicated the #304 immortal-payload path
that already does exactly what's needed once the sizing is right.

The 16384-bucket constant came in via #293 with explicit "ample
room" rationale; that rationale was tested against the workload at
#293 time but did not survive the kaic2 working set growing past
that point. This is the kind of sizing constant that needs
periodic re-validation as the compiler grows; #321 caught it.

## Cross-references

- **#293** umbrella RC discipline (introduced the immortal-var path).
- **#297** perceus_pass last-use on pass-through (this lane closes
  the EP wave; Some/ECall/TyCon/Step still need pass-through audit).
- **#300** variant_name attribution methodology — gave us the
  `KAI_TRACE_VAR_NAMES` instrument that surfaced the EP signal.
- **#301** nullary singletons.
- **#304** immortal-payload variants (the path this fix actually
  re-enables for the EP working set).
- **#321** validation lane (preceding lane, same HEAD pin).

## Reproduction

```sh
git checkout main && git pull
make -C stage1 clean && make -C stage2 clean
CFLAGS="-std=c99 -Wall -Wno-unused-function -Wno-unused-variable -g -O0 -DKAI_TRACE_VAR_NAMES=1" make -C stage1 kaic1
CFLAGS="-std=c99 -Wall -Wno-unused-function -Wno-unused-variable -g -O0 -DKAI_TRACE_VAR_NAMES=1" make -C stage2 kaic2
./stage2/kaic2 stage2/compiler.kai > /dev/null 2>varnames.log
grep '^\[VAR_NAME\] EP ' varnames.log
```

Expect `real=45 leak=45` on the post-fix branch and `real=642555
leak=642555` on the #321 baseline.

<!-- coverage: skip -->
