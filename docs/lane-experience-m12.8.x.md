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

## Phase 2 — protocols hardening

Eduardo built a 70-line external demo (`/tmp/kaikai-portfolio-demo`)
to exercise m12.8 + m12.5 + the Phase 1 derives, and surfaced 4
bugs + 1 gap, documented as `docs/m12.8-followup.md` and reproduced
here as Phase 2 of the same lane.

### Objective metrics

- **Start**: 2026-04-26T21:08:12-04:00
- **End**: 2026-04-26T21:43:00-04:00 approximately (build TSV closes
  at 21:42:27 + the docs commit a few minutes later).
- **Wall-clock**: ~35 minutes for impl + fixtures + first commit.
  Docs/report a few minutes on top.
- **Build/test invocations** (TSV at the bottom): the headline figure
  is `make all` (6 s incremental) / `make test` (133 s) / `make
  selfhost` (11 s) / `make selfhost-llvm` (8 s) all green on the
  final build. One earlier `make test` run included a fixture-data
  bug fix iteration (the `m12_8_y_show_real_unit.kai` fixture
  initially missed `impl Show for String`, which fired the
  dispatcher panic on the lifted-interp path; trivial fixture
  edit, did not require a compiler change).
- **LOC added in `stage2/compiler.kai`** (from `git diff --stat`
  vs. phase-1 commit):
  - LLVM `EReal` literal fix: ~10 LOC.
  - `finalise_typed_expr` walker (Bug 2 / Bug 1): ~115 LOC.
  - `desugar_interp_decls` walker (Bug 4): ~155 LOC plus the
    driver wiring (~5 LOC).
  - `unit_name` intrinsic (Gap 1, partial): ~50 LOC across the
    typer entry, prelude name list, C/LLVM emit interception, and
    the `unit_name_of_ty` helper.
- **Fixtures**: 5 new `examples/protocols/m12_8_y_*.kai` plus their
  golden outputs. ~140 LOC of fixtures, 13 LOC of `.expected`.

### Errors I encountered

- **Phase-1 `m12_8_y_show_real_unit.kai` panic**: the first version
  of the fixture defined `impl Show for Int` and `impl Show for
  Real` but not `impl Show for String`. After the Bug-4 desugar
  lifted `"a = #{a} #{unit_name(a)}"` into a `string_concat_all([…
  show(unit_name(a)) …])` chain, the `show(unit_name(a))` call
  reached the `__pimpl_Show_String` registry slot — empty — and
  panicked. Added `impl Show for String { fn show(s: String) :
  String = s }` to the fixture; resolved. Underlines a real
  ergonomic note: every type that appears in a `#{…}` expression
  must have a `Show` impl reachable in the unit. Stdlib provides
  the primitives, so users who load `--prelude
  stdlib/protocols.kai` rarely hit this.
- **LLVM rejected `double 100`**: `real_to_string` of an integer-
  valued double produces no `.`. LLVM IR's grammar requires a
  decimal point for `double` constants. First build failed with
  `integer constant must have integer type`. Added a `if
  string_contains_dot(s) { s } else { concat_all([s, ".0"]) }`
  guard. Trivial.
- **`show` was unresolved after the desugar lift**. The lift
  initially emitted `EVar("show")`. The `lower_protocols` rename
  pass had already run (it walks decls before the desugar runs),
  so the bare `show` was never rewritten to `__proto_show` — and
  the resolver flagged "undefined name `show`". Fixed by emitting
  `EVar(proto_dispatcher_name("show"))` directly from the desugar.

### Friction points

- **Realising Bug 1 was already fixed**: my first repro
  (`#derive(Show) type Inner = A|B|C; #derive(Show) type Outer =
  { tag: Inner, n: Int }`) ran clean. Spent five minutes
  convincing myself this wasn't a flaky reproduction before
  realising the typed-AST finalisation pass needed for Bug 2
  also resolves Bug 1's "field type unresolved at synthesis time"
  problem. Documented Bug 1 as "Fixed (incidental — see Bug 2)"
  with a fixture that exercises the previously-panicking shape.
- **Locating the actual root cause of Bug 2**: the followup spec
  hypothesis ("run rewrite after lambda specialisation") was on
  the right scent but not the cleanest fix. Lambda
  specialisation/monomorphisation does not happen yet in stage 2
  (the m4c retrofit is identity). The right intervention is one
  level lower: the typed AST simply has stale `Expr.ty`
  annotations from `synth_lambda` that no pass refreshes. A
  finalisation walker is much smaller and self-contained than
  re-architecting the rewrite to know about lambda bodies.
- **Wrestling with Gap 1's parametric-impl design**: I prototyped
  three approaches for `impl Show for Real[u: Unit]` mentally
  before deciding the per-unit specialisation was a structural
  refactor. The lane spec's "PARAR y reportar" rule applies here.
  The reflection intrinsic `unit_name(x)` ships as the practical
  workaround, leaving the parametric impl as a separate follow-up
  lane (m12.8.z).

### Spec ambiguities or interpretive choices

- **Where to put the `#{…}` desugar**: between `lower_protocols`
  and `check_program` was the obvious place. The desugar takes
  `op_names` to decide whether to lift, so it must run after the
  lower-protocols pass populates the registry. It must run
  before `check_program` so the resolver/typer sees the lifted
  form. The phase-2 driver inserts it there.
