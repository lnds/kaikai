# Lane experience — issue #1093: integer div/mod by zero is silent UB

## Scope as planned vs shipped

**Planned:** turn raw integer `/0`, `%0`, and `INT_MIN / -1` from silent UB
(divergent garbage across the native and C backends, exit 0) into a clean trap
— the same failure class as an out-of-range array index — on both backends,
reusing the existing OOB trap mechanism rather than a manual `panic()` or a new
construct.

**Shipped:** exactly that. Both backends route raw `Int` div/mod through two new
runtime helpers, `kai_idiv_chk` / `kai_imod_chk`, which trap via `kai_trap_abort`
(the same primitive the OOB check calls). No new trap node, no new mechanism.

## Design decisions and alternatives considered

The load-bearing decision was **where** the guard lives:

- **Option A — inline guard in each backend.** Emit an `icmp`+`condbr`+trap-call
  sequence in the native LLVM builder and a ternary/helper-call in the C emitter.
  Rejected: duplicates the guard logic in two places, and the C ternary form
  (`b==0 ? trap : a/b`) double-evaluates operands.
- **Option B — one runtime helper, both backends call it.** A checked helper
  does the guard and returns a raw `i64`; the C backend emits `kai_idiv_chk(a,b)`
  and the native backend emits a `call` to the exported shim `kaix_idiv_chk`.
  **Chosen.** This is precisely how OOB already works: the check lives inside
  `kai_array_get_impl`, and both backends only emit a call. One place, one
  message, and `INT_MIN/-1` is covered for free.

The div hot path pays a `call` instead of a bare `sdiv`, but integer division is
already tens of cycles; the guard is negligible next to it, and only `/`/`%` are
affected (add/sub/mul/cmp stay inline instructions).

## Structural surprises the brief did not anticipate

1. **The "keeps BOXED" comments were lying.** Several comments
   (`kir_lower_raw.kai`, `native_prims.kai`) claimed raw `Int` `/`/`%` "stay
   BOXED" and route through the trapping `kai_op_div`. The code does the opposite
   — `op_is_raw_arith` includes `/` and `%`, so they reach a bare `sdiv`/`srem`.
   The comment drift was bad enough that a first-pass exploration believed the
   comments and concluded there was no bug. Empirically, `10 / 0` returns `0`
   (C) / `2^30` (native), exit 0. All the stale comments were corrected as part
   of the fix.

2. **Scope is `Int` (i64) only, not every integer width.** The brief flagged
   Int32/Int128/UInt* as candidates. But `ty_is_integral_raw_t` in the unbox pass
   only raw-promotes `TyInt` (plus Bool/Char) — fixed-width div/mod stays boxed
   and already traps via `kai_fixed_div` (verified: `Int32 10/0` → clean trap
   pre-fix). So the raw UB path is `Int` alone; one i64 helper closes it with no
   width explosion. `INT_MIN/-1` overflow applies only to the signed i64 case,
   which is exactly what the helper guards.

3. **Real division is untouched, by design.** IEEE-754 defines `1.0/0.0` as inf,
   which is not UB; the fix gates on `ty_opt_is_int`, so Real div never enters the
   checked path. Confirmed: `1.0 / 0.0 = inf`, exit 0, before and after.

## Fixtures and coverage

`examples/effects/issue_1093_*`:
- `div_by_zero_traps`, `mod_by_zero_traps`, `div_overflow_traps` — negative
  runtime-abort fixtures (non-zero exit + a specific `trap: …` message on
  stderr; only stdout up to the trap has a golden), modeled on the top-level
  half of `test-issue-1067-fiber-trap-isolation`.
- `div_normal_ok` — positive control: `10/3=3`, `10%3=1` unchanged, and
  `1.0/0.0=inf` proving Real is not trapped.

Wired as `test-issue-1093-divzero-trap` in `.PHONY`, `TEST_LIGHT_TARGETS`, and
`test-fast`. CI runs it under both tier1 (C oracle) and tier1-native, so the
same fixtures exercise both backends and prove the divergence is gone — the
shared trap message is the parity gate.

## Cost vs estimate

Straightforward once the raw-vs-boxed path was pinned down. The bulk of the time
was empirical verification (baseline repro on both backends, confirming
fixed-width and Real were out of scope) rather than the edit itself, which is a
helper pair plus two call-site swaps.

## Follow-ups left for next lanes

- The unused `sdiv`/`srem` cases (op 3/4) in `kai_llvm_build_ibinop` are now
  dead for the div/mod path (kept for completeness). A later cleanup could drop
  them if nothing else routes opcodes 3/4 through the low-level builder.
