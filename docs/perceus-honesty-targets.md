# Perceus — honesty targets

What "Perceus done" means concretely, in three tiers of honesty, and
which follow-ups belong to which claim. This is a **scope decision**,
not a roadmap. It is the lane-close discipline's job to keep this file
matching reality; when a lane retro disagrees with this file, this file
wins (see CLAUDE.md *Primary docs vs. lane bitácora*).

Sister doc: `docs/fibers-honesty-targets.md` (same shape, applied to
the m8.x scheduler).

## Where we are today (refreshed 2026-06-03, HEAD `99e4f90`)

The honesty claims are all met for emitted user programs:

- **Tier 1 #2 "runtime-efficient"** — honest without footnotes. The
  canonical Perceus workload (red-black tree, 1 M random inserts) runs
  at **2.64× C wall, 1.17× C RSS, garbage-free** (`free_total ==
  alloc_total`, the spine is fully reclaimed). Against Koka 3.2.3 on
  the same machine + keystream + algorithm: **2.36× Koka** (Koka is
  1.12× C). Full numbers: `docs/benchmarks/rb_tree_2026-06-02.md`.
- **Compute parity** — fib(35) 1.00× C, Euler #4 1.05× C
  (`docs/benchmarks/compute_2026-05-09.md`).
- **Tier 1 #3 "mandatory TCO"** — honest for emitted programs (C
  backend; see the TCO section below for the bootstrap-chain and LLVM
  caveats that survive).

