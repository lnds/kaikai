# Native codegen performance — diagnosis & plan

**Lane:** `native-codegen-perf` · **Date:** 2026-06-16 · **Backend:** in-process
libLLVM (KIR Lane 1.5) · **LLVM:** 18.1.8 static · **Host:** macOS arm64

The in-process native backend produces correct binaries (0 parity gaps) but,
for scalar-heavy code, runs **dozens of times slower** than the C-direct
backend built from the *same* `kaic2`. This document pins the root cause with
machine-level evidence, measures the gap on an honest benchmark battery, and
lays out a priority-ordered optimisation plan (validated by an `asu` design
review).

> Since PR #851 (`feat(native): flip the default backend from c to native`),
> native is the **default** backend — so this gap is what users now ship by
> default. The plan below is the work that makes that default fast.

> **Closure status (updated 2026-06-29) — the diagnosis below is historical; the plan SHIPPED.**
> Everything from "ALL-BOXED scalar codegen" onward describes the *starting*
> state (native ~6s on the arith loop). That is no longer true. The optimisation
> lanes the plan scoped have all landed, and the three residuals the
> 2026-06-17 snapshot tracked (#858/#860/#861) have since all closed:
>
> | item | what shipped | PR |
> |---|---|---|
> | P1 | native Int unbox (mode-slave arith/cmp) | #853 |
> | P2 | runtime bitcode-link before O2 | #854 |
> | §3.4 | `variant_match` super-linear fix (immortal-cache tagged-Int trap) | #856 |
> | P3 | raw scalar params/returns from `UFnSig` | #857 |
> | P4 | raw `sdiv`/`srem` for Int `/` and `%` | #859 |
> | #861 | non-tail raw-scalar calls stay raw (no box/unbox round-trip) | `b1fce7ed` |
>
> **Numbering note:** the per-PR numbering above (the one in project memory and
> CHANGELOG) is *not* the same as the §"P1/P2/P3" headings later in this doc. The
> doc's "P3 — alloca→mem2reg · do not pursue" was correctly NOT pursued; the
> *shipped* "P3" is the raw-scalar-params lane (#857), a different item the
> original plan did not enumerate.
>
> **Measured 2026-06-17 (best-of-3, native kaic2, `tools/native-perf/`) — this
> snapshot predates the #861 fix (`b1fce7ed`, 2026-06-19), so the `deep_rec`
> row is the pre-fix number; re-measure pending:**
>
> | bench | C | native | ratio |
> |---|---|---|---|
> | `arith_const` / `arith_runtime` (scalar `+ - * / %`) | 0.07 | 0.07 | **1.0× (parity)** |
> | `variant_match` | 0.00 | 0.01 | ~parity |
> | `deep_rec` (non-tail `fib`) | 0.01 | 0.08 | **~8× (pre-#861-fix)** |
> | `list_fold` | 4.96 | 9.12 | **1.84×** |
> | `rbtree_corpus` | 1.21 | 2.65 | **2.2×** |
>
> Scalar arithmetic is at C parity. Residual status:
> - **#860 (FIXED)** — cons/list RC leak (native never cascaded `decref→free`):
>   the native back-edge now emits the dropmask (`nemit_drop_assigns_masked`)
>   and self-tail arms drop the owned scrutinee (`match_selftail_scr_drop` +
>   the TRMC `__kai_cons_s` step), so `free_total` cascades exactly as the C
>   oracle (`decref_total` identical). `leaked` is now a constant, not linear
>   in the list length.
> - **#861 (FIXED, `b1fce7ed`, 2026-06-19)** — non-tail raw calls no longer
>   re-box the result (`kaix_int` → `kaix_int_field` round-trip eliminated);
>   raw-scalar calls stay raw. This drove the `deep_rec` ~8×; that row above
>   is the pre-fix measurement.
> - **#858 (FIXED, PR #862)** — native `kai_op_eq` over-decref UAF.
>
> The heap-bound traversal residuals (`list_fold`, `rbtree_corpus`) are the
> remaining native-vs-C gap; their tuning continues under Anakena.

## TL;DR

- **The brief's opt-level hypothesis is FALSE in HEAD.** `kai_native_opt_pipeline()`
  (`stage2/runtime.h:12069`) already returns `default<O2>` even when
  `KAI_NATIVE_OPT` is empty, and the pipeline *runs* (O0 vs O2 binaries differ).
  The `-O2`-default wrapper bug was closed by issue #498 (L4). There is no
  one-line opt-level fix to make.
- **The real root cause is architectural: the KIR native walk is ALL-BOXED.**
  Every `Int` is a tagged pointer and every arithmetic op is an *opaque runtime
  call* (`kaix_add` / `kaix_sub` / `kaix_mul` / …). The C-direct backend
  *unboxes* scalars (`int64_t kair_x`, native C arithmetic) via the `unbox.kai`
  pass; the native backend ignores that pass for Int arithmetic.
- **O2 cannot rescue the boxed code:** the `kaix_*` helpers are external
  `declare`s linked *after* opt by `cc` (no LTO), so the O2 pipeline sees opaque
  barriers it cannot inline, unbox, or fold through.
- **The fix already has a precedent:** the native backend is *mode-slave* for
  `Real` (raw `f64` + `fadd`/`fmul`/`fcmp`) and for the Int `let`-binder form.
  Extend that same machinery to Int *arithmetic and comparison*.

## 1. Reproduction: two native routes, one cause

The brief reported that `KAI_BACKEND=native` gave 340 KB / 6.2 s while
`KAI_NATIVE_OPT=2` gave 154 KB / 0.07 s — two native routes differing ~125×.
**That divergence does not reproduce in HEAD.** Today, with a freshly-built
`kaic2`, *every* native route is slow:

| route (arith loop, 200M iter) | binary | best(s) |
|---|---|---|
| `KAI_BACKEND=c` | 132 KB | 0.07 |
| `KAI_BACKEND=native` (default) | 348 KB | 6.05 |
| `KAI_BACKEND=native` + `KAI_NATIVE_OPT=2` | 348 KB | 6.03 |
| `KAI_BACKEND=native` + `KAI_NATIVE_OPT=0` | 348 KB | 5.99 |

`KAI_NATIVE_OPT=0` and `=2` produce **same-size, byte-different** binaries — the
O2 pipeline *runs*, it just cannot improve all-boxed code. The brief's
`0.07 s` native number was almost certainly a mismeasurement (a C-backend
binary, or a const-folded run). The reproducible finding is more severe and more
useful: the native backend is uniformly slow on scalars regardless of opt level.

## 2. Honest benchmark battery

`tools/native-perf/run.sh` builds each `benches/*.kai` with both backends and
times the binary (best of 3). Iteration counts are runtime-opaque (`N * (seed /
seed)` where `seed = string_length(program_name())`): the optimiser cannot fold
the loop to a closed form, yet `N` is fixed and identical across routes.

| bench | what it stresses | C (s) | native (s) | factor |
|---|---|---|---|---|
| `arith_runtime` | pure tail-rec arithmetic, 200M iter | 0.07 | 6.03 | **~86×** |
| `deep_rec` | non-tail recursion (fib trees) | 0.01 | 0.30 | **~30×** |
| `variant_match` | variant build + `match` interp, 40K | 0.28 | 1.37 | **~5×** † |
| `list_fold` | cons build + fold, 600M cells | 5.19 | 17.71 | **~3.4×** |
| `rbtree_corpus` | Perceus rb-tree, 2M inserts | 2.36 | 4.63 | **~2.0×** |

The factor tracks **how much of the work is raw scalar vs. inevitable heap
allocation**. Pure arithmetic (all scalar) is ~86×; heap-bound workloads where
*both* backends pay the same `kaix_cons` / `kaix_variant_arg` allocation cost
compress to ~2–3×. This is the signature of an Int-boxing problem, not a
runtime-wide one.

> **† `variant_match` was reported at 40K rounds because the native binary used
> to scale *super-linearly* on this workload (now FIXED — issue #855, §3.4).**
> Before the fix, native wall time was N=10K → 0.30 s, 20K → 0.30 s, 40K →
> 1.37 s, **80K → 142 s** (a ~100× jump for a 2× increase in N) while the C
> binary stayed linear and peak RSS barely moved (24 MB → 34 MB) — the
> signature of the immortal-variant cache saturating, not a runaway leak.
> **After the fix, native scales flat:** N=10K → 0.31 s, 20K → 0.32 s, 40K →
> 0.32 s, 80K → 0.35 s, matching C's shape. The residual native-vs-C factor is
> now the *linear* all-boxed boxing gap (cause #1), the P1/P2 territory.

## 3. Root cause, with machine-level evidence

### 3.1 The native walk is all-boxed (cause #1, dominant)

`emit_native_ops.kai:16` states it outright: *"All boxed (KIR is all-boxed)."*
The arithmetic operator table (`emit_native_ops.kai:30`) maps `+` → `kaix_add`,
`-` → `kaix_sub`, etc. — the *boxed* runtime helpers that take and return `ptr`.

For `sum_loop(i, acc) = sum_loop(i-1, acc + (i*i - i/3))`, the recursive arm
of the native IR emits per loop iteration:

- **19 `kaix_*` calls** (8 `kaix_int` box + 5 `kaix_int_field` unbox + 5
  `kaix_sub`/`mul`/`div`/`add` + 1 `kaix_unit`), plus the loop-header test adds
  `kaix_int_field`/`kaix_int`/`kaix_eq`/`kaix_truthy`,
- **35 load/store** through `alloca` slots.

The O2-optimised native binary still issues **~30 `bl` (branch-and-link) per
iteration** to those helpers (`objdump`). The C-direct binary issues **ZERO**
`kaix_*` calls: O2 inlined `kai_sum_loop` away entirely into raw machine
arithmetic. The C emitter unboxes (`static int64_t kai_sum_loop(int64_t kair_i,
int64_t kair_acc)`, `goto`-TCO), so O2 has scalars to vectorise.

### 3.2 Why O2 cannot fix it

The boxed helpers are external symbols:

```
declare ptr @kaix_add(ptr, ptr)
declare ptr @kaix_int(i64)
declare i64 @kaix_int_field(ptr)
```

They are defined in `runtime_llvm.c`, compiled and linked **after** the
in-process O2 pass by `cc`. There is **no LTO**, so `default<O2>` sees opaque
call barriers: it cannot inline the body, cannot prove `kaix_add` is pure, and
cannot const-fold or unbox through it. The pipeline does its job — it just has
nothing it is *allowed* to optimise. mem2reg *does* clear the `alloca` traffic
(the loop runs in registers); the irreducible cost is the call-per-op barrier.

### 3.3 What is NOT the problem

Three things the IR rules out, so the plan does not chase them:

- **Not a leak / RC overcount.** Perceus reuse-in-place is present in native
  (`kaix_variant_reuse_at`, `kaix_internal_dup`/`drop` in the IR). The native
  rb-tree is garbage-free, same as C.
- **Not TCO.** Native `sum_loop` is already a loop (`br label %entry` backedge;
  no self-recursive `bl`). Simple self-tail-calls compile to a loop (#706). The
  loop body is full of `bl`s, but the loop *structure* is correct.
- **Not match dispatch.** Variant `match` lowers to a direct `switch i32` on
  `kaix_variant_tag_of` (O(1)), not a comparison chain.
- **Not heap re-box allocs.** Boxed `Int` uses Koka-style **tagged immediates**
  (`kai_tagged_int`, `runtime.h:520`): the re-box is a shift+bit-OR, not a heap
  allocation. So P1 below kills *inlining barriers*, not allocs.

### 3.4 RESOLVED: `variant_match` super-linear collapse (second finding) — issue #855

> **Status (2026-06-17): FIXED.** Root cause was the runtime's **immortal-
> variant cache**, not the allocator free-list / RC-bookkeeping / reuse-token
> candidates guessed below. N=80K went from >70 s to 0.35 s; native now scales
> **flat** in N, matching C. See `docs/lane-experience-native-variant-match-
> superlinear.md` for the full investigation.

The `variant_match` native binary ran *super-linearly* (table † in §2):
doubling the round count from 40K to 80K multiplied wall time ~100× while the C
binary stayed linear and peak RSS barely moved. The all-boxed gap (cause #1) is
*linear* — it could not explain a 100×-per-2× collapse against a near-constant
live set.

**Root cause.** The bench rebuilds `tree(i)` each round with leaves carrying the
variable index `i` (`Lit(i)`). The native all-boxed codegen boxes every Int
field and builds the variant via `kaix_variant` → `kai_variant_u` with `mask==0`.
That path consults the **immortal-variant cache** (`kai_slots_all_immortal_ptr`),
which counted a *tagged-Int immediate* slot as "immortal" — so `Lit(i)` for
**arbitrary** `i` was interned, one entry per distinct `i`, into a fixed
262144-bucket open-addressing table. An unbounded set of `Lit(i)` saturates the
table and degrades its linear probe to **O(n) per operation** → quadratic total.
RSS stays flat because the table is pre-allocated. The C backend never hits this:
it builds `Lit(k)` via `kai_variant_u_fast` with a typed `.i64` slot, bypassing
the `mask==0` cache.

**Fix (issue #855).** Two parts: (1) `kai_slots_all_immortal_ptr` now
*disqualifies* a variant whose slot is a tagged-Int immediate from
immortalisation (the load-bearing fix — closes the whole class); (2) the native
KIR match-lowering emits the owned-scrutinee match-exit drop it had been
skipping, so the no-longer-immortalised `Lit(i)` cells are reclaimed and the
live set stays constant. A follow-up lane can build `Lit(i)` with a typed `.i64`
slot in the native codegen (the C `_fast` path) to remove the representation
divergence at the source; the runtime fix then stands as defense-in-depth.

## 4. Optimisation plan (priority-ordered)

Reviewed by `asu`; the corrections below are folded in.

### P1 — Native Int unboxing (mode-slave for Int arith/cmp) · **SHIPPED** (#853)

**What.** Extend the existing `Real` raw-lowering machinery to `Int` arithmetic
and comparison, so a `MUnboxed` Int loop runs on native `add`/`sub`/`mul` +
`icmp`→`i1`→`condbr` instead of boxed `kaix_*` calls. The `unbox.kai` pass
**already** marks Int arith `MUnboxed` (`ty_is_unboxable: TyInt -> true`); the
information exists in the AST — the native walk just has to consume it.

**Where.**
- `stage2/runtime.h` — add `kai_llvm_build_ibinop` (`LLVMBuildAdd`/`Sub`/`Mul`)
  and `kai_llvm_build_icmp` (`LLVMBuildICmp` + `LLVMIntSLT`/`SGT`/…), mirroring
  `kai_llvm_build_fbinop`/`fcmp` (`runtime.h:11835`).
- `stage2/compiler/native_prims.kai` — register the two new C-API prims
  (alongside `llvm_build_fbinop` at line 67).
- `stage2/compiler/kir_lower_raw.kai` — promote Int arith/cmp to a raw FORM
  (`i+`/`i<` prims, analogous to `f+`/`f<`); mirror `kir_fbinop_op` /
  `kir_fcmp_pred`.
- `stage2/compiler/emit_native_ops.kai` — extend `nprim_is_raw` + `nemit_prim`
  (line 190) to dispatch the `i`-prefixed prims to the new builders.

**Three correctness guards the Real path did not need (asu):**
1. **No `nsw`/`nuw` flags** on `add`/`sub`/`mul`. kaikai Int arithmetic *wraps*
   (the C emitter casts through `uint64_t`); a `nsw` add would make signed
   overflow UB and mis-optimise. Emit plain wrapping ops.
2. **`/` and `%` emit raw `sdiv`/`srem` (P4, shipped).** Originally `/` and `%`
   stayed boxed in v1 because native `sdiv`/`srem` are UB on divide-by-zero
   *and* on `INT_MIN / -1`. P4 reversed this: the C-direct ORACLE already emits
   a bare `a / b` and accepts that same UB as a separate concern, so the native
   backend mirrors it (raw `sdiv`/`srem`) for byte-exact parity — a guard would
   DIVERGE from the oracle. This closes the `i/3` residual on `arith_runtime`
   (native to C parity). See `docs/lane-experience-native-div-rem-raw.md`.
3. **Lower `icmp` to a direct `i1`→`condbr`**, not just raw arithmetic. The
   comparison feeding an `if` should branch on the `i1` directly, skipping
   `kaix_eq` + `kaix_truthy`.

**Impact.** Removes the ~30-`bl`-per-iteration barrier from scalar loops. The
loop structure is already correct (§3.3), so the body simply empties of calls.
**Magnitude to be measured, not promised** (asu's honesty gate): re-run
`objdump` on the arith loop *after* P1 lands. If a `bl` remains per iteration the
win is 5–10×; if the body becomes pure arithmetic it approaches the ~86× ceiling.
Do not cite a figure in the changelog until measured.

**Risk.** Medium. RC discipline is the hazard the Real lane already solved: a raw
operand is never `decref`'d, so a multi-use Int never double-frees. The
`let`-binder Int raw path (`kir_lower_raw.kai:47`) is the live precedent. Does
**not** touch the boxed call/border ABI.

### P2 — Link the runtime into the module before O2 · **SHIPPED** (lane `native-bitcode-link`)

> **Shipped 2026-06-16** (`docs/lane-experience-native-bitcode-link.md`).
> Two structural corrections to the plan as written, both `asu`-reviewed:
> (1) the symbol model is a FULL link + internalize-except-`main` (Model X),
> not an `available_externally` graft — so the object is self-contained and
> the cc link line DROPS `runtime_llvm.c` (it does NOT stay identical; the
> literal "identical" reading was the unsound double-source one). (2) The
> bitcode is generated at BUILD TIME per-platform (clang-18, version-gated),
> NOT vendored — a single committed `.bc` mis-codegens across Mach-O/ELF
> because it encodes the build host's data layout. Measured: `list_fold`
> 1.21× and `rbtree` 1.05× faster than legacy native; `bl`-to-`kaix_*` per
> binary 4704 → 573. The residual is the boxed-cons `malloc`/slab tax, which
> P2 cannot remove (a future reuse-in-place / unboxed-cons lane).

**What.** Compile `runtime_llvm.c` (and the `runtime.h` helpers) to LLVM
**bitcode** and `LLVMLinkModules2` it into the in-process module *before*
`default<O2>` runs. Then O2 sees the *bodies* of `kaix_cons`,
`kaix_variant_arg`, the list spine — and can inline/specialise them. The
object becomes self-contained (the merged runtime is internalised + DCE'd
into it), so the final `cc` link drops `runtime_llvm.c`.

**Why this and not the alternatives (asu).** Rejected: (a) LTO via `cc` flags —
adds a toolchain dependency and a release-only failure mode; (b) emitting the hot
runtime ops *inline* in the walk — that is the double-source bug-class that
killed the text-LLVM backend (two copies of the same logic drifting). Bitcode
link is one mechanism, one source of truth, and turns O2 into an amplifier of P1.

**Impact.** Closes part of the ~2–3× residual on heap-bound workloads
(`list_fold`, `rbtree`) that P1 alone does not touch (their cost is `kaix_cons`,
not Int arith).

**Risk.** Higher than P1. Touches the link/module-assembly step. Needs the
bitcode to be built reproducibly in both dev and release (the static-LLVM
release path already vendors LLVM; bitcode is an added artefact).

**Telling whether P2 is on.** The opt-out stays clean by design — stage 0's
zero-dependency promise needs the fallback — but a host running without it
emits heap-bound native code several times slower than CI and the release do
(measured 2026-07-20 on `d425a904`, same kaic2, P2 the only variable:
`list_fold` 10.14 s → 16.83 s, `rbtree_corpus` 1.29 s → 4.87 s; arithmetic
that never calls a runtime op is unaffected). So every surface reports the
state:

```sh
tools/gen-runtime-bc.sh --status       # active | optout needs-regen | optout no-clang18
tools/gen-runtime-bc.sh --status-line  # the same state as one line, with the remedy
make KAI_LLVM=1 kaic2                  # last line of the build is the P2 banner
bin/kai --version                       # `native p2:` field
benchmarks/mn-throughput/run.sh         # `p2:` line in the environment banner
```

`optout needs-regen` means clang 18 resolves but the `.bc` was never built —
one `make KAI_LLVM=1 kaic2` away from active. `optout no-clang18` needs an
install. Distinguishing the two is what keeps a recoverable state from being
read as a native-codegen defect.

### P3 — alloca-everything → mem2reg · **do not pursue** (correctly not pursued)

> Not to be confused with the *shipped* "P3" in project memory / CHANGELOG,
> which is the raw-scalar-params lane (#857). This heading's P3 (re-implementing
> SSA construction in the walk) was correctly left alone — mem2reg gives it free.

The native walk emits every temporary to an `alloca` and relies on mem2reg to
promote to SSA. The arith-loop disassembly shows mem2reg *does* clear this under
O2 (the loop runs in registers). This is **already free from the O2 pipeline** —
re-implementing SSA construction in the walk would duplicate what mem2reg gives.
Leave it. (Listed so a future lane does not "discover" it as low-hanging fruit
and waste effort.)

## 5. What each item does NOT re-implement

Per the brief's constraint (do not re-implement what `default<O2>` gives free):

- P1 does *not* add an optimiser — it hands O2 *unboxed scalars* it can already
  vectorise/unroll. The optimisation is LLVM's; P1 only removes the opaque-call
  barrier.
- P2 does *not* add inlining — it removes the *no-LTO* barrier so O2's existing
  inliner can fire on the runtime bodies.
- P3 is explicitly *deferred to O2* (mem2reg).

## 6. Reproducing

```sh
# Build native kaic2 (LLVM 18 static, prebuilt under the main checkout):
make -C stage2 KAI_LLVM=1 \
  LLVM_CONFIG=<repo>/stage0/third_party/llvm/build/bin/llvm-config kaic2

# Battery (best of 3 per route):
RUNS=3 tools/native-perf/run.sh

# Single bench, with IR dump:
KAI_BACKEND=native KAI_NATIVE_DUMP_IR=/tmp/x.ll \
  bin/kai build tools/native-perf/benches/arith_runtime.kai -o /tmp/x

# Per-iteration call count in the optimised binary:
objdump -d /tmp/x | awk '/<_?sum_loop>:/{p=1} p&&/^[0-9a-f]+ <[^k]/&&!/sum_loop/{exit} p' \
  | grep -c '\bbl\b'
```

## 7. Scope of this lane

This lane **diagnosed and planned**. It shipped the benchmark harness
(`tools/native-perf/`) and this document; no codegen rewrite landed in *this*
lane. The opt-level "one-liner" the brief authorised does not exist (§TL;DR),
so no compiler code changed here. The codegen work the plan scoped then shipped
across follow-up lanes P1–P4 + §3.4 — see the closure-status table at the top.
Residuals: #858 (UAF), #860 (cons leak), and #861 (non-tail raw call re-box,
`deep_rec`) are all FIXED (the last via `b1fce7ed`, 2026-06-19). The remaining
native-vs-C gap is the heap-bound traversal tuning (`list_fold`, `rbtree`),
carried forward under Anakena.
