# Lane experience: LLVM↔C parity (rb-tree)

Branch `llvm-c-parity`, from `main` @075b702. Goal: bring the LLVM backend
to performance parity with the C backend on the rb-tree benchmark
(`benchmarks/rb-tree/run.sh`, N=1M): wall ≤ C backend, RSS ≈ 55 MB.

## Anchor (baseline @075b702)

The harness `run.sh` only measures the C-emit (`kaikai` column); the LLVM
column is reproduced by hand: `kaic2 --emit=llvm f.kai > f.ll; clang -w
-O2 -I stage2 -I stage0 f.ll stage0/runtime_llvm.c -o bin -lm`. Measured:

| | wall (N=1M) | RSS |
|---|---|---|
| C hand | 0.29s (1.00x) | 47 MB |
| kaikai-C-emit | 0.45s (1.56x C) | 55 MB |
| **LLVM (baseline)** | **2.93s (8.4x C)** | **195 MB** |

## PASO 0 — closed by measurement, not code

The brief feared a pre-existing arg-pass leak scaling with N that would
contaminate measurement. Measured: the rb-tree LLVM leak is **constant
~27 cells across N** (1K→28, 10K→27, 100K→27). It does not scale, does
not contaminate. The rb-tree is garbage-free (its scrutinees are
reconstructed). The 195 MB came from malloc-per-Int (184654 allocs at
N=10K vs 44749 in C) + no slab, NOT a leak. PASO 0 was a non-issue for
this shape — the brief over-estimated it.

## PASO 1 — unified runtime (commit 0642ac2)

The two backends share the WHOLE front-end (one perceus pass, identical
RC/reuse decisions). The only asymmetry was the runtime each linked:
emit_c → stage2/runtime.h (tagged-Int immediates + Koka slab + reuse
tokens); emit_llvm → stage0/runtime.h via the `runtime_llvm.c` shim
(heap-everything, one malloc/Int, no slab).

Fix: the shim now `#include <runtime.h>` (angle brackets — quotes bind to
the sibling stage0/runtime.h regardless of `-I`). Link lines put `-I
../stage2 -I ../stage0` (28 LLVM lines in stage2/Makefile via a targeted
perl-insert; `-I $RUNTIME_INC_C -I $RUNTIME_INC` in bin/kai). Swept every
raw `->as.i` in the shim to `kai_intf` (tagged-immediate-safe). `main`
already shipped stage2/runtime.h in the release (@8d15275).

Soundness precondition (verified): the IR declares `%KaiValue = type
opaque`, all 1056 GEPs are over `%KaiValue**` (never a cell interior),
zero inttoptr/ptrtoint. One runtime behind two ABIs is sound — the IR
cannot observe the value representation; only the shim reads Int payloads,
and that now goes through `kai_intf`.

Result: **RSS 195 → 55 MB, at parity with the C backend.** Wall 8.4x →
~5x C-hand (the malloc-per-Int gone). Verified: LLVM selfhost byte-id, C
selfhost intact, #747 garbage-free, ASAN clean.

## PASO 2 first token — arm-top reuse / spine (commit f3918b7)

After PASO 1, RSS was at parity but wall was 3.06x the C-emit because
`reuse_in_place=0` on the LLVM path (vs ~1.2M in C). The TRMC step
fresh-allocated every spine node; the C donates the consumed scrutinee as
a Koka ParcReuse token (`kai_drop_reuse_token` + `kai_variant_at`).

Ported the arm-top token model to emit_llvm, in lock-step with the SHARED
perceus pass (no backend branch in perceus — the move/dup decision is
read off the AST signature both backends share, via the now-`pub`
`trmc_body_has_step` / `arm_ptr_binders` / `arm_moved_binders`):

- shim forwarders `kaix_drop_reuse_token` / `kaix_variant_at` /
  `kaix_reuse_free` / `kaix_variant_arg_borrow`.
- an arm with a TRMC body consuming a PVariant whose ptr binders perceus
  MOVED steals the scrutinee shell as a token: borrow-bind children (no
  incref), `kaix_drop_reuse_token`, and on the SHARED branch dup only the
  moved children.
- the TRMC step donates the token via `kaix_variant_at`; an unconsumed
  token is freed with `kaix_reuse_free`.
