# Lane experience — native Int unboxing (P1 of the native-codegen-perf plan)

**Lane:** `native-int-unbox` · **Date:** 2026-06-16 · **Plan:** `docs/native-codegen-perf-plan.md` §P1

## Scope as planned vs. as shipped

**Planned (plan §P1):** extend the existing `Real` raw-lowering machinery to
`Int` *arithmetic and comparison*, so a `MUnboxed` Int loop runs on native
`add`/`sub`/`mul` + `icmp`→`condbr` instead of boxed `kaix_*` calls. Four
sites, three correctness guards.

**Shipped:** exactly that, plus the one site the plan did not enumerate — a
**second `rprelude_table()` in `stage1/compiler.kai`** (the stage-1 compiler
that builds stage 2 must know the new C-API prim names, the same
double-source discipline as the two `runtime.h` copies). And the two
type-routing fixes the plan's "mirror the Real path" instruction hid (below).

## What got unboxed

For a scalar tail-rec loop `sum_loop(i, acc) = if i==0 acc else
sum_loop(i-1, acc + (i*i - i/3))`, measured on `tools/native-perf`:

| | baseline | P1 |
|---|---|---|
| `bl`/iteration (arith loop) | 30 | **16** |
| native wall (200M iter) | 5.90 s | **2.78 s** |
| factor vs C-direct | ~84× | **~40×** |
| `deep_rec` (fib trees) | 0.30 s | **0.12 s** (~30× → ~12×) |

`kaix_add` / `kaix_sub` / `kaix_mul` (arith) and `kaix_bool` / `kaix_truthy`
(the boxed condition round-trip) are **gone** from the loop body. The ~2.1×
win is real and measured, not promised.

