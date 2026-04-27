# Lane experience report — m12.8.x (`#derive(Eq)` / `#derive(Hash)` for sum types)

## Objective metrics

- **Start**: 2026-04-26T20:35:30-04:00
- **End**: 2026-04-26T20:51:05-04:00 (commit 5164898 + docs commit follow-up)
- **Wall-clock**: ~16 minutes for impl + fixtures + first commit. Docs
  reporting added a few minutes on top.
- **Build/test invocations** (TSV at the bottom): `make all` / `make
  test` / `make selfhost` / `make selfhost-llvm` — all green on the
  first invocation following the implementation. No retry loops, no
  bisects.
- **LOC added in `stage2/compiler.kai`**: 289 inserted, 1 deleted
  (per `git diff --stat`). Of those, ~120 LOC are the sum-type Eq +
  Hash expander helpers, ~140 LOC are the new validation pass
  (`validate_derives` + `collect_future_impl_keys` +
  `derive_builtin_impl` + the four mutually-recursive validators),
  and ~30 LOC are the `lower_protocols` wiring + the inline `match
  body { TBSum(...) -> ...}` arms grafted onto the existing
  `derive_eq_impl` / `derive_hash_impl`.
- **Fixtures**: 5 fixtures, 233 LOC of `.kai` plus 33 LOC of
  `.expected` files.

## Errors I encountered

None at the compiler level.

The only authoring error worth recording was a **fixture data
choice**: in `m12_8_x_derive_hash_sum_basic.kai` the first draft
compared `Tag(1, "hi")` to `Tag(1, "ho")`. With the toy
`hash(s) = string_length(s)` impl, both strings hash to 2, so the
collision check the fixture meant to demonstrate as "different"
came out as "same". Fixed by changing the second value to
`Tag(1, "hop")` (length 3). Caught on the first run of the fixture
itself; never reached the gate. This is a fixture-design bug, not a
compiler bug — it just shows that the lane's "rough collision
check" needs to use distinct hash inputs, not just distinct
contents that happen to hash equal under a trivial impl.

## Friction points

- The biggest design choice was where to put the **typer-level
  validation**. The lane spec asks the typer to reject sum-type
  derives whose variant fields lack `impl P`, but records currently
  do not validate at all (they fall through to the dispatcher's
  runtime `panic`). Three plausible places:
  1. Inside `expand_derives` — must change the fn's signature to
     return errors and tightens its responsibility.
  2. As a separate pass before `expand_derives` — chosen.
  3. After `lower_protocols` returned, in the driver — would have
     duplicated the decl-walking logic for no benefit.
  Option 2 wins because the precomputed "future impls" set is just
  a list of `Pname/Tname` strings collected from `DImpl` and
  `DDerive` decls; the validation is a pure pre-pass and the
  existing `lp.errs > 0` short-circuit in the driver fires before
  the typer runs, so the user sees the clean diagnostic and not a
  cascade of inferred-type errors from a synthesised body that
  cannot typecheck.
- **Variant binding-name shadowing**: the inner-match needs
  identifiers disjoint from the outer-match. I picked `la_0, la_1,
  ...` for the outer (matching `a`) and `lb_0, lb_1, ...` for the
  inner (matching `b`), with `h_0, h_1, ...` for the Hash variant
  bindings. No clash with the params (`a`, `b`, `x`) and
  human-readable enough that the dumped AST is grep-able.
- The Show-on-sum helper already exists, so the **shape of the
  outer-match expansion** (one arm per variant, `PVariant(name,
  pat_subs)`) is a known good pattern. Reusing
  `derive_sum_pat_subs`-style helpers (parameterised on prefix)
  meant only the body and the Eq inner-match were genuinely new
  code.

## Spec ambiguities or interpretive choices

- **Hash multiplier**: kept at `acc * 31 + h_field` to match the
  record helper. The lane spec said "the multiplier exact and the
  combination can be adapted to whatever the record expander does
  — consistency with the record helper is desirable", and 31 is
  the value the existing record helper hard-codes. No change.
- **Variant-tag numbering**: 1-based (first variant = tag 1). The
  spec example uses 1, 2, 3. Tag 0 was avoided so a constant variant
  with tag 0 does not collide with a derived `hash` of `0` from a
  primitive payload (e.g. `Variant1(0)` would otherwise have tag 1
  and value 0, with `(1 * 31) + 0 = 31`; using tag 0 would make
  `Variant1` and `Variant2` indistinguishable when both are
  constant). This is the same rationale 1-based tags carry in
  most "hash-by-discriminant" implementations.