- **Conditional vs. unconditional lift**: lifting unconditionally
  would break files that use `#{int_to_string(x)}` without
  declaring or loading `Show` — `show` would be unresolved.
  Conditional on `Show` being declared keeps the legacy path
  working; the desugar's first line is `if not list_has(op_names,
  "show") { ds }`.
- **`unit_name` arg shape**: the followup spec wrote
  `unit_name(u)` with `u` as a type-arg. kaikai has no value-level
  binding of unit names, so the implementation takes the
  dimensioned value: `unit_name(x: Real<u>) : String`. The
  emitter reads `x.ty` (now properly substituted by the typed-AST
  finalisation pass) to extract the unit symbol. `Real<USD>`
  yields `"USD"`, bare `Real` yields `""`, and an unresolved
  `Real<UVar(_)>` (the protocol-impl-generic case) also yields
  `""`. The empty fallback keeps the emitter total.
- **1-based vs 0-based variant tagging in Phase 1's hash derive**:
  not Phase 2, but worth noting that the same "no special-case
  panic, fall through to a defensible default" instinct applies
  here.

### Subjective summary

- **Confidence in correctness**: high for Bugs 2, 3, 4 and Gap 1
  (intrinsic). The fixtures are deterministic and exercise both
  backends. Bug 1 is "trust by construction": my fixture exercises
  the shape that previously panicked and now prints correctly,
  and the reasoning chain (Bug 2 fix → typed AST is fully
  resolved → Bug 1's nested-derive call sites get pinned types)
  is direct. A pre-fix git checkout would have given a
  bisect-confirmed proof, but the time cost was higher than the
  documentation gain.
- **Wall-clock vs estimate**: the followup said "~50-100 LOC each".
  Phase 2 added roughly 350 LOC across the four bug fixes plus
  the intrinsic — about 70-90 LOC per fix, in line with the
  estimate. The single-pass-fixes-multiple-bugs phenomenon (Bug
  1 falling out of Bug 2's fix) saved time the followup
  conservatively budgeted as separate.
- **Hardest fix**: Gap 1. Not because the intrinsic itself was
  hard, but because deciding what to ship vs. what to defer
  required understanding the protocol-impl machinery's
  type-key (P, T_head) limitation, which is not obvious from
  the surface. The other fixes (LLVM real, lambda dispatch,
  interp lift) were straightforward once the right pass was
  identified.

### Sub-phase comparison: Phase 1 vs Phase 2

| Phase | Theme                                  | LOC added (compiler) | Wall-clock | Hardest piece            |
|-------|----------------------------------------|----------------------|------------|--------------------------|
| 1     | `#derive(Eq/Hash)` for sum types       | ~289                 | ~16 min    | typer-level validation   |
| 2     | Protocols hardening (4 bugs + 1 gap)   | ~340                 | ~35 min    | Gap 1 scope-bounding     |

Phase 1 was *adding new feature surface*; Phase 2 was *plugging
real demos' edges*. Phase 2's hardest piece was strategic
(deciding what to defer), not technical. Phase 1's was technical
(designing the validation pass to surface clean diagnostics
before the typer runs).

The most leveraged single change in Phase 2 was the typed-AST
finalisation walker (`finalise_typed_expr` and friends). That ~115
LOC pass fixed Bug 2 directly, fixed Bug 1 incidentally, and is
the load-bearing prerequisite for the `unit_name` intrinsic
(which reads concrete unit symbols from `.ty` post-substitution).
Without it, three of the five issues would not resolve.

### Validation against the external demo

Three forms verified end-to-end against
`/tmp/kaikai-portfolio-demo/portfolio.kai` and two natural-form
variants:

| Form                          | Workarounds                                        | C output           | LLVM output        |
|-------------------------------|----------------------------------------------------|--------------------|--------------------|
| `portfolio.kai` (as shipped)  | manual `impl Show for Tx`, `print_tx` named fn     | `845 USD` ✓        | `845 USD` ✓ (was `0 USD` pre-fix) |
| `portfolio_v2.kai`            | `each(txs, (t) => print(show(t)))` + `"#{x} USD"`  | `845 USD` ✓        | `845 USD` ✓        |
| `portfolio_v3.kai`            | `each(txs, lambda)` + `"#{x} #{unit_name(x)}"`     | `845 USD` ✓        | `845 USD` ✓        |

The pre-phase-2 `portfolio.kai` printed `0 USD` on LLVM; phase 2
restores backend parity. The two natural-form variants exercise
Bug 2 (lambda dispatch), Bug 4 (interp), and the Gap-1 reflection
intrinsic — all green on both backends.

### Limitations of this report

- Self-report bias acknowledged: the agent that designed and wrote
  every fix is the same agent reporting on it.
- The fixture-data error in `m12_8_y_show_real_unit.kai` (missing
  `impl Show for String`) was caught immediately and added on the
  first iteration — this report makes it look like a clean
  incident, which is generous.
- Bug 1's "incidental fix" claim is reasoned but not formally
  proven by a pre-fix bisect. The fixture matches the followup
  spec's shape and now passes.
- Single agent (Claude Opus 4.7, 1M context). LOC/min figures are
  not generalisable across LLMs.
- The Gap-1 "deferred" decision is a one-agent judgement on
  whether per-unit protocol specialisation fit phase 2's budget.
  A different reviewer might call it tractable; the unit-name
  intrinsic ships either way as a forward-compatible wedge.

### Phase 2 build TSV (raw)

```
timestamp	cmd	outcome	elapsed_s
2026-04-26T21:29:10-04:00	make test	OK	174
2026-04-26T21:39:32-04:00	make all	OK	0
2026-04-26T21:41:53-04:00	make test	OK	133
2026-04-26T21:42:11-04:00	make selfhost	OK	11
2026-04-26T21:42:27-04:00	make selfhost-llvm	OK	8
```
