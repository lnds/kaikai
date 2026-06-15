# Lane experience — native-sha512-hang

**Lane:** `native-sha512-hang`
**Date:** 2026-06-15
**Scope:** Close the native-parity gap `examples/stdlib/crypto_hash_basic.kai` —
SHA-512 hung (infinite loop → SIGKILL, ~39 s CPU + OOM) under the in-process
libLLVM native backend (KIR Lane 1.5), while C-direct computed the digest
instantly. Deterministic, not flaky.

## Scope as planned vs. as shipped

**Planned (brief):** reproduce the hang; bisect which 64-bit helper diverges
(`sha_add64` / `sha_rotr64` / `sha_combine64` / `sha_top64`); dump the native
IR of the culprit; consult asu once on the lowering mechanism; fix the native
lowering (NOT the stdlib); take the fixture off the baseline.

**Shipped:** exactly that, but the brief's heuristic ("a `bit_*`/shift that
lowers differently in native vs C") was WRONG. Every 64-bit helper — `lo32`,
`hi32`, `top64`, `combine64`, `add64`, `shl64`, `rotr64` — produced BYTE-IDENTICAL
results in native and C when exercised with isolated values. The hang was not a
wrong-arithmetic bug; it was a **use-after-free** that corrupted a loop guard so
a recursion never terminated. The real root was orthogonal to the 64-bit math:
**a `let`-bound out-of-cache Int reused more than once was double-freed in the
KIR.** The fix is in the KIR Int materialisation, one layer below where the
brief pointed.

## Diagnosis path (the bisection that mattered)

1. Confirmed the split: `sha256([])` → `32` (OK native); `sha512([])` → hang.
2. Bisected every 64-bit helper with isolated known inputs → ALL native==C.
   Bisected the 64-bit literals (`0x...f3bcc908`, `-5349999486874862801`) →
   native==C. The brief's suspects were all innocent.
3. Bisected the compression `round` → native **SIGSEGV** (not hang), and a
   SINGLE round diverged in field `a` only. Printed each subterm → the FIRST
   diverging value was `big_sigma1(e)` → `rotr64` of a large `let`-bound `e`.
4. Reduced to the minimal repro: `let e0 = <big>; bit_ushr(e0,14); bit_shl(e0,1)`
   — the SECOND use of `e0` reads garbage (`bit_shl` → `15`). With ONE use,
   native is correct. **Multi-use of a `let`-bound big Int is the trigger.**
5. `--backend=llvm` (LLVM-text + clang) was CORRECT; only `--emit=native`
   (in-process) failed → suspected an opt-pass. But `KAI_NATIVE_OPT=0` (O0)
   ALSO failed → not an opt-pass. The divergence was in the MODULE the
   in-process backend BUILDS (via the C-API), not in how it optimises it.
6. Dumped both IRs (`KAI_NATIVE_DUMP_IR`). The text emitter REMATERIALISES the
   literal at each use (`kaix_int(i64 ...)` fresh per use — accidentally
   correct). The KIR/native builds ONE `e0.addr` alloca, calls `kaix_int` once,
   and LOADS it twice — feeding the SAME boxed cell to two `kaix_*` prims that
   each `kai_decref` their args. First use frees `e0`; second reads a reused
   slab cell. **Confirmed use-after-free.**
7. Dumped the KIR (`--emit=kir`): `e0: box = <lit>; prim +(e0,1); prim +(e0,2)`
   — NO `dup e0` between the uses (contrast: the same dump shows `dup acc` /
   `dup buf` before every multi-use of a boxed binding in another fn).

## Root cause

Three facts compose into the bug:

- **`kai_int` immortal cache.** Ints in `[-65536, 65535]` return a cached slot
  with `rc = INT32_MAX` (decref is a no-op). Out-of-cache Ints (`|n| > 65535`)
  are real slab allocations with a normal refcount.
- **The unbox pass marks every Int literal `MUnboxed`** (`decide_mode_aware`:
  `EInt(_) -> MUnboxed`). Perceus then SKIPS the RC of a raw binder
  (`prc_pat_bindings_skip_raw` returns the binder OUT of scope when
  `rhs.mode == MUnboxed`), so no `__perceus_dup` is inserted for a non-last
  read — exactly as for a raw `double`.
