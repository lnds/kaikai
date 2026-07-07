# Koka-parity plan — closing the shared front-end gap (#1104)

Analysis lane, 2026-07-06. Read-only comparison of Koka's Perceus/reuse
implementation (source at `../koka`, commit `d5b946ec`, matching the
installed Koka 3.2.3) against kaikai's, grounded in fresh measurements
on the rb-tree bench (`benchmarks/rb-tree/`). No code was changed; this
doc is the deliverable.

## Baseline (measured 2026-07-06)

Darwin arm64, `cc -O2`, N=1,000,000 inserts, median of 7 interleaved
runs, kaikai `4eaffb4d` (v0.99.0), Koka 3.2.3. Instructions via
`/usr/bin/time -l` (host-dependent but reproducible on this machine).

| column | wall | instructions | peak RSS |
|---|---:|---:|---:|
| Koka (defaults) | 0.26 s | 1.041 G | 49.4 MB |
| Koka `--fno-reusespec` | 0.27 s | — | — |
| Koka `--fno-dropspec` | 0.32 s | — | — |
| Koka `--fno-reuse` | 0.54 s | 4.838 G | 49.5 MB |
| kaikai-c | 0.40 s | 2.488 G | 57.5 MB |

Two facts frame everything below:

1. **kaikai-c is faster than Koka-without-reuse** (0.40 vs 0.54).
   kaikai's baseline machinery — packed 48-byte cells, raw-`i64` Int
   slots, TRMC cctx, tagged-immediate colors, borrow-inferred `is_red`
   — is already at or past Koka-minus-reuse. The whole remaining gap
   fits inside the reuse/allocation-traffic story.
2. **Reuse is worth 2× to Koka on this bench** (0.26 vs 0.54, 1.04 G vs
   4.84 G instructions). Reuse *specialization* (field-match write
   elision) is worth ≈ 0 wall (`--fno-reusespec`: 0.27); *drop
   specialization* ≈ 0.06 s (`--fno-dropspec`: 0.32). The ablation
   flags are hidden `fflag`s (`koka/src/Compile/Options.hs:517-520`,
   defaults: reuse/dropspec/reusespec ON, borrow inference OFF).

## The smoking gun: allocation traffic

kaikai-c compiled with `-DKAI_TRACE_RC`, run at N=100,000
(`KAI_TRACE_RC=1` env), per-insert normalization:

| counter | total @100K | per insert |
|---|---:|---:|
| `alloc_total` (variant) | 523,643 | **5.24** |
| `reuse_freed` (tokens captured then freed) | 423,634 | **4.24** |
| `tok_unique` (arm-top tokens captured) | 1,578,121 | 15.8 |
| `tok_null_shared` | 0 | 0 |
| `incref_total` / `decref_total` | 1,118,570 / 1,118,582 | 11.2 each |

Koka allocates ≈ 1 fresh cell per insert by construction (the `Leaf →
Node` arm; every rotation and recolor rides reuse tokens — inferred
from the generated `rbbench.c`, not counter-measured; kklib release
builds carry no alloc counters). **kaikai allocates 5.24 cells per
insert and throws away 4.24 captured reuse tokens per insert** — ~5×
Koka's allocator traffic, all of it malloc/free pairs on the hot path.

Where the 4.24 wasted tokens come from is visible in the emitted C
(`stage2/kaic2 --path stdlib --path examples/perceus
examples/perceus/rb_tree_bench.kai`): in `insert_loop`'s Black arm, the
`is_red(child)` branch tails into
`balance_left(insert_loop(l,k,v), kx, vx, r)` — an out-of-line call.
The arm-top token `_arm_ru` (captured by `kai_drop_reuse_token`,
`stage2/runtime.h:4667`) cannot cross the call boundary, so the arm
exit frees it (`kai_reuse_free`, `stage2/runtime.h:4744` — whose own
comment names exactly this shape). The `is_red` guard fires on roughly
a quarter of the ~16 unique descent steps per insert: 4.24 balance
calls per insert, each freeing the caller's token and doing fresh
allocations inside `balance_left`/`balance_right` instead.

## What Koka does that we do not

### 1. Inline-before-Perceus makes rotation tokens intra-procedural

Koka's pipeline runs the Core inliner well before the backend RC
passes: unroll → **inline** → simplify → specialize → TRMC
(`koka/src/Compile/Optimize.hs:73-81, 107`), and only then, inside the
backend, box → `parcCore` → `parcReuseCore` → `parcReuseSpecialize`
(`koka/src/Backend/C/FromCore.hs:93-98`). By the time reuse analysis
runs, `balance1`/`balance2` are already inlined into `ins`: the
generated `rbbench.c` shows the rotation match *inside*
`_trmc_unroll_ins`, holding **three** live reuse tokens (`_ru_x110`
from the descent node, `_ru_x112` from the balance scrutinee,
`_ru_x111` from the inner red node) and rebuilding all three rotation
cells with zero allocation:

```c
_x_x178 = kk_rbbench__new_Node(_ru_x112, ...);
_x_x179 = kk_rbbench__new_Node(_ru_x111, ...);
_x_x177 = kk_rbbench__new_Node(_ru_x110, ...);
```

kaikai's reuse machinery is per-match-arm and syntactic: the KIR-side
nested-rotation plan (`stage2/compiler/kir_lower_reuse.kai`) and the
C-backend recogniser get 2-of-3 cells *inside* `balance_left` (outer
overwrite + inner token steal — visible in the emitted C), but the
third cell is a fresh `kai_variant_u_fast` alloc, and the caller's
token dies at the call boundary. Per rotation vs Koka: +1 alloc,
+1 token free, plus the RC slop below.

### 2. An `Available` token pool threaded through the function body

Koka's reuse pass carries `type Available = M.IntMap [ReuseInfo]` — a
size-keyed pool of donatable cells (`koka/src/Backend/C/ParcReuse.hs:621`)
— registered per deconstructed pattern (`ruPattern`, ParcReuse.hs:322-341),
threaded through lets with branch intersection (`ruLet`,
ParcReuse.hs:194-205; `ruBranches`, ParcReuse.hs:296-303), and consumed
by *any* later constructor of the same byte size (`ruTryReuseCon`,
ParcReuse.hs:369-384). A token from one pattern can pay for a
constructor several bindings away, in a different sub-expression.
kaikai has no equivalent pool: a token exists only inside the single
arm-shape the recogniser matched.

### 3. Per-branch liveness cancels dup/drop pairs; ours leaves residue

Koka's Parc computes live sets in reverse per guard, dups only when a
variable is still live or borrowed (`useTName`,
`koka/src/Backend/C/Parc.hs:550-560` — last use is a free move), fuses
dup/drop pairs set-theoretically (`fuseDupDrops`, Parc.hs:405-423), and
drops what a scope owns but doesn't use (`ownedInScope`,
Parc.hs:984-990). Net RC ops in the unique descent path: **zero**.

kaikai's emitted `balance_left` shows the residue of our
heuristic-based pass (`stage2/compiler/perceus.kai`): every arm does
`kai_internal_dup(kai_r)` and the function exit does
`kai_internal_drop(kai_r)` (each arm uses `r` exactly once — per-branch
liveness would make each a move); the reuse path ends in
`_r = kai_incref(_scr)` compensating a blanket match-exit
`kai_decref(_scr)`. That is ~4 avoidable RC ops × 4.24 balance calls +
assorted pairs elsewhere = the measured 11.2 increfs/insert where
Koka's unique-dominated run does approximately none. At ~2-5
instructions per inlined RC op this is second-order (~100-200 M
instructions of the 1.45 G excess) — real, but not the lever.

### 4. What we already match (verified, not assumed)

- **Cell layout**: 48-byte Node, 8-byte packed header — explicit Koka
  mirror (`stage2/runtime.h:430-450`). RSS 57.5 vs 49.4 MB is allocator
  slack, not layout.
- **Raw Int slots**: keys/values read as `.i64` directly in the C
  backend match arms; Koka uses `kk_integer_t` value-immediates with
  borrow-comparisons (`kk_integer_lt_borrow`). Parity. (The kind-1 raw
  *binder* gap is native-backend-only — see Lane N below.)
- **TRMC cctx**: `kai_cctx_extend_linear` / `apply_linear` are inline
  structs, a direct port of Koka `types-cctx.h`
  (`stage0/runtime.h:3729-3753`). No allocation, parity.
- **Drop-guided token capture**: `kai_drop_reuse_token`
  (`stage2/runtime.h:4667`) is a faithful `kk_block_drop_reuse`
  (`koka/kklib/include/kklib.h:818-829`) port, modulo rc==1 vs rc==0
  uniqueness (`kklib.h:306-308`) and extra tag/arity checks.
- **Borrow inference for inspectors**: `is_red` is borrow-inferred
  (`stage2/compiler/perceus.kai:617-646`); no dup at its call sites.
  Koka ships borrow *annotations* plus an off-by-default inference flag
  (`binference`, `koka/src/Compile/Options.hs:517`).
- **Field-match write elision** (ParcReuseSpec's `tryMatch`,
  `koka/src/Backend/C/ParcReuseSpec.hs:171-182`): kaikai's TRMC
  in-place path already skips unchanged key/value/sibling slots. Koka's
  own ablation prices the full mechanism at ≈ 0 wall here — not a lever.

## Levers, prioritized (impact × cost, each with its measurement)

### Lane 1 — donate the arm token across the balance boundary (THE lever)

**Claim**: recovering the 4.24 wasted tokens/insert takes alloc traffic
from 5.24/insert to ~1.1/insert and removes the paired frees. Model
estimate: roughly half of the 1.45 G excess instructions (allocator
round-trips + `kai_variant_u_fast` slot copies + attendant RC slop).
Predicted wall 0.40 → ~0.30-0.33. **Estimate, not measured — the
acceptance gate is the counters, not the prediction.**

Two mechanisms, spike decides:

