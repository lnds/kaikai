# Lane experience — native-tco-lambda-param

**Lane:** `native-tco-lambda-param`
**Date:** 2026-06-15
**Scope:** Close the native-parity gap `examples/effects/issue_668_map_large_in_fiber.kai`
— `list.map`/`filter`/`flat_map` over 40 K elements **inside a fiber**
(64 KiB stack) crashed with `fiber stack overflow` under the in-process
libLLVM native backend (KIR Lane 1.5), while C-direct printed `total: 120000`.
Deterministic, not flaky. This was the LAST TCO-shaped gap on the ratchet.

## Scope as planned vs. as shipped

**Planned (brief):** the TCO that detects a self-tail-call and emits
`KTcrecGoto` (the O(1)-stack goto-loop) is DISCARDED when the tail-recursive
fn has a lambda/closure parameter; find where the detector rejects the lambda
param and make it lower to `KTcrecGoto` like the no-lambda case.

**Shipped:** the brief's hypothesis was WRONG, and the diagnosis it was sure of
("TCO no dispara con parámetro lambda") did not hold up at the first dump. TCO
**does** fire for a lambda-param tail loop — the KIR for `apply_loop`/`map_loop`
contains `tcrec-goto` exactly as for the no-lambda `copy_loop`. The overflow had
nothing to do with the tail-call detector. The real root is one layer down in
codegen: **the `[n x ptr]` argument buffer for the lambda call (`kaix_apply`)
was `alloca`'d in the loop-body block, so the `tcrec` goto-loop re-executed the
`alloca` every iteration and grew the stack N times.** The fix hoists that
alloca to the function entry block; the KIR and the tail-call detector are
untouched.

## Diagnosis path (where the brief's certainty broke)

1. Reproduced: fixture native → `fiber stack overflow`; C-direct → `total: 120000`.
2. Minimal repro `apply_loop(xs, f, acc)` (EMatch + lambda param) on the
   **OS stack** (200 000 elems) → native ran fine, no overflow. First crack in
   the brief: if TCO didn't fire, 200 000 frames would overflow even 8 MiB.
3. `--emit=kir` dump of `apply_loop` and stdlib `map_loop`/`flat_map_loop`: ALL
   carry `tcrec-goto _kai_<sym>_entry {...}`. **TCO fires with a lambda param.**
   The brief's whole premise was false.
4. Bisected the fiber case: `build` (self-tail, no lambda) inside a fiber → OK;
   `copy_loop` (EMatch self-tail, NO lambda) inside a fiber → OK; `apply_loop`
   (EMatch self-tail, lambda param, calls `f(h)`) inside a fiber → OVERFLOW.
   The discriminant is the **lambda call**, not the EMatch, not the effect row,
   not the tail-call shape.
5. Read the lambda-call lowering: `f(h)` → `nemit_call_indirect` →
   `nemit_apply_call` → `kaix_apply(clo, n, args)`. The `args` array comes from
   `nemit_ptr_array` (emit_native_ops.kai), which did
   `llvm_build_alloca(native_ctx_b(ctx), arrty, "args")` — an alloca at the
   CURRENT builder position, i.e. inside the loop body block the `tcrec` back-edge
   branches to. An `alloca` instruction in a loop allocates stack PER EXECUTION
   (LLVM dynamic alloca; an `[n x ptr]` array is not promoted by mem2reg). 40 K
   iterations × 8 bytes overran the fiber's 64 KiB; the OS 8 MiB stack absorbed
   it (≈320 KiB), which is exactly why it hid for so long.
6. The smoking gun was already documented: native_prims.kai says "every named
   register is an entry-block alloca; mem2reg promotes it". This one call-site
   escaped that invariant.

## Root cause

`nemit_ptr_array` planted the call-site argument buffer's `alloca` at the
builder's current block. For any `kaix_apply` / `kaix_variant` / `kaix_record`
call reached inside a `tcrec`/TRMC goto-loop, that block is the loop body, so the
alloca re-executed each iteration and the stack grew linearly in the iteration
count. The C-direct oracle never had this: it emits a frame-scoped
`KaiValue *args[n]`, allocated once per frame, content overwritten per call.

## Fix (asu Camino A — consulted once)

Hoist the fixed-size argument-buffer alloca to the CURRENT function's entry block
(`alloca.entry`, which `nemit_fn_with` already creates for the param / fx / TRMC
allocas), so the goto-loop reuses one slot per call-site — exactly the C oracle's
frame-scoped array.

- **runtime.h (`KAI_LLVM`):** new C forwarder `kai_llvm_build_alloca_entry(ctx,
  ty, name)`. Saves the current insert block, positions before the entry block's
  first instruction (`LLVMGetEntryBasicBlock` + `LLVMGetFirstInstruction` /
  `LLVMPositionBuilderBefore` — inserts the alloca BEFORE the entry's `br`
  terminator, which is already built by the time a body block emits a call, so
  the module stays well-formed), builds the alloca, restores the builder. Takes
  the native ctx (not the builder) so it can reach `c->fnval`. A matching
  `!KAI_LLVM` stub returns `kai_llvm_native_unavailable()` so the C-only
  bootstrap / selfhost build compiles.
