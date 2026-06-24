# Lane experience — issue #895

Free-fn `fn[T:P]` instantiated at two or more types inside string
interpolation `#{...}` resolved every colliding call site to the
first-registered spec (wrong per-type ABI → corrupt value or
segfault). Independent of #897 (this lane branched from main with #897
PR #899 already merged).

## Scope as planned vs. as shipped

**Planned:** close the call-site position collision so that any
`fn[T:P]` instantiated at ≥2 types inside `#{...}` in one program, in
any order, for any colliding type pair, resolves each site to its own
spec.

**Shipped:** exactly that, via the type-disambiguation fix (the
issue's Option B). No surface or AST change; the fix is local to
`monomorph.kai`.

## Which fix — B (type disambiguation), not A (real source position)

The issue offered two routes. I spiked the architecture for both before
writing code:

- **Option A (real source position):** seed the interpolation reparse
  lexer with the `#{...}` body's true offset in the original source so
  distinct sites get distinct `(line,col)`. Rejected as invasive: the
  same `esrc` slice is reparsed *independently* in ~7 places (`infer`,
  `perceus`, `fnreg`, `emit_c`, `emit_shared`), and the recorded
  instantiation key (infer) and the lookup key (monomorph) must agree
  across all of them. The `EStr` node carries the literal's start
  position and `iscan_collect_rev` knows each body's offset within the
  span, so A is *feasible* — but threading a consistent base offset
  through every reparse call site and into `lex_new`/`tokenize` is a
  broad, fragile change for one symptom.

- **Option B (type disambiguation in `resolve_call_inst`):** the
  monomorphiser already had the exact infrastructure. `resolve_call_inst`
  falls back to `synthesize_inst_for_decl(d, args_, ...)` — which derives
  the correct `(tys, units)` by unifying the poly fn's formal param
  types against the call's actual `args_[i].ty` — whenever *no* RCS is
  recorded at a position (the `__pimpl_*` case). Its own comment already
  documented that this disambiguates interpolation collisions for
  protocol-method calls. The bug for a *free* `fn[T:P]` was that an RCS
  *is* recorded, so `find_mono_inst` returned the first by position and
  never reached the synth path.

B wins decisively: it reuses tested infrastructure, is local to one
file, leaves the parse/lex layers untouched, and fixes the resolution
correctness for the whole class (any reparse-coalesced position, not
just this fn).

## The fix

`resolve_call_inst` now:

1. Collects *all* RCS at `(name, callee.line, callee.col)` via
   `find_mono_insts_at`.
2. If they **collide** (`mono_insts_collide`: ≥2 distinct `(tys,units)`
   at the same position), synthesises from the call's actual arg types
   (`synth_inst_pair`) so each site retargets its own spec; falls back
   to first-match if synthesis declines (never worse than before).
3. Otherwise (zero or one distinct tuple at the position) keeps the
   exact old first-match path — **byte-identical behaviour for genuine
   distinct-position sites and the common single-inst case**, which is
   what preserves the selfhost fixed point.

`find_mono_inst` (the old single-result helper) had `resolve_call_inst`
as its only caller and was deleted.

The collision branch only ever fires for the reparse-coalesced case:
two genuinely distinct source positions can never share an identical
`(line,col)`, so non-interpolated code never enters it.

## ≥3-type verification

The regression fixture interpolates `twice[T:Plus]` at **Int, Real, and
a user `Vec2 = V(Real, Real)`** — three distinct specs all recorded at
the slice-local `(1,1)`, confirmed via `--dump-mono`
(`twice@1:1 [Int] / [Real] / [Vec2]`). A wrong-ABI read on `Real` or
the boxed `Vec2` (Real fields) is catastrophic, not lucky-survivable
like Int/String. Emitted C verified per site:
`kai_twice__mono__Int(kai_int(5LL))`,
`kai_twice__mono__Real(kai_real(2.5))`,
`kai_twice__mono__Vec2(...)` — each spec applied to its own type, in
**both** source orderings. The issue's smoking gun
`(kai_twice__mono__Int(kai_real(2.5)))->as.r` is gone.

## ASAN result

`-fsanitize=address,undefined`, both orderings of the 2-type repro plus
the 3-type case plus the single-inst and let-form regressions: all exit
0, correct values, zero sanitizer diagnostics. Native backend (the
default) gives identical correct output for every variant.

## Non-interpolated / single-inst paths do not regress

- Single instantiation inside `#{...}` → correct (no collision, old
  path).
- Two types via `let` + `int_to_string`/`real_to_string` (not
  interpolated) → `10`/`5`.
- Genuine distinct-position non-interpolated sites (two `twice(...)` on
  different lines) → `10`/`5`.
- Full `test-protocols` (incl. `eq_in_interpolation`,
  `free_fn_eq_bound`, the multi-proto and derive cases) → all OK, zero
  DIFF.

## Fixtures added

`examples/protocols/free_fn_interp_mono_collision.kai` (+ `.out.expected`),
wired into the auto-globbed `test-protocols` tier. Exercises 2-type and
3-type instantiation, both orderings, all interpolated, in one program.
The issue noted no existing fixture did this — `free_fn_eq_bound.kai`
only instantiates at Int via direct calls.

## Structural surprises

- The synth-disambiguation machinery already existed and was already
  documented as handling interpolation collisions — for protocol-method
  (`__pimpl_*`) calls. The free-fn case was a gap in *reaching* it, not
  a missing capability. The fix is "let the existing path win when the
  recorded key is ambiguous," not new logic.
- **Selfhost stale-bootstrap trap:** the first selfhost run DIFF'd —
  but the diff was *only* the dead `find_mono_inst` function. I had
  deleted it from source *while* the background selfhost was mid-build,
  so `kaic2b` embedded the old source and `kaic2c` the new. A clean
  rebuild + re-run gave byte-id OK. Lesson: never edit compiler source
  while a selfhost is running.

## Cost vs. estimate

Single-file fix (~70 LOC of small helpers, one deletion), one fixture,
one retro. The bulk of the wall time was the byte-id selfhost and the
serial native parity gate, not the change.

## Follow-ups

None. Option A (real interpolation source positions) remains a
*latent* improvement — it would make `--dump-mono` show distinct
positions and could help any future tooling that keys on interpolation
positions — but it is not needed for correctness and is not deferred
work: B closes the bug class completely.