- **(a) Koka's route**: a pre-Perceus inliner for small leaf functions
  (`balance_left`/`balance_right` inline into `insert_loop`), then
  extend token threading so an enclosing arm's token reaches the
  nested rebuild (a minimal `Available`-set: size-keyed, intersected
  across branches — ParcReuse.hs:621 is the reference design). Precedent
  is exact; cost is a new pass + recogniser generalization.
- **(b) kaikai-specific**: interprocedural token passing — compile
  `balance_*` with a hidden reuse-token parameter when the call sits in
  an arm tail that holds an unconsumed token. No Koka precedent,
  smaller blast radius, but bakes an ABI decision.

**Measurement (before/after, both backends)**:
`-DKAI_TRACE_RC` + `KAI_TRACE_RC=1` at N=100K:
`reuse_freed`/insert 4.24 → < 0.1; `alloc_total`/insert 5.24 → ≤ 1.5.
Wall: `benchmarks/rb-tree/run.sh` (median-of-7 interleaved).
Instructions: `/usr/bin/time -l` (Darwin) and the Docker callgrind
recipe in `benchmarks/rb-tree/README.md` for the C backend.
Gates: serial parity (`BACKEND_PARITY_JOBS=1`) and the native
self-host gate — this is an RC-discipline change.

### Lane 2 — fuse the residual RC pairs in the legacy match protocol

Port the two cheap pieces of Parc's discipline into the paths the KIR
walk does not cover (the `pcs_` recogniser output, e.g. `balance_left`):
single-use-per-branch params become moves (no dup + exit-drop), and the
match-exit scrutinee decref is elided when every arm consumes the
scrutinee via token/decref (killing the compensating
`_r = kai_incref(_scr)`).

**Measurement**: `incref_total + decref_total` per insert 22.4 → < 4
(same trace build); wall delta expected small (~2-5%) — report it
honestly even if ~0, the counter reduction is the deliverable since it
compounds on non-rb-tree workloads.

### Lane 3 — per-level protocol width (deprioritized until 1+2 land)

`kai_drop_reuse_token` performs null/is_value/tag/arity/rc checks where
Koka's `kk_block_drop_reuse` is an rc compare (kklib.h:818); the TRMC
in-place write re-stamps color slot, `variant_tag`, and `rc` even when
constructor and color are unchanged (Koka's reuse write touches only
the changed field, generated `rbbench.c` `_con_x162->lchild = ...`);
`kai_variant_at` (`stage2/runtime.h:4703`) rewrites all 5 slots. ~16
unique tokens/insert × a handful of instructions each ≈ 300-500 M
instructions upper bound, but Koka's `--fno-reusespec` ablation (≈ 0
wall) warns this class of saving often hides under the memory system.

**Measurement**: instruction count is the only honest probe (wall will
not resolve it); do it after Lane 1 so the balance noise is gone.

### Lane N (native backend only, already tracked) — kind-1 raw binder + shim boundary

The C backend already reads Int slots raw; the native backend's
deferred kind-1 raw binder (#1102 revert, #709 phantom-box class) and
the `kaix_*` call boundary are what separate 1.92× from 1.60×. Needs
border-reboxing to be sound. This is not part of the shared front-end
gap and stays in the native perf lane.

## Attack order

1. **Lane 1 spike** — 2-3 day probe of mechanism (a) vs (b) on the
   rb-tree shape only, gated by the `reuse_freed` counter. Pick the
   winner, then the full lane with parity + self-host gates.
2. **Lane 2** — independent of Lane 1's mechanism choice; can run as
   the following lane using the same trace harness.
3. Re-measure the full table (this doc's baseline section) after Lanes
   1+2; decide whether Lane 3 is worth a lane at all.

Prediction to hold me to: Lanes 1+2 should land kaikai-c between 0.28
and 0.33 s on this machine's table (1.1-1.3× Koka), from 0.40 today.
If Lane 1 closes `reuse_freed` to ~0 and the wall does NOT move below
~0.36, the model above is wrong and the residual must be re-profiled
before any further lane — do not proceed on momentum.

## Measurement protocol notes

- Wall: `benchmarks/rb-tree/run.sh` (interleaved median-of-7) is the
  canonical harness; the numbers here used the same discipline.
- Instruction counts: the historical callgrind-in-Docker recipe applies
  to the **C backend only**; the retired llvm-text backend's callgrind
  path does not transfer to the in-process native backend (objects are
  emitted in-process, nothing for callgrind to `-g`-compile). For
  native, `/usr/bin/time -l` on Darwin is the working proxy; a
  Linux-perf recipe for the native binary is an open follow-up.
- RC counters: compile the emitted C with `-DKAI_TRACE_RC=1` and run
  with `KAI_TRACE_RC=1`; the report prints alloc/free/token/incref
  totals at exit. This works today, no new instrumentation needed.
- Koka reference ablations: `koka -O2 --fno-reuse|--fno-dropspec|--fno-reusespec`
  (hidden flags, verified on 3.2.3). Note `--fno-reuse` still leaves
  drop-specialization's `is_unique` dual branches; it removes token
  capture/`alloc_at` only.