The **residual 16 `bl`** are all the boxed-param border: `kaix_int_field`
(unbox a boxed param on each use) + `kaix_int` (rebox the result for the
tcrec back-edge assign) + one `kaix_div` (guard #2). These are NOT arith —
they are the function-call **ABI** boundary, which the plan explicitly scopes
*out* of P1 ("does not touch the boxed call/border ABI"). Closing them needs
raw-`i64` *parameters* (a signature/ABI change + every call-site), the same
boundary the Real path also stops at today. That is the P2/follow-up lever.

## The three correctness guards (verified, not assumed)

1. **Wrapping, no `nsw`/`nuw`.** `LLVMBuildAdd`/`Sub`/`Mul` with the no-wrap
   flags unset. Verified: `INT_MAX + 1 == INT_MIN`, `INT_MAX * 2 == -2`,
   `INT_MIN - 2 == INT_MAX`, native == C-direct on every case. A `nsw` add
   would have made signed overflow UB and let O2 mis-fold.
2. **`/` and `%` stay boxed.** Not added to `kir_op_is_raw_iint_arith`; a raw
   Int `/`/`%` falls to the type-driven `lower_scalar_unbox_border`, which
   lowers it boxed (`kaix_div`/`kaix_mod`) then `int.unbox`'s the i64 back.
   Verified: `INT_MIN / -1` and `INT_MIN % -1` do **not** crash native (they
   return a defined boxed result), where the **C-direct binary emits UB**
   (`sdiv INT_MIN, -1` is C UB — C gave non-deterministic stack garbage).
   This is the rare case where native is *more* defined than the oracle.
3. **`icmp` → direct condbr.** A raw comparison/bool feeding an `if` lowers
   raw (an `SBool` i32 register); `nemit_condbr` reads the register slot and
   `icmp ne i32 0`'s the raw value directly — skipping `bool.box`
   (`kaix_bool`) + `kaix_truthy`. Verified in the IR: the loop header issues
   no call. Implemented as `lower_if`'s raw branch + `ncond_is_raw_bool` in
   the emitter (slot-driven, so a boxed condition is untouched).

**RC:** `KAI_TRACE_RC` over the multi-use-`i` loop shows **zero `int`
allocs** — a raw i64 has no refcount, so a multi-use Int operand cannot
double-free (the very bug the Real lane and the #845 let-binder lane fought).
`incref_total=0`, no negative refcounts.

## Structural surprises the brief did not anticipate

The brief said "mirror the Real precedent, it's the same form." Two places
where a literal mirror was a **miscompile**, found by measurement, not review:

- **Arith routes by RESULT TYPE, not op string.** `+ - *` are in BOTH the
  Real (`kir_op_is_raw_freal_arith` = `+ - * /`) and Int
  (`kir_op_is_raw_iint_arith` = `+ - *`) sets. The Real-first `if
  op_is_freal { kir_ty_is_real }` ordering rejected *every* Int `+`/`-`/`*`
  (its result is Int, so `kir_ty_is_real` is false) before the Int branch
  ran — silently leaving all Int arith boxed. The first build "compiled and
  ran correct" but unboxed *nothing*; only the `bl` count (30→26, not 30→16)
  exposed it. Fix: gate on `x.ty` first, then the op set. Same fix in two
  places (`kir_kind_is_raw` classify + `lower_expr_raw_kind` emit).

- **Unary `-` is Real-only; the border must be type-driven.** The Real path's
  `EUnop("-")` arm unconditionally emitted `fneg`/`SReal`. A raw Int `seen ==
  -1` (the `int_cache_reuse_bump` fixture) recursed into the `-1` unop, hit
  that arm, and emitted `fneg ptr` over a boxed/i64 value — an LLVM verify
  crash, NOT a silent miscompile (the loud failure the "unsupported node"
  discipline buys). Two fixes: `EUnop("-")` gates on `kir_ty_is_real(x.ty)`,
  and the box→raw border (`lower_real_unbox_border` → `lower_scalar_unbox_
  border`) now picks `int.unbox` (`kaix_int_field`) vs `real.unbox`
  (`kaix_real_field`) by node type. A Real-only border would `kaix_real_
  field` a boxed Int (read `as.r` off an Int payload) — the kind of bug that
  passes a smoke test and corrupts a real program.

The lesson: the Real path's helpers keyed on the *operator string* alone
because Real was the only scalar family. Adding a second family makes the
operator ambiguous; every classify/route decision had to move to the
**type** axis. This is generic, not a `sum_loop` hack: it routes any
Real-vs-Int arith/cmp the unbox pass marks.

## Why the residual is not chased here

The boxed-param border (`kaix_int_field`/`kaix_int` per use) dominates the
remaining 16 `bl`. Eliminating it = raw `i64` *parameters*: the function's
LLVM signature changes from all-boxed `ptr (ptr×n)` to a mixed/raw sig, and
**every call-site** must pass i64 raw. That is an ABI change the plan scopes
to P2, and the Real path stops at exactly the same boundary today (a Real
loop still shows `kaix_real_field` per param use). Pulling it into P1 would
couple an ABI reshape with its call-site consumers — the precise
land-together hazard the lane discipline warns against.

## Fixtures

- `examples/perceus/native_int_unbox_arith.kai` (+`.out.expected`) — new.
  Pins all three guards: wrapping at the int64 boundary, boxed `/`/`%`, raw
  comparison conditions, and an Int operand used 3× in one expression.
  Walked by the backend-parity ratchet (native vs C) + the perceus golden
  harness.
- `examples/perceus/int_cache_reuse_bump.kai` — existing; was the fixture
  that caught the `fneg ptr` unary-minus crash (a raw `seen == -1`). Its
  parity now holds.

## Gates (all green local)

- selfhost byte-id: OK (`kaic2b.c == kaic2c.c`) — the C path is untouched;
  the Int-unbox code is purely additive to the native walk.
- backend-parity ratchet (native vs C): **pass=479 fail=0** over 537 fixtures
  (was 478/1 mid-lane; the 1 fail was the unary-minus crash I introduced and
  then fixed).
- tier0: OK (32 fixtures, demos baseline 35/35, arena gate plain+ASAN).
- guards #1/#2/#3 + RC: verified by direct measurement (above).
- `km score`: `kir_lower_raw.kai` B+ (86.7), `emit_native_ops.kai` B+ (83.9)
  — both edited (not new) files held above the B floor; cogcom max 14 in
  `kir_kind_is_raw` (the type+op classify), under the <25 cap.

tier1 / tier1-native / tier1-backend-parity / tier1-asan: deferred to CI.

## Follow-ups for the next lane (P2)

1. **Raw `i64` parameters** — the boxed-param border is the residual ~40×.
   Needs a mixed/raw LLVM signature + call-site marshalling. Generic: applies
   to Real params too (the Real loop has the identical `kaix_real_field`
   residual). This is the single biggest remaining native scalar lever.
2. **Plan P2 (bitcode-link the runtime before O2)** — orthogonal; closes the
   heap-bound residual (`list_fold`, `rbtree`), not the scalar one.
3. **`variant_match` super-linear collapse** (plan §3.4) — unrelated, its own
   investigation.