- **native_prims.kai + stage1/compiler.kai:** register `llvm_build_alloca_entry`
  in BOTH rprelude tables (stage2's drives the typer scheme + resolver name;
  stage1's is the separate copy kaic1 needs to compile the stage2 bundle — the
  first build failed `undefined name` until stage1 learned it too).
- **emit_native_ops.kai:** `nemit_ptr_array` swaps `llvm_build_alloca(...b...)`
  → `llvm_build_alloca_entry(ctx, ...)`. The STORES into the buffer stay in the
  current (loop) block — the values are loop-variant; only the storage is
  hoisted. One change covers ALL arg-buffer call-sites (apply / variant / record
  / closure-capture) since they all route through `nemit_ptr_array` — the right
  altitude.

The KIR, the tail-call detector (`tcrec_rewrite_*` in emit_c.kai, shared by both
backends), and `nemit_tcrec` (the goto back-edge) are UNTOUCHED.

## asu review — the four checks + one hazard caught

asu confirmed Camino A and flagged the placement hazard I had to get right:
insert **before the entry block's terminator** (the `br` to the loop header is
already present when a body block emits a call), not append after it — else the
module is malformed. Verified the dominance: `alloca.entry` is the loop header's
entry predecessor, so it dominates every iteration; the alloca is live across the
back-edge. asu's RC point: the alloca is pure storage for pointer copies — the
closure's RC is governed by the `dup f`/`drop` the KIR already emits, orthogonal
to where the buffer lives; the all-boxed-native per-iteration leak (no
decref→free cascade) is pre-existing and out of lane. Rejected `stacksave`/
`stackrestore` (it's for runtime-variable allocas; every call-site here has
compile-time-constant arity). One alloca per call-site, not shared — LLVM's
stack-coloring reuses disjoint-lifetime slots for free.

## Verification (the IR-grep invariant asu asked for)

`KAI_NATIVE_DUMP_IR` on `apply_loop`: `%args = alloca [1 x ptr]` sits in
`alloca.entry` (with the param `.addr` allocas); the loop body `L4` (which
`br`s back to `entry`) has **zero** allocas — only a `getelementptr` into the
hoisted `%args` and a `store`. Programmatic check: 0 allocas outside
`alloca.entry`.

## Fixtures added

- **Took off the ratchet:** `examples/effects/issue_668_map_large_in_fiber.kai`
  (the closing gap) — native now == C-direct (`total: 120000`).
- **New regression fixture:** `examples/effects/tco_lambda_param_in_fiber.kai`
  (+ `.out.expected` → `total: 80000`). A USER tail-self-call with a lambda
  param (`apply_loop`, calls `f(h)`) paired with a no-lambda control
  (`copy_loop`), both over 40 K inside a fiber. This pins the lambda-param /
  arg-buffer path directly, independent of stdlib `map` internals. Running inside
  a fiber (bounded 64 KiB stack) is load-bearing — on the OS stack the overflow
  would need millions of iterations and the regression would not surface (the
  exact trap that hid the original bug).

## Gates

- native `issue_668` == C-direct: `total: 120000`, exit 0, no overflow.
- `apply_loop`/`map_loop` (40 K+) inside a fiber: O(1) stack, native == C.
- no-lambda self-tail (`build` / `copy_loop`) still TCOs (#706 happy-path intact).
- IR-grep: arg-buffer alloca in `alloca.entry`, zero allocas in the loop body.
- ASAN + UBSan on the lambda loop: clean (no double-free of the closure, no UAF).
- selfhost byte-id: OK (kaic2b.c == kaic2c.c).
- native-parity ratchet: `issue_668` removed; baseline holds (only
  `m7a_6d_double_resume`, PR #847's separate resume gap, remains); zero new gaps.

## Cost vs. estimate

Smaller than the brief implied. The brief budgeted a TCO-detector change; the
actual fix is a 1-line emitter swap + a forwarder + two registry entries + one
C stub. The bulk of the lane was DISPROVING the brief's diagnosis (the dump
that showed `tcrec-goto` present) and bisecting to the real cause (the loop-body
alloca). The standard trap of trusting a confident-but-wrong brief: the brief
said "verify" and the verification flipped the entire premise.

## Follow-ups left for next lanes

- The per-iteration `dup f` on the closure leaks under all-boxed native (no
  decref→free cascade) — a pre-existing, separate gap, not introduced or touched
  here. Same class as the leaks noted across prior native burn-down retros.
- `m7a_6d_double_resume` is the only remaining ratchet entry (PR #847, resume
  path — disjoint from this TCO/alloca fix). When it closes the baseline is empty
  and Lane 1.5's native-default flip is unblocked on the corpus-parity axis.