- **The KIR is all-boxed and IGNORED `.mode` for Int.** The np-real lane made
  the KIR mode-slave for **Real only** (`kir_lower_raw` SCOPE explicitly
  excluded Int/Bool/Char, on the false premise that "boxed Int rides the
  immortal cache, no double-free"). So a `MUnboxed` Int `let`-binder was
  re-boxed into ONE shared cell, read N times, with no dup — and the consuming
  prims freed it on the first use.

The premise was false for out-of-cache Ints. SHA-512's 64-bit constants
(`sha512_k`, `sha512_initial`) are all out-of-cache and reused across the
80-round schedule; a `let`-bound K/state word freed mid-loop, the corrupted
read fed a recursion guard, and it looped forever. A fn PARAM did NOT trigger
it (params ride perceus's RC scope); only the `let`-binder leaked to raw.

The C-direct oracle was always correct: it lowers a `MUnboxed` Int `let` to
`int64_t kair_x` raw and boxes a FRESH `kai_int(kair_x)` at each consuming use
— no shared cell, no RC. The bug was native-only.

## The fix (asu Camino A — the Real precedent, extended to Int let-binders)

Make the KIR mode-slave for the Int `let`-binder FORM, mirroring the Real raw
path the np-real lane built:

- `kir_lower_raw.kai` — `kir_ty_is_int` + admit `EInt` / `EVar`-of-Int into
  `kir_kind_is_raw` (FORM-restricted: literal/var only, NEVER Int arith/bit-ops,
  which have no native raw lowering and ride their own fresh cell at the
  border); `kir_raw_box_prim` → `int.box` for Int.
- `kir_lower_walk.kai` — `raw_let_slot` → `Some(SInt64)` for a `MUnboxed` Int
  binder; `lower_expr_raw_kind` materialises `EInt(n)` as `KIntV(n)`.
- `emit_native_ops.kai` — `int.box` → `kaix_int(i64)` in `nemit_raw_box` /
  `nprim_is_raw`.
- `emit_native_fn.kai` — `nemit_atom_raw` materialises `SInt64` (const i64 /
  raw load); `nemit_load_reg_as_int` (box→raw borrow via `kaix_int_field`, the
  existing `kai_intf` reader, no decref); `nemit_load_reg_boxed` boxes a raw
  `SInt64` register with a FRESH `kaix_int` (tag 1) at each read — the raw→boxed
  border, mirror of the oracle's `kai_int(kair_x)` per use.

Net: a `let x = <big Int>; ...x...x...` lowers `x` raw `i64` (slot `SInt64`)
and boxes a fresh cell at each consuming read. No shared cell → no RC → no
double-free. Identical to the C-direct oracle's shape.

### Why `EVar`-of-Int is a raw FORM (and the round-trip cost it accepts)

The narrowest fix — raw-promote only the `EInt` literal binder — does NOT
close SHA-512: a `let y = x` (an `EVar` reading a raw `SInt64` binder) is ALSO
`MUnboxed`, so perceus skip-raws it; left boxed with no dup, `y` double-frees
the same way. So an Int `EVar` must be a raw FORM too. The cost: a `MUnboxed`
Int `EVar` / `EInt` reaching a BOXED consumer (a call arg `f(x)`, an arith
operand `n + 1`) now crosses the raw→boxed border — an `int.box` round-trip the
KIR dump shows, so the `test-kir` goldens (`atoms_calls` / `closures` /
`control_flow` / `effects` / `interpolation` / `list_match` / `stateful_clause`)
were regenerated. This is the SAME round-trip the C-direct oracle pays
(`kai_int(kair_x)` at every boxed use), so it is correct and oracle-matching;
it is a missed *optimisation* (a boxed-context Int read could materialise boxed
directly), not a bug. Gating the raw path at the binder ONLY (leaving inline
reads boxed) re-introduced the hang for a `let s = <Int arith>` binder, and an
`int.unbox` border for that case tripped a boxed-temp-RC SIGSEGV — the
FORM-level raw promotion is the sound shape.

## Design decisions & alternatives considered

- **A — raw-Int arith/bit-ops in the KIR (full symmetry with Real).** Rejected:
  large churn (needs native `i+`/`iand`/`ishl` raw instructions + a border at
  every Int consumer), and unnecessary — the bug is the `let`-binder's RC, not
  the arith. The chosen fix raw-promotes ONLY the binder FORM (literal/var);
  arith/bit-ops stay boxed and receive a fresh boxed cell at the read border.
- **B — perceus stops skip-raw'ing Int binders (insert `__perceus_dup`).**
  Rejected: the AST is SHARED with emit_c, which lowers the binder RAW
  (`int64_t kair_x`); a `__perceus_dup(kair_x)` over a raw `int64_t` is a type
  error in C (`kai_internal_dup` wants a `KaiValue *`). Confirmed by reading
  emit_c.kai:2052. The fix CANNOT live in perceus without breaking the C oracle.
- **C — bit-prims BORROW (drop the `kai_decref`).** Rejected: against the
  oracle (emit_c's bit/arith prims consume too), and a mass leak risk.
- **D (chosen).** Make the KIR consistent with what perceus assumed (a raw,
  RC-free binder) by lowering it raw — exactly what emit_c already does.

## Structural surprises the brief did not anticipate

- The brief's heuristic ("a 64-bit helper diverges") was a red herring; the
  arithmetic was bit-identical. The discriminating bisection was multi-USE, not
  per-helper-value. Lesson: a hang from a corrupted loop guard looks like a
  math bug but is an RC bug.
- The LLVM-text backend was a false oracle for "is the lowering correct": it
  rematerialises literals (accidentally dodging the shared-cell), so it passed
  while the in-process built the buggy module. The real oracle is C-direct
  (`int64_t kair_x` raw), which states the intended convention.
- The bug was ALREADY documented as a CLOSED hazard for Real (the np-real lane's
  `kir_lower_raw` header describes this exact double-free) — but explicitly
  scoped OUT for Int on a false "immortal cache masks it" premise. This lane is
  the Int half of the same fix.

## Fixtures added & coverage

- `examples/perceus/let_bound_big_int_multiuse.kai` (+ `.out.expected`) — the
  minimal regression: an out-of-cache big-Int `let` with two bit-op uses + one
  arith use; a single-use big-Int `let` (still raw, must print right); an
  in-cache small-Int multi-use (guards the cache-range boundary). native==C.
- `examples/stdlib/crypto_hash_basic.kai` — the gap itself, now native==C for
  sha256/sha512/hmac-256/hmac-512. Removed from `tools/native-parity-baseline.txt`
  (3 → 2 gaps).

byte-id is FALSE-GREEN for this bug (compiler.kai has no hot multi-use big-Int
`let`-binder, so selfhost can't exercise it); the real gate is the two fixtures
running native==C-direct + an ASAN/UBSan run on the sha512 native binary.

## Gates

- Native `sha512([])` / full crypto fixture: native==C, exit 0, < 1 s (no loop).
- selfhost byte-id: OK (`kaic2b.c == kaic2c.c`).
- tier0: green.
- ASAN + UBSan on sha512 + the full crypto fixture: clean (the int64 overflow in
  the bit-prims goes via uint64, so UBSan is satisfied).
- native-parity ratchet (`TARGET_BACKEND=native ORACLE_BACKEND=c`): zero new gaps.

## Cost vs. estimate

Diagnosis dominated (the brief's heuristic sent the first bisection at the
wrong layer). The fix itself is ~30 lines across 4 files — additive, symmetric
to an existing pattern. No new files in the compiler (the only new file is the
fixture).

## Follow-ups left for next lanes

- **Round-trip optimisation.** A `MUnboxed` Int `EVar`/`EInt` in a boxed
  position (a call arg, an arith operand) crosses the raw→boxed border with an
  `int.box`/`kaix_int` round-trip — correct (it mirrors the C oracle's
  `kai_int(kair_x)`) but a missed optimisation: a boxed-context Int read could
  materialise boxed directly. Closing it needs the emitter to distinguish a
  raw-binder read whose CONSUMER is boxed (skip the raw load → box) from one
  whose consumer is raw — a slot/consumer-aware border, the same shape the
  Real raw path could also tighten. Not forced here (correctness over the
  micro-opt; the C oracle pays the same cost).
- The native backend still leaks (all-boxed, never cascades decref→free —
  pre-existing, orthogonal). KAI_TRACE_RC on this fix shows no NEW double-free;
  the residual leak is the existing one.
- **PRE-EXISTING, OUT-OF-LANE (found during edge-case testing, NOT fixed here):**
  a `let x` that SHADOWS an outer `let x` CLOBBERS the outer's register in the
  native backend — a read of the outer `x` AFTER the inner block returns the
  inner value. Reproduces with SMALL ints on the untouched BOXED path
  (`let x = 100; let x2 = { let x = 200; x }; x  // native: 200, C: 100`), so it
  is NOT introduced by this lane's raw-Int path — the KIR keys a register by
  name and a shadowing `let` reuses the alloca. No corpus fixture exercises it,
  so it is not on the ratchet, and the regression fixture here deliberately
  avoids shadowing. This is a separate native register-naming gap (the KIR
  should scope-qualify a shadowing binder's register). Per lane discipline it is
  reported here, not fixed inline; opening a GitHub issue needs explicit
  authorisation.