> **v1 status (2026-06-17):** the block below is the **2026-06-07 LLVM-text
> backend** snapshot (`--emit=llvm`, a flag that no longer exists). That
> backend was **removed (#850)**; the in-process **native** backend
> (`--backend=native`) is now the default. Its bitcode-link (P2, #854) already
> merges the runtime module before O2 — the "build-chain Tier-2 lane" this note
> says the gap "needs". Current native-vs-C numbers and the live residuals live
> in `docs/native-codegen-perf-plan.md` (scalar at C parity; #860 cons leak
> FIXED — `free_total` now cascades as the C oracle; #861 non-tail re-box
> remains). The block below is retained for the RC-discipline
> genealogy (#747), not as a current perf claim.

> **Backend note (LLVM RC, 2026-06-07, #747 CLOSED — historical):** the numbers above
> are the **C backend** (the primary, behind the honesty claims). The
> **LLVM-text backend** (`--emit=llvm`, since removed in #850) had never implemented the Perceus match
> RC discipline at all — it leaked every scrutinee and boxed slot in every
> match (rb-tree: `frees=0`, 3.7 GB RSS). PASO 1 (PR #756) ported the C RC
> protocol + the unified stage2 runtime (slab + tagged-Int); the rb-tree
> went garbage-free and RSS reached parity (55.4 vs 55.2 MB @ 1M). #747
> then ported the **i64-inline representation**: Int variant slots ride
> raw `i64` (construction `kaix_variant_masked` + `[n x i64]` buffer +
> int-only `slot_mask`; read `kaix_variant_arg_i64` straight from `.i64`;
> reuse-in-place `kaix_variant_at_masked`; literal-Int pattern test via
> `icmp eq i64`), plus the **fast ctor paths** (`kai_variant_u_fast` with
> startup payload-ctor registration, and `kaix_nullary_fast` array-load
> with a startup nullary seed). Result (callgrind @ 100K): LLVM
> **1,331.93M → 897.15M (4.87× C)** in the production separate-compile path,
> **534.87M (2.90× C)** with the shim module merged (`llvm-link` / `-flto`);
> −60% from baseline. Correctness exact (height == C), RSS in parity,
> selfhost byte-id on both backends, ASan-clean. Gated by `test-issue-747`
> (asserts the raw representation) and `test-issue-747-mixed-slots` (the
> `k == 1` Int-only coherence gate vs Real/enum/ptr slots).
>
> The residual gap (2.90×–4.87× depending on link) is the `kaix_*` shim
> **call boundary** (the LLVM backend keeps `%KaiValue` opaque and routes
> every cell access through an external shim, where the C backend inlines
> the whole runtime in one TU) plus the structurally heavier `insert_loop`
> codegen. Closing it needs the production link to merge the shim module
> (`-flto` / `llvm-link`) — a **build-chain Tier-2 lane**, measured on
> Linux (the wall is cache-bound on Mac and does not move with LTO there) —
> not a codegen change. The design ceiling is ~1.5× C (the same the C
> backend hits vs hand-written C), not 1.0×: the opaque-IR / one-runtime /
> two-ABI invariant forbids inlining cell-interior access into the IR.

The 2026-05 doc generation cited the rb-tree at **12.6×–15.9× C**.
**Those numbers are dead.** The reuse-in-place cascade of 2026-06-01/02
(four perf commits, below) moved the wall ~5.4× and the RSS ~5.3×. Do
not cite the old figures.

### The reuse-in-place cascade (2026-06-01 → 2026-06-02)

Each step cited from the commit that shipped it (measured on rb-tree,
1 M inserts):

| Commit | Lever | Effect |
|---|---|---|
| `c44a445` (#741) | Koka tagged-Int + reuse-token + borrow | 14.78× C → ~5× C wall |
| `5ca630d` | arm-binder move + linear reuse-token | 6.2× C → 4.4× C wall |
| `141b1b7` | consume-map + tl-gated cons-overwrite | RSS **38.6× C → 2.29× C** (live-peak 1920 → 114 MB); reuse_in_place 5.6 M → 19.6 M |
| `e345393` | borrow-pure unique branch (Koka move model) | incref/decref −43%; wall 0.63 s → **0.54 s** |

After the cascade `free_total == alloc_total` holds at HEAD — Perceus
is garbage-free on this workload, which is why process RSS sits at
1.17× C rather than the 6.26× C of the pre-cascade baseline.

### Read-only array borrow (#1120)

`array_get` / `array_length` are borrow-annotated on their container
(arg0): the caller no longer dups the array to pass it, and a `_borrow`
runtime variant reads the slot without decref'ing the container.
Bulk-read of an `Array[Int]` (idiomatic tail-recursive `a[i]` sum) used
to pay a fixed **2 rc ops per access** (container incref + decref) that
the borrow-free Perceus model does not need — the array outlives every
borrow. Those per-access container ops now collapse to **0**; the
residual is the element escaping the array (owned by the caller), which
the boxing gap — not this lane — governs. Measured on a 10 M-element
sum: container `incref_total`/`decref_total` fell from `20 M` / `20 M`
to `0` / `0` (only the element's `10 M` incref remains). A container
read *only* through borrowed slots (never re-threaded to a consumer)
stays consuming so its ref is still reclaimed — the borrow fires on the
loop shape, where the recursive re-thread keeps the array alive.

### User-parameter borrow (#1127)

The borrow **surface** now extends to **user function parameters**: `^` on
a `pub` parameter (calling convention = ABI, serialized in the module
interface), plus the pre-existing conservative inference (the `is_red`
shape) on non-`pub` functions. The emit soundness gates that make a
borrowed scrutinee safe on the native backend (arm-move / goto-tail / reuse
recogniser inhibited, TCO dropmask unions the borrow set) ship too.

The **closure borrow (HOF win) shipped in #1130**: the runtime
`kai_apply_borrow` / `kaix_apply_borrow` invokes a closure without consuming
it, so a `^`-borrowed function-typed parameter routes its indirect call to the
borrowing variant. `foldl` / `foreach` / `any` / `all` and the other self-
recursive HOFs pay zero closure RC per element (borrow-through); `map` /
`filter` and the loop-delegating HOFs take one dup-on-consume at the owned
private-loop entry (rule 3, Koka `parcBorrowApp`) — one incref per HOF call,
not per element. The decision is local (uses only the callee's borrow-map
positions), so it survives the sep-comp oracle that #1127's whole-program
relaxed inference did not.

> **v1 status (2026-07-08):** one thing is still DEFERRED:
>
> - **Relaxed read-path inference.** A relaxed callee-discipline rule
>   (children-of-borrowed, borrow-through) passed every single-TU gate but
>   crashed `test-modular-selfhost` (the sep-comp compiler UAF'd compiling a
>   real program). The emit is not a pure function of the whole-program
>   borrow map — it rides a decl-grouping / per-partition-linkage invariant
>   the monolithic TU only guarantees incidentally. Reverted to the
>   conservative rule; reopening gate is the modular fixture in red. (A fresh
>   borrowed closure passed to a HOF also leaks one bounded ref per call-site
>   — the caller drop-after-call reconciled with TCO is a #1130 follow-up.)

### The one remaining lever, and what is NOT the lever

The gap to Koka is now a **constant factor**, not a chasm, and the
instruction counts locate it precisely:

- kaikai retires **8.37 G instructions vs Koka's 1.09 G (7.7×)** for a
  bit-identical tree. Allocations are already at parity (6.30 M, an
  RC-traffic lever now exhausted) and reuse-in-place is firing
  (5.6 M → 19.6 M cells). **The gap is neither allocation nor missing
  reuse.**
- It is the boxed↔unboxed `Int` frontier: every key comparison and
  argument pass still routes a small `Int` through a `kai_int` heap
  box, where Koka uses a value-immediate (tagged pointer, register-move
  field read). Phase 3 signature-unboxing does not reach into the
  `RBNode`-with-pointer-children layout. **Value-immediate `Int` is the
  single remaining large lever.** It is post-MVP (Tier 3).
- Further reuse work is explicitly *not* the lever — the reuse retro
  notes C and Koka pay the same random-descent cache miss, so chasing
  `reuse_freed` does not move the wall.

## What does NOT work today (residual leak sources)

These leak on `kaic2` self-compile but do **not** surface below the
Tier 2 boundary (no demo a user would try in a tutorial hits them).

- **`kai_truthy` non-consuming** — intentional, not a bug. The LLVM
  short-circuit phi returns `lhs` in the early-exit branch, so consuming
  the truthy probe's argument would alias-free a value still referenced
  downstream.
- **`kai_prelude_*` C helpers that borrow** — `9fe6f6d` flipped 12 of
  them to callee-consume (print/eprint, int/real string conversion,
  array & list ops). The remaining hand-written prelude helpers in
  `runtime.h` still borrow rather than consume.
- **Stage 0 eager-dup retrofit** — `emit_ident_value` in `stage0/emit.c`
  still emits `kai_internal_dup` on every multi-use / captured /
  unresolved local read. The single-use, non-captured fast path landed
  in two steps (`7ab3d64` fn params, `b3b1e2f` lets + match-arm binds),
  keyed by binding identity so disjoint scopes don't collapse.
  Lambda-body lets are still on the brute-force path — a per-lambda-body
  counter is the follow-up.

The named architectural debt of early 2026 is **closed**:
`perceus_pass` multi-read let dup (`pcs_rewrite_estr_span`),
match-scrutinee real plug (stages 0/1/2), and `kai_field`/`pat_test`
balance (`kai_field_borrow`) all landed in the perceus-tier-2 lane
(2026-04-29). The #82 leak sweep (PR #123, 2026-05-02) closed the four
leak sources that issue catalogued. Branch-aware dup elimination
(#599 — the `pcs_pass` conservative-dup wall regression) is also
CLOSED.

## Tier 1 — *Show HN honest*

**Met.** The R1 atomic flip (v0.2.0) did the Show-HN work: 13 runtime
primitives consume their args linearly, `__perceus_dup`/`__perceus_drop`
wrappers, small-int + char cache. A live demo on unfamiliar code never
reaches the self-compile leak ceiling. Self-compile peak RSS is ~530 MB
post-Phase-3-unboxing (it was 3 GB at flip time). The two correctness
bugs that would embarrass a 5-minute browse — R2 (m8x_2 SIGSEGV,
`3553e9f`) and R3 (interp panic, `235638f`) — are closed. No Perceus
debt surfaces below the Tier 2 boundary.

## Tier 2 — *Production-honest 1.0*

**Met** for emitted user programs. Tier 1 #2 ("runtime-efficient")
holds without footnotes: rb-tree at 2.64× C / 1.17× C RSS / garbage-free
(above), compute at ~1× C. The architectural leak debt is closed; the
residual self-compile leak sources listed above are perf hotspots on the
25 k-line compiler self-compile, not honesty footnotes on user code.

Remaining Tier 2 polish (perf, not honesty):

| Item | Status |
|---|---|
| Remaining `kai_prelude_*` borrowing helpers | open — audit `runtime.h`, mirror the `9fe6f6d` 12-helper callee-consume pattern |
| Stage 0 eager-dup retrofit (lambda-body lets) | open — per-lambda-body counter to extend the `7ab3d64`/`b3b1e2f` fast path |

## Unboxing — Phases 1–4

| Phase | What it does | Status | Perf vs C |
|---|---|---|---:|
| Phase 1 — small-int + char cache | Cache hits for ints in `[-65536, 65535]` / chars `[0, 127]` reuse pinned slots; no alloc. Range widened from `[-128, 127]` in Phase 1.A (per-entry lazy warm: RSS scales with the working set, not the range). | ✅ landed (`69c6166`); widened 2026-05-29 | 50–100× (hits free, miss boxes) |
| Phase 2 — locals + return values unboxed | `Int`/`Bool`/`Char`/`Real` live in C `int64_t`/`int`/`double` inside fn bodies; boxing only at call boundaries and storage edges. | ✅ landed | 5–10× (call-boundary boxing) |
| Phase 3 — call boundaries unboxed (#383) | Top-level fns with all-primitive concrete signatures emit raw C signatures (`int64_t fib(int64_t)`); recursive sites pass raw scalars. | ✅ landed 2026-05-09 | **~1× C on compute** (fib 1.0×, Euler #4 1.05×) |
| Phase 4 Option A — variant field unboxing (#440) | Each user variant whose declaration spells `Int`/`Real` literally gets a typed payload slot in `slot_mask`; runtime walkers (free, eq, to_string, reuse) branch per-slot; all-pointer cells stay on the legacy hot path. | ✅ landed; superseded by the reuse cascade | folded into the 2.64× C rb-tree result above |
| Value-immediate `Int` (Tier 3) | Small `Int` as a tagged value-immediate instead of a `kai_int` heap box — the remaining 7.7× instruction gap to Koka. | ⏳ post-MVP | closes the 2.64× C → ~Koka gap |
| Phase 4 full unboxing (Tier 3) | Drop specialisation, regions, cross-fiber unboxed messages, type-erased layouts. | ⏳ post-MVP | matches C across the whole alloc mix |

Phases 2 + 3 are contained chunks that do **not** require the
multi-threaded scheduler / cross-thread atomics the full Koka feature
set needs.

## Tier 3 — *Full Perceus* (post-MVP)

The Koka feature set the m5 lane named "future milestone". Listed so it
doesn't accidentally get pulled into a 1.0-scoped lane.

| Item | Status / note |
|---|---|
| **Reuse-in-place** | **v1 landed #118, v1.1 #209 (2026-05-03); production-active since the 2026-06 cascade** (`c44a445`→`e345393`). Runtime helpers `kai_reuse_or_alloc_{cons,record,variant}` + `kai_check_unique`; recogniser + magic-name dispatch in the compiler. Fires 19.6 M cells on rb-tree at HEAD. The "self-compile firing remains 0" note from the v1.1 retro is obsolete — the cascade activated it on the linear-rebuild shape. |
| **Value-immediate `Int`** | post-MVP. The single largest remaining lever (7.7× instruction gap to Koka, all boxing traffic). The one named in the 2026-06-02 bench as what moves 2.64× C toward Koka's 1.12× C. |
| **Drop specialisation** | Investigated 2026-05-01 — closed doc-only: end-to-end working (selfhost byte-identical, tier1 green) but −1.7% wall at `-O2`, +5.4% at `-O0`. Phase 2 unboxing already absorbed the addressable overhead. Re-evaluate after the alloc-mix shift from reuse stabilises. Retro: `docs/lane-experience-drop-specialisation.md`. |
| **Opt-in regions** | Arena allocation for parser scratch / lexer state. Power-user feature; only where RC demonstrably costs more than an arena reset. |

CLAUDE.md keeps "Full Perceus (value-immediate Int, regions) is
post-MVP" pinned alongside the multi-threaded scheduler decision.

## TCO half of Tier 1 #3 — landed 2026-04-30 (issue #37)

`tcrec_rewrite_decls` (between unboxing and Perceus) rewrites every
self-tail-call's callee `EVar` into a `__kai_tcrec|<c_sym>|<dropmask>|…`
sentinel; the C backend emits a rebind+goto block at each call site and
plants `_kai_<c_sym>_entry:;` before the enclosing return. Verified:
`euler4` runs under `ulimit -s 256` on macOS (recursion depth ~1 M), and
the PR #36 runtime `RLIMIT_STACK` bump was removed.

Caveats that survive:

- The rewrite lives in `stage2/compiler.kai`, so it covers **emitted
  programs** (demos, kaic2's selfhost output). The bootstrap chain
  `kaic0 → kaic1 → kaic2` is built by stage 0 / stage 1, whose emit
  still uses the recursive shape, so the kaic2 binary's internal loops
  remain stack-bound (hence the macOS 128 MB linker stack in
  `stage2/Makefile`). The honesty claim applies to kaikai *source
  programs*, not the bootstrap chain.
- Mutual tail-recursion (`f → g → f`) is out of scope — v1 rewrites
  self-recursive calls only.
- **The LLVM backend lowers the sentinel to a real loop** (`br
  %tcrec.loop` back-edge, issue #706), including for mixed raw/boxed
  param signatures: a `String` (or any boxed) accumulator threaded
  through a self-tail-call round-trips as `%KaiValue*`, matching the C
  backend. (Earlier the LLVM UFn convention read a boxed param as a raw
  `i64`; fixed by threading the per-param raw mask through every UFn
  lowering site — see `docs/lane-experience-llvm-tco-mixed-param.md`.)
- Fns with any `LUUnused` parameter keep the normal-call shape (the
  entry drops would re-fire each goto iteration). Hoisting entry drops
  above the label is a follow-up.

## What this document is NOT

- **Not a calendar.** No cost estimates: gate by soundness +
  verifiable measurement, never by days.
- **Not a list of all Perceus follow-ups** — only those that affect an
  honesty claim. Internal scaffolding lives in the closed `perceus`
  issues (#77, #78, #82→#123, #440, #599).
- **Not an excuse to defer Tier 2** — Tier 2 shipped. The flip's wall
  regression was reversed by Phase 3 unboxing (#383) + the #82 sweep
  (#123) + the 2026-06 reuse cascade. What remains is the
  value-immediate `Int` lever (Tier 3), which is performance toward
  Koka, not an honesty footnote.