- **Inline match vs helper fn**: inlined the entire match
  expression in the synthesised impl body. A helper-fn approach
  would produce smaller emitted code but would require the
  expander to track and inject auxiliary top-level fns; the AST
  emitter does not have a hook for that today. Deferred.
- **Empty sums**: defensive handling — `derive_eq_sum_body([])`
  emits `EBool(false)`, `derive_hash_sum_body([])` emits `EInt(0)`.
  The parser does not produce empty-variant sums today, but the
  expander cannot assume that; emitting a single value is cheaper
  than special-casing.
- **Records still fall through to runtime panic** for missing
  field impls. Symmetry would suggest validating records too, but
  this lane's scope was sum types; promoting the records check is
  trivial (apply the same `validate_derive_decl` to `TBRecord` arms)
  but slightly out of scope and might surface hidden regressions
  in existing fixtures. Deferred.

## Subjective summary

- **Confidence in correctness**: high. All four gates green. The
  Hash↔Eq invariant fixture exercises constant variants,
  single-field variants, and multi-field variants on both
  positive and negative pairs. Both backends produce the same
  output on every fixture.
- **Wall-clock vs estimate**: m12.8 said "30 LOC each if needed".
  The lane added ~289 LOC to the compiler. The estimate was
  understated by ~10×: it accounted only for the Eq/Hash sum-type
  expanders (which collectively are about 110 LOC of the diff),
  not for the validation pass (~140 LOC) which was a separate ask
  in the lane spec ("Validación de campos"). If the lane had been
  scoped to "expanders only, runtime panic for missing impls", the
  30-LOC estimate would have been roughly right (Eq sum: 60 LOC,
  Hash sum: 50 LOC, plus 5 LOC of dispatch glue per impl — call
  it 60 LOC each rather than 30 because the inner-match for Eq
  needs four helpers and Hash needs three).
- **Hardest sub-task**: the typer validation. The expanders were
  mechanical once the shape was agreed. The validation needed
  thinking about ordering against `expand_derives`, building the
  `future_impls` set, deciding what to do when validation fails
  (let `proto_errs > 0` short-circuit), and writing a diagnostic
  string that names the variant + field clearly enough to be
  greppable in the `.err.expected` fixture.

## Validation of design assumptions

- **m12.8 estimate (~30 LOC each)**: did not hold. The actual
  expander work was ~110 LOC for both protocols combined; the
  validation pass added another ~140 LOC. The lane spec's own
  "Hard rules" plus the negative-fixture requirement effectively
  doubled the scope vs the m12.8 follow-up estimate.
- **Lack of tuples (m8.5 pendiente)**: did not force any awkward
  construction in this lane. The "outer-match per variant; inner-
  match for the second arg" pattern is exactly what the lane spec
  showed as the kaikai source, and it expanded cleanly via
  `EMatch` / `Arm` / `PVariant`. Tuples would let the user write
  `eq((a, b)) = match (a, b) { ... }` if they wanted to roll
  their own; the expander does not need them.
- **Hash↔Eq invariant**: no tricky cases. Tag-then-fold over
  field hashes is symmetric on both sides of `eq`, so equal values
  trace the same arithmetic. The invariant fixture confirms this
  empirically across constant, single-field, and multi-field
  variants.
- **`#derive` expander dispatching pattern**: as the lane spec
  predicted, generalising `derive_*_impl` to switch on `body`
  (TBRecord vs TBSum) and call into per-shape helpers was a clean
  extension point. The existing record code was not touched.

## Limitations of this report

- Self-report bias: the agent that designed and wrote the
  validation pass is the same agent reporting on it. The
  ergonomics judgement ("easy" vs "tricky") may be off for a
  reader who has not internalised the code.
- Single agent (Claude Opus 4.7, 1M context). Not generalisable
  across LLMs.
- Wall-clock figure (~16 min) is dominated by the absence of
  errors. A lane that surfaced any compile-time bug in the
  expander would have spent more time in the bisect/diagnose
  loop; this lane did not exercise that capacity, so the LOC/min
  number is best-case.
- Empirical correctness only — no formal proof of the Hash↔Eq
  invariant. The fixture covers the obvious shapes but does not
  bound out-of-Int hash overflow (Int wraps modulo `2^63`, and
  large field values combined with the `* 31` multiplier will
  wrap; eq remains structural so wrapped sums of equal inputs
  still match, but a property-based test would catch any
  non-commutativity bug — out of scope here).

## Build TSV (raw)

```
timestamp	cmd	outcome	elapsed_s
2026-04-26T20:43:29-04:00	make all	OK	6
2026-04-26T20:49:02-04:00	make test	OK	88
2026-04-26T20:49:17-04:00	make selfhost	OK	8
2026-04-26T20:49:32-04:00	make selfhost-llvm	OK	8
```
