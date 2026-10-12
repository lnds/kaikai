# The fixed cost of compiling a tiny kaikai program on Linux

Scope: the C backend as tier1 pays it — `kaic2 --core-cache-dir …`, then
`clang $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 out.c -o bin -lm`, then the
binary. Every number below is labelled **MEASURED** (taken on the machine
described in [Method](#method)) or **READ** (taken from a file or a CI log, not
reproduced here). Derived figures say which measured numbers they multiply.

## Results

### 1. Where one fixture compile goes

Wall time, min of 3 runs (spread = (max − min) / min), idle 4-vCPU Linux
x86-64 box, clang 18.1.3, flags exactly as the fixture loops use them
(`-std=c99 -Wall -Wno-unused-function -Wno-unused-variable -g -O2`). MEASURED.

| stage | hello | effects fixture | perceus fixture |
|---|---:|---:|---:|
| kaic2, core cache warm (`--core-cache-dir`, as the loops run) | 202 ms (14%) | 227 ms (16%) | 237 ms (37%) |
| kaic2, core cache cold (empty dir) | 278 ms (39%) | 304 ms (35%) | 321 ms (56%) |
| kaic2, no core cache flag | 233 ms (17%) | 300 ms (23%) | 305 ms (12%) |
| **clang compile + link, one step (as the loops run)** | **1,670 ms (2%)** | **3,510 ms (6%)** | **3,526 ms (5%)** |
| — clang `-c` alone | 1,713 ms (7%) | 3,570 ms (2%) | 3,523 ms (17%) |
| — link alone (`clang x.o -lm`) | 40 ms (13%) | 40 ms (38%) | 46 ms (45%) |
| run | 3.6 ms (12%) | 3.4 ms (37%) | 4.0 ms (14%) |
| **total, loop shape (warm kaic2 + compile/link + run)** | **1,876 ms** | **3,740 ms** | **3,767 ms** |
| **clang's share of the total** | **89%** | **94%** | **94%** |

Instructions (callgrind `Ir`, 3 runs, spread < 0.002% on every row). MEASURED.

| stage | hello | effects fixture | perceus fixture |
|---|---:|---:|---:|
| kaic2 warm | 1.231 G | 1.560 G | 1.618 G |
| kaic2 cold | 1.895 G | 2.224 G | 2.282 G |
| kaic2 no cache | 1.690 G | 2.019 G | 2.077 G |
| clang `-c` | 6.943 G | 16.778 G | 16.942 G |
| link | 0.116 G | 0.134 G | 0.134 G |
| run | 0.0004 G | 0.0004 G | 0.0007 G |

Programs: hello is `fn main() : Unit / Console = println("hi")`; the effects
fixture is `examples/effects/cap_read_in_interp.kai`; the perceus fixture is
`examples/perceus/cspec_heap_int_copies.kai`. Both fixture binaries match their
`.out.expected`.

The headline: on Linux the per-fixture floor is clang, not kaic2. kaic2 is
0.2 s; clang is 1.7 s for hello and 3.5 s for each real fixture.

#### Inside the clang compile

What the translation unit is made of (MEASURED, line counts):

| | hello | effects fixture | perceus fixture |
|---|---:|---:|---:|
| `stage2/runtime.h` (included whole) | 22,208 | 22,208 | 22,208 |
| preprocessed TU | 27,833 lines / 862 KB | — | — |
| emitted `.c`, total | 423 | 2,912 | 3,101 |
| — glue (type prelude, default-effect shims, `main`, impl tables) | 381 | 694 | 718 |
| — core stdlib code (monomorphised `protocols`, `decimal`, `complex`, `list`, …) | 34 | 2,210 | 2,349 |
| — the user's own functions | 9 | 9 | 35 |

Clang phase split for hello (`-ftime-report`, one run of 2.03 s; MEASURED):
front end (parse + sema) ≈ 0.18 s (`-fsyntax-only` alone: 198 ms, min of 3),
LLVM IR generation 0.06 s, then LLVM optimization + code generation 1.80 s —
the middle-end pass pipeline 0.80 s and the back end 0.84 s, of which DWARF
emission 0.11 s. **89% of clang's time is optimizing and generating code, not
parsing.** A TU that only `#include`s runtime.h costs 207 ms
at `-g -O2` (MEASURED): unused `static` runtime functions are dropped cheaply;
what costs is the runtime code that the emitted `main` reaches (scheduler, RC,
strings), which is re-optimized in every TU.

Who the optimizer time belongs to (`-ftime-trace`, per-function `OptFunction`
time classified by where the function is defined; MEASURED):

| | runtime.h | core stdlib code | glue | user |
|---|---:|---:|---:|---:|
| hello | 78% | — | 20% | 3% |
| effects fixture | 35% | 58% | 6% | 1% |
| perceus fixture | 35% | 56% | 7% | 1% |

So in a real fixture **99% of the C the compiler optimizes is not the user's**:
about a third is runtime.h, more than half is core stdlib code kaic2 emitted.

Why the fixtures carry 2,200 lines of core code: **one string interpolation
pulls in the core protocol impl table — 88 impls (Show, Eq, Ord, … of core
types).** `println("hi #{1}")` emits 2,955 lines and
registers 88 protocol impls; `println("hi")` emits 423 and registers none
(MEASURED, same holds for `"#{int_to_string(1)}"`; an `assert` alone registers
16). Cost of that one `#{1}` (MEASURED): clang `-c` 1,513 → 4,005 ms (min of 3),
6.94 G → 17.11 G instructions; kaic2 1.23 G → 1.56 G instructions. In the
tier1 census below, 1,229 of the 2,749 compiled kaikai TUs register ≥ 80 impls.

### 2. kaic2 on hello, core cache warm (callgrind, 1.231 G Ir; MEASURED)

| phase | Ir | share | scales with |
|---|---:|---:|---|
| 0 core blob decode (`load_core_cached`) | 169.7 M | 13.8% | core size (flat: identical for hello and a 200-function program) |
| 1 lex + parse of the user file | 0.2 M | 0.0% | user size |
| 2a resolve / scope / module checks, before the core cut | 491.7 M | 39.9% | every declaration, core + user |
| 2b desugar / surface lowering, before the core cut | 368.8 M | 30.0% | every declaration, core + user |
| 3 core reachability cut (`core_reach`) | 11.4 M | 0.9% | — |
| 4 typecheck (`typed_cut`, after the cut, 32.2 M) + `infer_*` validators (before the cut, ≈ 27 M) | 59.2 M | 4.8% | reached / every declaration |
| 5 monomorph (`mono_front`) | 9.1 M | 0.7% | reached declarations |
| 6 perceus + post-cut lowering | 12.3 M | 1.0% | reached declarations |
| 7 emit C | 19.9 M | 1.6% | reached declarations |
| 8 drop of intermediate trees (`kai_internal_drop` in the driver) | 78.3 M | 6.4% | every declaration |
| 9 process start, CLI, exit | 10.9 M | 0.9% | fixed |

Phases are the direct callees of `compile_source` (plus `load_core`), bucketed
by module, and placed before or after the cut by their call line relative to
`core_reach` (`stage2/compiler/driver.kai:5614`). Callgrind's call-arc costs
make every row exact; rows sum to the total.

**Fraction spent on core declarations the program never uses.** The core set
is 16 modules, 5,230 lines, 460 `fn` definitions counting impl methods
(MEASURED, `grep`).
Hello's emitted C uses `io.println`, two core helpers and 15 default-effect
shims. Rows 0, 2a, 2b, 8 and the pre-cut validators run over the whole core
stream before anything is cut: **≈ 92% of kaic2's instructions on hello are spent
on core declarations, essentially all of them unused.** After the cut, the
whole typed back half (typecheck, monomorph, perceus, emit) is ≈ 5%.

**Is each phase per core declaration?** Experiment (MEASURED): the same compile
on a program with 200 trivial user functions (`perf-floor/programs/syn200.kai`).
Pre-cut passes grow only ×1.05–×1.34 (`resolve_decls_views` ×1.17,
`lower_protocols` ×1.05, `st_extend` ×1.33), and several are flat ×1.00
(`qual_head`, `effect_scope_core`, `qualtype_decls`, `rqc_decls`): their cost
is proportional to the declaration stream, which for a fixture is almost all
core. Post-cut passes scale with reached code: `typed_cut` ×5.5, `emit_program`
×13.9. Full per-pass table in `raw/kaic2-passes-hello-vs-syn200.txt`.

Where the instructions physically go (self cost, MEASURED): `kai_free_rec`
11.8%, `_int_malloc` 6.7%, `_int_free` 5.9%, `malloc` 4.6%, `free` 2.7%,
`kai_alloc` 2.2%, `calloc` 1.4% — **≈ 35% of kaic2's instructions are allocation
and freeing**, the cost of building and tearing down whole-core ASTs in every
pass.

Top 25 functions by inclusive Ir (hello, warm; MEASURED). Inclusive cost of a
function in a call cycle (the AST walkers and `kai_apply`) is over-counted by
callgrind; the phase table above is the exact view.

| # | inclusive Ir | share | function |
|---:|---:|---:|---|
| 1 | 1,050.8 M | 85.3% | `driver.compile_source` |
| 2 | 587.5 M | 47.7% | `kai_apply` (closure calls; cycle) |
| 3 | 533.8 M | 43.4% | `resolve.chk_expr` (cycle) |
| 4 | 529.0 M | 43.0% | `resolve.chk_kind` (cycle) |
| 5 | 378.2 M | 30.7% | `list.map` |
| 6 | 310.4 M | 25.2% | `kai_free_rec` |
| 7 | 307.1 M | 24.9% | `ast.BinSerialize[Expr].from_bytes` (cycle) |
| 8 | 263.7 M | 21.4% | `ast.BinSerialize[ExprKind].from_bytes` (cycle) |
| 9 | 180.2 M | 14.6% | `resolve_program.resolve_decls_views` |
| 10 | 178.6 M | 14.5% | `resolve_program.resolve_run` |
| 11 | 175.0 M | 14.2% | `desugar.lcr_expr` (cycle) |
| 12 | 169.7 M | 13.8% | `driver.load_core` |
| 13 | 169.6 M | 13.8% | `driver.load_core_cached` |
| 14 | 163.7 M | 13.3% | `protocols.bin_list_from_bytes` (Decl) |
| 15 | 162.9 M | 13.2% | `ast.BinSerialize[Decl].from_bytes` |
| 16 | 154.9 M | 12.6% | `desugar.lcr_kind` (cycle) |
| 17 | 152.0 M | 12.3% | `resolve.chk_decl` |
| 18 | 150.6 M | 12.2% | `cache_ast.cache_deserialize_module` |
| 19 | 143.2 M | 11.6% | `desugar_map.dsg_map_expr_kind` (cycle) |
| 20 | 139.2 M | 11.3% | `cache_ast.cache_decls_from_bytes` |
| 21 | 133.2 M | 10.8% | `resolve.chk_call` |
| 22 | 119.6 M | 9.7% | `malloc` |
| 23 | 117.5 M | 9.5% | `unit_walk.map_units_expr` (cycle) |
| 24 | 114.6 M | 9.3% | `free` |
| 25 | 103.5 M | 8.4% | `kai_alloc` |

### 3. What one tier1 run invokes

All eight light slices (`make -C stage2 test-light-shard SHARD=n SHARDS=8`) and
the namespace-collision C axes (`test-namespace-collisions-c-axes`, the other
half of `tier1-shard-2`), each from cold kai caches, under logging shims for
kaic2, kaic1 and cc. Per-call CPU is from `wait4` rusage (includes waited
children such as `ld`); "other" is the make tree's CPU minus the logged calls
(make, sh, bin/kai itself, diff, python). MEASURED.

| target | elapsed | kaic2 calls | cc calls | runs | kaic2 CPU | cc CPU | run CPU | other CPU |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| light 1/8 | 230 s | 360 | 218 | 209 | 117 s (16%) | 567 s (77%) | 1.2 s | 48 s (6.5%) |
| light 2/8 | 142 s | 156 | 221 | 158 | 60 s (11%) | 468 s (86%) | 1.0 s | 18 s (3.3%) |
| light 3/8 | 206 s | 312 | 720 | 300 | 170 s (21%) | 585 s (73%) | 1.8 s | 41 s (5.2%) |
| light 4/8 | 494 s | 562 | 430 | 411 | 189 s (10%) | 1,646 s (89%) | 2.2 s | 22 s (1.2%) |
| light 5/8 | 183 s | 165 | 397 | 96 | 60 s (8.5%) | 634 s (89%) | 0.8 s | 17 s (2.4%) |
| light 6/8 | 251 s | 366 | 280 | 249 | 102 s (10%) | 867 s (88%) | 0.5 s | 17 s (1.7%) |
| light 7/8 | 215 s | 262 | 247 | 288 | 70 s (9.5%) | 644 s (88%) | 0.8 s | 14 s (1.9%) |
| light 8/8 | 159 s | 730 | 210 | 255 | 193 s (34%) | 357 s (62%) | 0.9 s | 21 s (3.7%) |
| **all 8 light slices** | **1,880 s** | **2,913** | **2,723** | **1,966** | **960 s (14%)** | **5,766 s (83%)** | **9.3 s (0.1%)** | **200 s (2.9%)** |
| nscol C axes | 396 s | 580 | 402 | 378 | 136 s | 1,363 s | 0.8 s | n/a ¹ |

¹ The shell's `times` reported 2,793 CPU-s for this target, more than 4 cores
can deliver in 396 s; the logged calls alone (1,500 CPU-s) already fill the
box, so "other" is not reported for it.

kaic1 was called 12 times (0.1 CPU-s). Light slices ran with
`TIER1_SELFHOSTS=0`.

The cc calls by kind, all nine targets (MEASURED):

| cc call | count | CPU | mean |
|---|---:|---:|---:|
| fixture loop: full TU (runtime.h inlined), compile + link, `-g -O2`, registers ≥ 80 impls | 882 | 3,679 s | 4.17 s |
| fixture loop: same, registers < 80 impls | 733 | 2,150 s | 2.93 s |
| bin/kai: program TU, `-c -O2 -DKAI_SEPARATE_COMPILATION=1`, ≥ 80 impls | 347 | 552 s | 1.59 s |
| bin/kai: same, < 80 impls | 787 | 713 s | 0.91 s |
| link-only, sanitizer and other small calls | 376 | 34 s | 0.09 s |

Means are under `make -j8` on 4 cores, so they run above the idle-box figures of
section 1.

For comparison, READ from CI run 38178989804 (PR `split-monoliths-movable`,
compiler-touching, `TIER1_SELFHOSTS=1`): tier1 shard steps took 475 / 635 /
970 / 708 / 884 / 1,069 / 573 / 598 s (shards 1–8), tier0 765 + 389 s, ASAN
305 + 206 s. In that log, light slice 2 took 196 s and the namespace C axis
416 s (`22:19:59 → 22:26:55`).

### 4. What bin/kai's caches avoid, and what the loops leave off

READ (`tools/kai/cli_toolchain.kai`, `cli_plan.kai`, `cli_csingle.kai`,
`stage2/compiler/emit_cache.kai`, `driver.kai:4666`, `stage2/Makefile:13919`):

- `--core-cache-dir`: post-parse core blobs (KAB2). Both bin/kai and the
  fixture loops pass it.
- `--toolchain-id`: enables the emitted-C core TU cache (KCT2), whose key hashes
  the toolchain id, edition, every core source and the program's type-level
  surface. It serves only the c-modular emit. bin/kai passes it; the loops do
  not (the Makefile comment: "Only --core-cache-dir is passed (no
  --toolchain-id), so the emitted-C and native core caches stay off here"),
  and the loops emit one TU anyway.
- bin/kai also keeps compiled objects: the runtime owner
  (`runtime_owner_c.c` at `-O2`, keyed by cc, flags and runtime.h bytes) and the
  core TU objects. The program TU is compiled with
  `-DKAI_SEPARATE_COMPILATION=1`, so the scheduler lives in the owner object,
  not in every program. The fixture loops call `$(CC)` on the raw emitted file:
  every fixture re-optimizes the reached runtime and all emitted core code.

MEASURED, `bin/kai build --backend=c`:

| | wall |
|---|---:|
| hello, every kai cache cold (17 compile calls — core TUs, runtime owner, program — summing 16.8 s, then a link) | 9.5 – 10.0 s (3 runs) |
| hello again, caches warm (kaic2 0.55 s + link 0.05 s; program `.o` cached) | 0.56 – 0.68 s (3 runs) |
| effects fixture, first build, core caches warm (kaic2 0.68 s + program TU 1.12 s + link 0.06 s) | 1.95 s (1 run) |
| perceus fixture, same (kaic2 0.67 s + program TU 1.46 s + link 0.05 s) | 2.27 s (1 run) |
| effects fixture with `CFLAGS=… -O0`, core caches warm (kaic2 0.59 s + program TU 0.37 s + link 0.05 s) | 1.10 s (1 run) |

A loop-shape compile of the same fixtures costs 3.74 / 3.77 s (section 1).
bin/kai's kaic2 is slower than the loops' (0.55–0.68 s against 0.20–0.24 s):
the c-modular emit with the emit cache costs more than the single-TU emit on
these programs. With `KAI_BIN_MEMO` on, a repeat `build` still ran kaic2 (0.55 s).

## Top 5 levers, ranked by seconds saved per tier1 run

Units: CPU-seconds over the measured scope (all 8 light slices + namespace C
axes ≈ 8,200 CPU-s in logged kaic2/cc/run calls), and runner-seconds =
CPU-s ÷ 4, assuming 4-core runners like the box measured here (the CI runner
size was not verified). Each saving multiplies a per-TU saving MEASURED on
the idle box by a call count and CPU MEASURED in the census; ranges come from
the spread of the per-TU ratios. Levers 1 and 2 overlap; the others stack.

### 1. Fixture loops: compile the TU at `-O0` and link a prebuilt `-O2` runtime owner — ≈ 5,000–5,400 CPU-s ≈ 1,250–1,350 runner-s

Evidence (MEASURED, clang `-c`, min of 3):

| TU | `-g -O2` (today) | `-g -O0` | `-O0 -DKAI_SEPARATE_COMPILATION=1` |
|---|---:|---:|---:|
| hello | 1,513 ms | 301 ms | 205 ms (−86%) |
| hello + `#{1}` | 4,005 ms | 454 ms | 285 ms (−93%) |
| effects fixture | 3,796 ms | 468 ms | 313 ms (−92%) |
| perceus fixture | 3,768 ms | 491 ms | 355 ms (−91%) |

The owner object (`runtime_owner_c.c`, `-g -O2`) compiles once in 2.14 s.
Today's full-runtime fixture TUs cost 5,829 CPU-s (1,615 TUs); −86…−93% of that
is the range above. Without the owner split, `-g -O0` alone gives −80…−89%
(4,660–5,190 CPU-s); the split alone at `-g -O2` gives −15…−44% (hello
1,519 → 851, effects 3,796 → 2,898, perceus 3,768 → 3,189 ms). Both fixtures'
`-O0` binaries still match their goldens and run in 3 ms. Risk to pilot first:
fixtures whose verdict depends on `-O2` (stack depth of non-tail recursion,
sanitizer legs, timing or benchmark gates). The separate-compilation runtime is
the configuration bin/kai already ships.

A smaller variant: drop `-g` at `-O2` — −12% (hello 1,519 → 1,330 ms) to −25%
(effects 3,796 → 2,835 ms), ≈ 700–1,460 CPU-s; it is almost entirely subsumed
by lever 1 (at `-O0`, `-g` costs ≈ 20–115 ms per TU).

### 2. Make string interpolation stop reaching all 88 core protocol impls — ≈ 1,330–2,430 CPU-s ≈ 330–610 runner-s (at today's `-O2`)

Evidence (MEASURED): one `#{1}` adds 2.49 s of clang (1,513 → 4,005 ms) and
2,530 emitted lines; 10.2 G clang instructions and 0.32 G kaic2 instructions
(+26%). In the census 882 full fixture TUs and 347 bin/kai TUs register ≥ 80
impls; under load their mean cc CPU exceeds the < 80 group by 1.24 s and 0.69 s.
Low end: 882 × 1.24 + 347 × 0.69; high end: 882 × 2.49 + 347 × 0.69. After
lever 1 the clang part shrinks to ≈ 0.08–0.15 s per TU (285 vs 205 ms;
454 vs 301 ms), but the kaic2 part (+26% instructions per such program) and the
bin/kai path keep it. Hypothesis, not verified here: interpolation lowers to
dynamic `Show` dispatch, and reaching the runtime impl table keeps every
registered impl (Show, Eq, Ord, … for every core type) and its dependencies.

### 3. bin/kai: compile the program TU at `-O0` when it is a test fixture — ≈ 850 CPU-s ≈ 210 runner-s

Evidence: the census has 1,134 bin/kai program-TU compiles at `-O2` costing
1,266 CPU-s (MEASURED). On the effects fixture with warm core caches the
program TU drops from 1,121 ms at `-O2` to 374 ms with `CFLAGS=… -O0` (−67%,
one run each; MEASURED); 0.67 × 1,266 ≈ 850. Caveat (MEASURED): with that
`CFLAGS`, the perceus fixture's build recompiled all 16 core TUs at `-O0`, so
the core objects must be warmed per flag set, once per job.

### 4. kaic2: cut the core before resolve and desugar, not after — up to ≈ 860 CPU-s ≈ 215 runner-s

Evidence (MEASURED): the census has 3,493 kaic2 calls costing 1,096 CPU-s. On
hello, the pre-cut whole-core passes plus their tree drops are ≈ 78% of
instructions (rows 2a, 2b, 8 and the pre-cut validators); 0.78 × 1,096 ≈ 860
is the ceiling if those passes touched only reached declarations (they would
still cost something on the reached set). The share falls as user code grows:
55% on the 200-function program. Allocation and freeing are ≈ 35% of kaic2's
instructions, so the same cut removes most of that churn. A variant with the
same ceiling: persist the core after desugar (it is the same for every program
of an edition) instead of the post-parse blobs.

### 5. kaic2: stop decoding the whole core per invocation — ≈ 160 CPU-s ≈ 40 runner-s

Evidence (MEASURED): blob decode is 13.8% of warm hello (169.7 M Ir, identical
for hello and the 200-function program), process start and exit 0.9%;
0.147 × 1,096 ≈ 160. `--batch-list` already compiles many inputs against one
core load (READ, `driver.kai:5962`) and is used only by
`test-batch-isolation`; a fixture loop driving it, or a decode limited to
reached modules, collects this. It stacks with lever 4 only on the part
lever 4 leaves.

Not levers: runs (0.1% of CPU), link-only calls (0.2%), kaic1 (0.0%),
make/shell/bin-kai overhead (2.9% on the light slices).

## Method

- Machine: 4-vCPU Linux 6.18 x86-64 VM; clang 18.1.3 (Ubuntu), GNU ld,
  valgrind 3.22. `perf stat` was not possible: the VM exposes no CPU PMU
  (`/sys/bus/event_source/devices` has no `cpu`), so instruction counts are
  callgrind totals (`--trace-children=yes` for clang, summing every process).
- Compiler: `make CC=clang KAI_LLVM=0 kaic2` from main at `1468a0c`
  (`KAIC_BOOT=release`), C-only (the first attempt auto-enabled the native
  backend because `llvm-config` exists but the static LLVM libraries do not).
  `make CC=clang kaic1` for the slices that need stage 1. The sandbox lacked
  clang's sanitizer runtime; `libclang-rt-18-dev` was installed before the
  slice-3 run that links ASAN.
- Section 1: `tools/measure.sh` — wall via `date +%s%N` around each command,
  3 runs, then 3 callgrind runs. "Cold" = a fresh empty `--core-cache-dir`;
  "warm" = a directory populated by one prior compile; edition flag
  `--edition hanga-roa` as `$(EDITION_FLAG)` gives.
- Clang attribution: `-ftime-report` (one run) for the phase split;
  `-ftime-trace` with per-function `OptFunction` events classified by the
  function's definition site (`tools/attrib.py`): defined in runtime.h,
  emitted with a core-module prefix (`kaiu_<core module>__`), glue, or user.
- kaic2 profile: callgrind call arcs out of `compile_source` and `kaiu_main`
  (`tools/cgarcs.py`, `tools/phasetab.py`, `tools/diffphase.py`).
- Census: `stage2/kaic2` and `stage1/kaic1` replaced by a C wrapper
  (`tools/wrap.c` → `tools/cpuexec.c`, ~1 ms overhead) and `CC` by
  `tools/cc-shim.sh` (bash, ~6 ms overhead per call), which also links
  `tools/runtime_timer.c` into every binary so each run logs its own wall and
  CPU. `~/.cache/kai` and `stage2/build/core-cache` were cleared before each
  target (CI runners start cold). Default `TEST_JOBS=8` on 4 cores, as on CI.
  A first pass with warm kai caches gave 147 instead of 218 cc calls on slice 1
  (bin/kai's object and binary memos hit); it is not used. The real binaries
  were restored afterwards.
- READ, not reproduced: the macOS arm64 figure of 2.2 G instructions per hello
  (from the brief) and the CI step durations above.

## Raw data (`perf-floor/raw/`)

- `per-program.tsv` — every section-1 run (wall µs and Ir).
- `clang-flag-variants.txt` — clang `-c` wall under each flag set (3 runs).
- `clang-attribution.txt`, `hello-clang-ftime-report.txt` — clang splits.
- `kaic2-hello-warm-phases.txt`, `kaic2-syn200-warm-phases.txt`,
  `kaic2-hello-warm-passes.txt`, `kaic2-passes-hello-vs-syn200.txt`,
  `kaic2-hello-warm-top-functions.txt` — kaic2 profile tables.
- `extra-ir.tsv` — Ir for the interpolation and 200-function programs.
- `tier1/` — per-call logs (`kaic2|cc|run wall_ns cpu=… rc=… link= src= lines=
  impls= opt= sep= san=`), per-target cost lines and summaries.
- `bin-kai-builds.txt` — the bin/kai builds of section 4 with their cc logs.
- `../programs/` — every `.kai` measured; `../tools/` — the scripts and shims.