- a `%_pcs_scr_live` alloca mirrors the C `_scr = NULL`: an arm that stole
  the scrutinee neutralises the match-exit drop so it can't cascade-decref
  the moved children.

Result: **wall 3.06x → 2.64x the C-emit**, RC balanced (leaked constant),
RSS unchanged at 55 MB, reuse fires (tok_unique tracks the C backend
exactly: 1578121 @ N=100K, identical).

### Bugs found along the way (each cost a rebuild+test cycle)

1. naive `kaix_reuse_or_alloc_variant(mask=0)` for has-prim ctors → leaked
   123386 (that helper's eager-1:1-decref double-frees non-bijective
   rebuilds; the C uses the token model, not that helper).
2. match-exit drop double-freed the stolen token → fixed with the
   `%_pcs_scr_live` alloca.
3. `%t` reg collision (two `llvm_fresh_reg` off the same `e`) → reorder.
4. `alloca` emitted with a label name (no `%`) → `llvm_fresh_reg`.
5. `#747` reported a false LEAK (`live=91921`): `kai_reuse_free` bypassed
   `kai_free_value` and never bumped the per-tag free counter, inflating
   the diagnostic `live` by the reuse_free count. Fixed in
   stage2/runtime.h (bump `kai_rc_free_by_tag` under KAI_TRACE_RC). RSS
   was always flat (2.8 MB) — pure accounting.
6. `pub` privacy: emit_llvm calling emit_c module-private fns broke
   selfhost's module resolver → marked the 3 helpers `pub`.

### Mandatory fixture

The rb-tree and #747 are perfectly linear (every token UNIQUE,
`tok_null_shared=0`), so they never exercise the SHARED branch.
`examples/perceus/llvm_arm_top_reuse_shared.kai` (+ target
`test-arm-top-reuse-shared`) keeps the base tree alive and inserts twice →
shared nodes → SHARED branch fires (`tok_null_shared=22`), C == LLVM,
ASAN clean.

## PASO 2 second token — balance reuse: attempted, reverted, follow-up

The remaining 2.1x alloc gap (LLVM 1.11M vs C 0.52M @ N=100K) is ALL
balance (the spine is done — `tok_unique` identical). asu predicted the
"outer-cell reuse of balance" (token a) was transportable 1:1 with no new
risk. **That was wrong.** balance is NON-BIJECTIVE: the rebuild `Node(R,
Node(B, lx,..), ky,vy, Node(B, ry, k,v, r))` reorders children between
slots (lx/ry come from nested sub-cells). Stealing the outer cell + borrow-
binding the reordered children CORRUPTS the tree — `panic: non-exhaustive
match` (a node ends with a tag no arm matches). The C handles it with the
full `emit_match_arm_reuse_variant` machinery (~150 lines:
nonbij/borrow_pure/old_drops + a SECOND nested token via
`rv_inner_decision` + temps-before-overwrite + string-surgery
`replace_first` for the inner donate). Porting that sound to the IR (no
comma-expr, no string-surgery) is a lane of its own; the naive token (a)
is NOT a safe subset — (a), (b), and the non-bijective handling are
inseparable for balance.

Reverted to f3918b7. Open as a follow-up: **LLVM balance-rotation reuse
(non-bijective variant reuse-in-place)**.

## Honest gate

The verifiable lane gate is `reuse_in_place`/`alloc_total` PARITY with the
C backend, NOT wall ≤1.0x. The wall residual after spine parity has two
components, both separate dependencies: (1) balance reuse (the follow-up
above), and (2) runtime convergence — the shim routes variant builds
through `kai_variant_u`; routing through the inline `kai_variant_u_fast`
was MEASURED SLOWER here (1.20 → 1.83s), so it is NOT the floor asu
guessed, and closing it needs more than a one-line swap. The first token
achieves SPINE parity with the C backend; balance parity is the follow-up.

## Cost vs estimate

PASO 0 (measurement) + PASO 1 (runtime unify) landed cleanly and fast —
the soundness analysis (opaque IR) made the unification a mechanical
`-I`-order + `->as.i`→`kai_intf` sweep. The first token took ~6 RC sub-
bugs but converged (the shared perceus signature is what kept move/dup in
lock-step). The second token (balance) did not fit a sound acotado attempt
— it is categorically harder (non-bijective + nested token + aliasing) and
is correctly a separate lane, not a phasing stall within this one.
