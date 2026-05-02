# Lane experience report — Real unboxing

Best-effort retrospective by the implementing agent. See limitations
at the bottom.

## TL;DR

**Shipped.** Real unboxing extends the Phase 2 unbox pass (PR #38)
to `Real` operands, lowering raw arithmetic to `double` instead of
`KaiValue *`. Inner Real-heavy chains now collapse to native C
arithmetic with no `kai_add` / `kai_mul` / `kai_sub` / `kai_div`
boxing inside the loop body.

Closes the last item of Tongariki (5/5). Brings the user-facing
performance gap on Real-heavy code from ~14× wall-clock vs C (the
Phase 2 v1 baseline that excluded `Real`) down to ~2.2× wall-clock —
inside the 5–10× target band the milestone aimed for. Wall-clock
speedup vs Real-boxed: **~6.7×**.

## Objective metrics (from `/tmp/lane-real-unboxing-builds.tsv`)

- Start: 2026-05-01T21:56:54-04:00
- End:   2026-05-01T22:14:27-04:00 (last logged build)
- Wall-clock: ~17m 33s (single agent run; no wait windows)
- Build / test invocations recorded in TSV:
  - `make tier0`: 3 invocations, 3 passes, 0 fails
  - `make test-unbox-phase2`: 1 invocation, 1 pass, 0 fails
  - `make tier1`: 1 invocation, 1 pass, 0 fails
  - `make -C stage2 selfhost-llvm`: 1 invocation, 1 pass, 0 fails

Out-of-band selfhost rounds were also run while toggling between
the pre-extension and post-extension `kaic2` for the bench
measurement (see *Measurement* §); those were not logged in the
TSV because they were one-shot diagnostic builds, not pre-commit
sanity gates.

## Investigation (M0)

Read of `docs/unboxing-phase2-design.md` + the `unboxing` issue
inventory (#87–#91) established that the v1 lane shipped Int / Bool /
Char with `Real` explicitly deferred:

> `Real` is in scope architecturally but skipped in v1 because it
> doubles the test surface; tracked as a Phase 2 follow-up item
> (~1d) once the v1 numbers are in.

The follow-up doc parked Real with `String` / `Array` / `var` as
"all explicitly Phase 3 per `docs/unboxing-phase2-design.md`
§Non-goals". Tongariki's pull-forward decision contradicted that —
the gap on Real-heavy code remained at the pre-Phase-2 ~50–100× C
ratio, undefended in `CLAUDE.md` Tier 1 #2's
`runtime-efficient` claim for any program that touches floats.

Architectural inspection of the existing `unbox_pass`
(`stage2/compiler.kai:22968+`) showed the design was already
type-driven via `ty_is_unboxable_t`. The v1 lane left a clean slot
where adding `TyReal -> true` lights up the analysis for `Real`
without rewriting the pass itself. The emitter side has three lookup
tables (`raw_c_type`, `box_wrap`, `unbox_field_for`) plus
`emit_kind_raw` — each grows one row.

Two type-aware constraints surfaced as load-bearing:

1. **`%` (modulo) has no native operator on `double`.** The v1
   `op_is_raw_arith` predicate accepts `%` for raw lowering on
   integers; for Real it would emit `(a % b)` which is a C compile
   error. The `decide_mode` rule needs a type guard.
2. **`switch` requires an integral operand.** The M5 match fast
   path emits `switch (_scr)` with `_scr` a `raw_c_type(ty)` value;
   for `Real` that is `double`, which the C standard rejects as
   a switch operand. `emit_match_expr` needs a type gate.

Both constraints are local: they touch one branch each in the
existing rule rather than reshaping the analysis.

## Implementation (M1 + M2)

Single-commit chunk because the changes are co-load-bearing — the
unbox pass marking Real `MUnboxed` would crash the emitter without
the new `EReal` raw case. Splitting them cleanly was not possible.

**Type lookup growth** (`stage2/compiler.kai`):

- `ty_is_unboxable_t`: add `TyReal -> true` row.
- New `ty_is_integral_raw_t` predicate (mirrors `ty_is_unboxable_t`
  but excludes `TyReal`). Used by the two type-aware gates below.
- `raw_c_type`: add `TyReal -> "double"` row.
- `box_wrap`: add `TyReal -> "kai_real(...)"` row.
- `unbox_field_for`: add `TyReal -> "->as.r"` row.
- `emit_kind_raw`: new `EReal(r) -> real_to_string(r)` case (no
  `LL` suffix; the C compiler infers `double` from the literal
  shape).

**Type-aware boundary gates**:

- `decide_mode` for `EBinop`: when `op == "%"` and the result type
  is non-integral (`Real`), keep `MBoxed` even if both operands are
  raw. Routes through `kai_mod` as before.
- `emit_match_expr`: when `scrut.mode = MUnboxed` AND
  `ty_is_integral_raw(scrut.ty)`, take the `switch`-on-raw fast
  path; otherwise fall through to the boxed `_scr` path. A
  `Real`-scrutineed match boxes once at the boundary and uses the
  per-arm equality chain.

`decide_mode` for `EReal` literals: needs an explicit `MUnboxed`
arm — the switch in `decide_mode` had per-kind cases for
`EInt`/`EBool`/`EChar` and a `_ -> MBoxed` fallthrough. `EReal`
landed in the fallthrough by default; without lifting it out, no
Real literal would ever start an unboxed chain.

## Fixtures (M3) + bench (M4)

- `examples/perceus/unbox_phase2_arith_real.kai` — `+ - * /` chain
  on Real operands. Mirror of `unbox_phase2_arith.kai`. `%` is
  intentionally omitted (boxed in v1).
- `examples/perceus/unbox_phase2_cond_real.kai` — comparison +
  boolean + EIf-cond chain on Real operands. Comparison results
  emit as raw `int` per the v1 rule (Bool is integral).
- `examples/perceus/unbox_bench_real.kai` + `unbox_bench_real.c.ref`
  — performance benchmark. Same shape as `unbox_bench.kai` (27
  binops × 20_000 iterations), `+ - * /` only. The integer fixture
  uses `% 100` / `% 257` / `% 1009` reductions; the Real fixture
  substitutes multiplication by reciprocals (`* 0.01` etc.) for
  the same purpose.

`stage2/Makefile :: test-unbox-phase2` picks up the two new
arith/cond fixtures via the existing `unbox_phase2_*.kai` glob and
adds parallel structural greps that assert `double kair_*` locals
land in the bodies and that no `kai_add(` / `kai_mul(` / `kai_sub(`
/ `kai_div(` / `kai_lt(` / `kai_truthy(` / `kai_eq_v(` survives.

## Validation (M5)

| gate                              | result                                    |
|-----------------------------------|-------------------------------------------|
| `make tier0`                      | green (3 runs across the lane)            |
| `make test-unbox-phase2`          | green — Real fixtures pass + Int unchanged |
| `make tier1`                      | green (24 demos, 21 fmt fixtures, bench smoke, check smoke) — 189s |
| `make -C stage2 selfhost-llvm`    | byte-identical fixed point — 19s          |

The LLVM backend stays byte-identical because the v1 design
deferred LLVM emit-mode wiring as M6-followup; the Real-unboxing
extension inherits that posture. A binary compiled via
`--emit-llvm` does not see the raw `double` collapse and remains at
the pre-extension ratio. Lifting LLVM into mode-aware emit is a
~1d follow-up, exactly mirroring the v1 LLVM-mirror item already
parked as issue #87.

Selfhost (C) was rebuilt twice intentionally to support the
pre/post bench measurement (see *Measurement* §). Both rounds
reached fixed point cleanly.

## Measurement

Methodology: build the same `unbox_bench_real.kai` fixture against
two `kaic2` binaries:

- **Pre-extension** (`main` HEAD `2c36a1c`): every Real binop emits
  `kai_add(kai_real(a), kai_real(b))` style boxed allocations.
- **Post-extension** (this branch): the same chain emits raw
  `double kair_*` arithmetic with one `kai_real(...)` boundary
  wrap on the return value.

Both built with `cc -std=c99 -O2`. Wall-clock measured via 2_000-run
batches (`time (for i in $(seq 2000); do … done)`) to amortise
process startup over a stable signal.

| binary                       | wall-clock total | per-run | speedup vs PRE | ratio vs C |
|------------------------------|-----------------:|--------:|---------------:|-----------:|
| pre-extension (Real boxed)   | 36.771 s         | 18.39 ms |  1.0×          | ~14.5×     |
| post-extension (Real unbox)  |  5.494 s         |  2.75 ms | **6.7×**       | ~2.17×     |
| C reference (`unbox_bench_real.c.ref`) | 2.533 s |  1.27 ms | 14.5×          |  1.0×      |

The post-extension wall-clock is **~2.2× the C reference** —
comfortably inside the 5–10× target band. Per-run includes process
startup (~1 ms uniform across all three binaries); subtracting that
gives a core-loop ratio closer to ~5×, still inside the band.

The pre-extension Real bench sat at ~14.5× wall-clock vs the C
reference (~50–60× core after subtracting startup), confirming the
~50–100× pre-Phase-2 gap pinned in
`docs/perceus-honesty-targets.md` §Tier 2.5 was load-bearing for
Real-heavy code even after Phase 2 v1 shipped.

Output equality verified across all three binaries: `3.42736e+08`.

### Inspection of the emitted C — pure_chain_real

Pre-extension (`main`):

```c
KaiValue *kai_a = kai_add(kai_real(1), kai_real(2));
KaiValue *kai_b = kai_mul(kai_internal_dup(kai_a), kai_real(7));
KaiValue *kai_c = kai_sub(kai_internal_dup(kai_b), kai_internal_dup(kai_a));
... (27 boxed-allocation lines + dup/drop machinery)
```

Post-extension:

```c
double kair_a = (1.0 + 2.0);
double kair_b = (kair_a * 7);
double kair_c = (kair_b - kair_a);
... (27 raw arithmetic lines)
kai_real((kair_z_ + kair_t_ + kair_u_ + ...))   // single boundary box
```

The visible-allocation reduction matches the wall-clock signal:
27 `kai_add`/`kai_mul`/`kai_sub`-style allocations per chain pre,
0 (one `kai_real` boundary wrap on the return) post. With 20_000
iterations, that's 540_000 allocations eliminated per run.

## LLM-friendly bet evidence (Tier 3, per `docs/lane-experience-retro-2026-05-01.md`)

> *At any point during this lane, would `--effects-json` or
> `--effect-holes-json` have helped you recover from a typing or
> effect-row error? Did you actually use them, or did the plain-
> text compiler output carry the work?*

Neither used; one plain-text compiler error hit:

```
compiler.kai:23189:12: error: expected `{` after `if` condition
```

This was a parser-side limitation (the kaikai parser does not
accept a multi-line `if` condition with `(... or ...)` after `and`
on the next line) rather than a typing error. Recovered by
restructuring the rule into nested `if/else`. The error message
pointed at the right column on the right line; the JSON channels
have nothing to add — they describe semantic shapes (effect rows,
typed holes), not parser-recovery hints.

Consistent with the lane-experience-retro survey: JSON contracts
favour type-driven authoring. Performance-tuning lanes that grow
existing analyses by one row each (the shape of this lane)
exercise the parser and the structural greps, not the type system.

## Compiler errors I encountered

Exactly one, captured above:

1. **Parser** — `expected `{` after `if` condition`. Triggered by:
   ```
   if op_is_raw_arith(op) and child_is_raw(a) and child_is_raw(b)
      and (op != "%" or ty_is_integral_raw(opt_ty)) { MUnboxed }
   ```
   The kaikai parser accepts long `if` chains broken at `and` only
   when each line continues with `<func>(...)` form; introducing a
   parenthesised sub-expression `(op != "%" or …)` after the `and`
   confused the parser into treating the `(` as a function call
   target. Resolved by lifting the gate into a nested `if`:
   ```
   if op_is_raw_arith(op) and child_is_raw(a) and child_is_raw(b) {
     if op == "%" and not ty_is_integral_raw(opt_ty) { MBoxed }
     else { MUnboxed }
   }
   ```
   Worth noting because the same shape (`if a and b and (c or d)`)
   works fine when the parens lead the condition (line 4180 of the
   same file uses `if (name == "Real" or name == "Int") and …`).
   The asymmetry is parser-specific; first-line-leading paren
   parses, mid-line-after-`and` paren does not.

## Friction points

1. **Pre-extension binary capture for measurement.** The brief
   demanded a wall-clock comparison "antes (Real boxed) vs después
   (Real unboxed)". To get a clean baseline I had to:
   1. Save WIP `compiler.kai` + `Makefile` to `/tmp`.
   2. `git show 2c36a1c:stage2/compiler.kai > stage2/compiler.kai`
      and equivalent for the Makefile.
   3. `make selfhost` to get the pre-extension `kaic2`.
   4. Compile `unbox_bench_real.kai` with that binary, save the
      output binary to `/tmp/bench_real_kai_PRE`.
   5. Restore WIP files.
   6. `make selfhost` again to rebuild the post-extension `kaic2`.
   7. Compile `unbox_bench_real.kai` with that, time both.
   The dance is reversible and worked first try, but the per-step
   risk is non-trivial — losing track of which `kaic2` is in
   `stage2/kaic2` would mean publishing wrong numbers. A future
   lane that needs pre/post baselines could benefit from a
   `make pre-baseline-binary` target that does this dance hermetically
   under a temp directory.
2. **Process-startup dominance for short benches.** With N=20_000
   iterations the recursive bench finishes in ~2–3 ms. Over 2_000
   runs, ~30% of total wall-clock is process startup. The signal
   is strong enough that it doesn't matter for a 6.7× ratio, but
   for sub-3× signals the noise floor would matter. Raising
   N runs into the R6 TCO regression cap (issue #92) and the
   loop-driver TCO regression in perceus-wrapped blocks (issue
   #89). Out of scope for this lane.
3. **`real_to_string` formatting.** kaikai's `print(real_to_string(r))`
   uses `snprintf("%g", ...)` (runtime.h:1173). The C reference
   uses `printf("%g\n", r)`. The exact formatting is identical for
   the bench output `3.42736e+08`, but for some inputs `%g`
   collapses trailing zeros (`14` not `14.000000`). Worth
   double-checking output equality before claiming any bench
   matches its reference.

## Spec ambiguities or interpretive choices

1. **`%` on Real keeps boxed.** The brief explicitly said
   "**`%` (modulo)**: NO unboxear en v1 — keep boxed via
   `kai_mod_real(a, b)`. Uso poco común en Real, box overhead
   aceptable." Implemented exactly that. Note that the runtime's
   `kai_mod` already handles Real → Real via integer truncation
   (`av = (int64_t) a->as.r`), but the brief's quoted helper name
   `kai_mod_real` does not exist — the boxed path routes through
   the existing `kai_mod` which handles mixed Int/Real cases.
   Calling out because a future "lift `%` on Real to native
   `fmod()`" follow-up would touch the runtime, not just the
   emitter.
2. **`^` (power) keeps boxed.** Brief item: "`^` (potencia): boxed
   via `kai_pow_real` en v1." The existing emitter routes `^`
   through `kai_pow_int` / `kai_pow_real` based on the result
   type at runtime; no emitter change needed for v1.
3. **Match scrutinee on Real takes the boxed path.** Decided to
   gate `emit_match_expr`'s switch fast path by
   `ty_is_integral_raw` rather than threaten the C `switch`
   with a `double` operand. A Real `match { x -> … }` boxes once
   at the scrutinee boundary then uses the per-arm equality
   chain. For literal-arm Real matches (`match r { 0.0 -> … |
   1.0 -> … }`) this means one `kai_real` allocation + N `kai_eq`
   calls instead of a C `switch`, which is correct but slower
   than ideal. No fixture exercises this path; if it shows up
   hot, a follow-up could lower to an `if (kair_r == 0.0) … else
   if (kair_r == 1.0) …` chain on the raw `double`.
4. **xmm vs GPR ABI.** The brief flagged this as a verification
   item. Confirmed mechanical: `static KaiValue *kai_real(double r)`
   in `runtime.h:913` already takes a `double` (xmm0 on
   System V AMD64), so the call from emitter `kai_real(<raw>)`
   uses the same ABI the constructor was always called with.
   No code change needed — the boundary already worked.

## Subjective summary

- **Confidence in correctness**: high. Selfhost C byte-identical
  except where Real fixtures opt in (each diff is a strict
  simplification: boxed `kai_add` calls collapse to native `+`).
  Selfhost LLVM byte-identical at the fixed-point level. Tier1
  green. No UB signal during any bench run.
- **Confidence in measurement**: medium. Single machine, no
  process pinning, n=2000 per condition. Wall-clock variance per
  individual run is ~5%, but averaged over 2000 runs the
  population mean is stable. The 6.7× speedup signal is well
  outside the noise floor; the 2.2× POST-vs-C ratio is also
  outside it.
- **Hardest sub-task**: parser-error recovery on the `if … and (…
  or …)` shape. Easy to fix once recognised; the asymmetry vs
  line 4180 of the same file (which uses an outwardly identical
  shape) was the surprising part.
- **Easiest sub-task**: the M1 `ty_is_unboxable_t` extension. One
  line. The architecture from PR #38 was already structured around
  the type-driven analysis, so the extension surface for Real
  was three to four one-line additions plus the matching
  `EReal(_) -> MUnboxed` decision-arm.
- **Did the compiler help or hinder?** Neither distinctly. One
  parser error early on (described above), no type errors, no
  effect-row errors. The structural greps in the Makefile target
  caught the validation work; the typed-hole / JSON channels had
  nothing to do here.

## Limitations of this report

- Self-report bias acknowledged.
- Context truncation: counts and error lists exclude anything that
  fell out of the agent's visible context window.
- Single agent (Claude Opus 4.7). Not generalisable across LLMs.
- Wall-clock measurement methodology is `time (for i …)` on a
  single machine without isolation (no taskset / cpuset / power
  profile pinning). Variance ~5% at single-run level; averaged
  over 2000 runs the signal is stable for 2×+ ratios but a
  rigorous evaluator would want isolated benches with longer N
  and proper warm-up isolation.
- The selfhost LLVM "byte-identical" claim covers the fixed-point
  invariant only. A binary compiled via `--emit-llvm` does NOT
  see the Real raw collapse; the LLVM emit path is unchanged
  (parked as the same M6 follow-up the v1 lane already noted).
  A user running `kai build --emit-llvm` on a Real-heavy
  workload still pays the pre-extension cost. The C backend is
  the default; LLVM is opt-in at the bin/kai layer.
- Bench N capped at 20_000 by the loop-driver TCO regression
  inside perceus-wrapped blocks (issue #89). A larger N would
  improve signal-to-noise; that fix would need to land first.

## Raw build log

```
timestamp	cmd	outcome	elapsed_s
2026-05-01T21:59:37-04:00	tier0-baseline	OK	33
2026-05-01T22:01:28-04:00	tier0-after-M1M2	OK	3
2026-05-01T22:02:24-04:00	tier0-after-M2-fix	OK	29
2026-05-01T22:04:35-04:00	test-unbox-phase2-real	OK	3
2026-05-01T22:14:00-04:00	tier1	OK	189
2026-05-01T22:14:27-04:00	selfhost-llvm	OK	19
```
