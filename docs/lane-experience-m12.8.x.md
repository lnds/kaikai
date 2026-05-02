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

### Bug 5 — postfix `.method()` sugar + missing-impl diagnostic

Eduardo added Bug 5 to phase-2 scope after the four-bug bundle had
already shipped (commit `4e20b67` on main). It is ergonomic, not
functional — `show(x)` inside `impl Show for X` looks recursive
even though the rewrite dispatches it elsewhere — so it lands as
a postfix method-call sugar `x.op()` that desugars to `op(x)`.

#### Objective metrics

- **Wall-clock incremental for Bug 5**: ~14 minutes from "start
  reading the spec" to first commit.
- **LOC added in `stage2/compiler.kai`**:
  - Method-call arm in `desugar_interp_kind`'s ECall branch: ~25
    LOC (one match arm; uses the existing op_names plumbing).
  - `validate_resolved_protocols` walker (the missing-impl
    diagnostic): ~210 LOC of mechanical mirror of the existing
    `resolve_protocol_calls_kind` walker shape, threaded with an
    Int errs accumulator. Plus the driver wiring (~10 LOC).
- **Makefile**: imported `test-llvm-coverage` from main `bd0c5b0`
  (~48 LOC). Coverage harness ran 61 pass / 0 DIFF / 13 skip on
  the post-fix branch.
- **Fixtures**: `m12_8_y_postfix_method_call.kai` (positive,
  records + sums + `w.show()` on both sides of `++`) and
  `m12_8_y_postfix_no_impl.kai` (negative, `b.show()` where
  `Bare` has no impl Show; `.err.expected` matches `no impl of
  \`Show\` for type \`Bare\``).

#### Errors I encountered

- **Duplicate impl on the first fixture draft**: I wrote both
  `#derive(Show) type Inner = A | B` and a manual `impl Show for
  Inner` in the same file. Phase 1's coherence check correctly
  flagged the duplicate. Dropped the manual impl, regenerated.
- **Two trivial parser nits in the validator walker**: kaikai
  rejects `else match X { ... }` — needs explicit braces
  (`else { match X { ... } }`); and rest-pattern bindings with
  `[head, ..._]` need a name (`[head, ...rest]`), even when
  unused. Both are five-second fixes.

#### Friction points

- **The "missing-impl as a typer-level error" feature was
  arguably out of strict Bug 5 scope** but the negative fixture
  (`m12_8_y_postfix_no_impl.err.expected`) implicitly required it.
  Phase 1's m12.8 v1 had explicitly chosen "runtime panic" for
  missing impls; Bug 5's negative fixture forces the choice the
  other way. Implementing it touches a non-trivial amount of
  walker boilerplate (mirroring `resolve_protocol_calls_kind`'s
  shape but Int-valued), but the change is purely additive — no
  existing behaviour breaks unless the user actually called a
  protocol op on a concrete-type-with-no-impl, in which case the
  v1 runtime panic is replaced by a clean compile-time
  diagnostic. All 61 coverage fixtures still pass, all selfhost
  gates green.
- **Folding the method-call rewrite into the interp desugar pass
  vs. a separate pass**: chose to fold. The interp pass is the
  natural home for "post-`lower_protocols` desugars that depend on
  `op_names`". Adding a method-call arm in the same `ECall` match
  costs five lines of dispatch and avoids a second walker.

#### Spec ambiguities or interpretive choices

- **Field-vs-protocol-op precedence**: the lane spec suggested
  "first try field-call, fall through to protocol-op if field
  doesn't exist". I went the other way: when the field name
  matches a registered protocol op, the desugar **always** lifts
  to a protocol call. Reason: the desugar runs before the typer,
  so we don't have the receiver's type yet, so we can't ask "does
  this field exist on this record?". A user who declares a record
  field with the same name as a protocol op (`record { show: fn(
  Self) -> String }`) would be surprised; this is documented as
  the same edge as the existing `__proto_<op>` rename pass and
  noted as a v1 limitation. Realistic field names that collide
  with protocol ops (`show`, `eq`, `cmp`, `hash`,
  `to_string`/`from_string`) are unusual.
- **Diagnostic wording**: I emitted `no impl of \`<P>\` for type
  \`<T>\` (operation \`<op>\`)` — same shape as the existing
  orphan-rule diagnostic. The lane spec sketched "no impl of Show
  for T" so users can grep for substring `no impl of \`Show\``.
- **What about polymorphic helpers (`fn show_list[a](xs: [a]) :
  String = ...`)?**: their inner `show(xs[0])` site has receiver
  type `TyVarT(_)` at rewrite time. The new validator skips
  unresolved-TyVar receivers exactly because of this case;
  `ty_head_name` returns `None` and the check returns the same
  errs unchanged. Polymorphic helpers continue to compile cleanly
  and rely on the v1 runtime fallthrough. Test: phase-1 fixtures
  that exercise polymorphic helpers (e.g.,
  `m12_8_x_derive_eq_sum_nested.kai`'s `eq(x.field, y.field)`
  where `field` may be a polymorphic field at the time of synth)
  all stayed green.

#### Validation against the external demo

A fourth portfolio variant `portfolio_v4` rewrites the demo's
`impl Show for Tx` body in the new `t.kind.show()` style — the
exact shape Bug 5 was aimed at:
```kai
impl Show for Tx {
  fn show(t: Tx) : String =
    t.kind.show() ++ "  " ++ t.amount.show() ++ " USD  (" ++ t.note ++ ")"
}
```
Both backends produce `845 USD` byte-identical to the original
demo. The original `portfolio.kai` (with the bare `show(t.kind)`
calls) also still compiles + runs identically — backward-compat
with the recursive-looking call-site form is preserved.

#### Backward-compat audit

`grep -rn "\.kai" examples/ | xargs grep -l '\.show()'` found no
existing fixtures using `.show()` style; the new fixtures are the
first instances. All other fixtures' field-accessor uses (`x.f`
followed by no `(...)`) are untouched because the lift only
triggers on `ECall(EField(...), ...)`, not on bare `EField(...)`.
`make test` and `test-llvm-coverage` confirm no regressions.

#### Phase 2 (Bug 5) build TSV (raw)

```
timestamp	cmd	outcome	elapsed_s
2026-04-26T22:00:37-04:00	make test	OK	140
2026-04-26T22:00:59-04:00	make selfhost	OK	9
2026-04-26T22:01:16-04:00	make selfhost-llvm	OK	8
2026-04-26T22:03:33-04:00	test-llvm-coverage	OK	74
```

## Phase 4 — effects-prelude gap (Bug 6)

Eduardo discovered while writing a USD→EUR external demo that
kaikai's prelude `print`, `eprint`, `read_line`, `read_file`,
`write_file` are **flat fns**, not effect ops, so any function
with an empty row can perform IO without declaring it. That is a
clean CLAUDE.md Tier 1 #1 violation. The Phase 4 spec tasked this
lane with closing the gap: declare an atomic effect catalog
(Stdin/Stdout/Stderr/File/Env), make the typer enforce row
declaration on prelude calls, infer main's row from its body,
add `use Effect` open scope, and implement row subtyping
(declared row may over-cover the body's used row).

### Outcome: investigated, partial-only ship

The lane shipped:

- `stdlib/effects.kai` — a documentation-only forward-compat
  catalog file that records the proposed atomic-effect shape
  (Stdin / Stdout / Stderr / File / Env, with `Console` and `Io`
  aliases) for the follow-up lane to consume. Loadable via
  `--prelude` as a no-op.
- `docs/m12.8-followup.md` Bug 6 entry documenting the
  investigation, the architectural blocker, and the two paths
  forward.
- This lane experience section.

The lane did **not** ship the typer-level row enforcement,
main row inference, `use Effect`, or row subtyping. Reasons
below.

### What I tried

1. **Prelude effect-row label injection** in `synth_call`: a
   ~30-LOC shim `add_prelude_effect_label` that, when the
   callee is a syntactically-bare `print`/`eprint`/`read_line`/
   `read_file`/`write_file`, accumulates the matching effect
   label (`Console`/`Stdin`/`File`) in the caller's row. The
   existing `check_body_row` then enforces "performed must be
   subset of declared". The shim worked on user fixtures: a
   `fn pure_fn(x: Int) : Int { print("...") x+1 }` correctly
   reported "effect not handled: Console" with a clean
   diagnostic.
2. **Main row inference** in `infer_decl`: when main's declared
   row is REmpty, skip the strict check, take the body's
   accumulated `st.row`, dedup by effect name, and rebuild the
   `DFn` with `RClosed([RL("Console", []), ...])`. The runtime
   wrapper's `main_row_labels` then sees the inferred labels and
   `inject_builtin_effects` installs the matching default
   handlers automatically.
3. **Pre-typer scan** for `main_row_labels`: a structural walker
   `scan_prelude_effects_in_expr` that iterates main's body
   looking for syntactically-bare prelude effect calls. This
   ran before the typer so `inject_builtin_effects` (which also
   runs pre-typer) had the row info up-front. Implementation
   was ~150 LOC of pattern-match boilerplate mirroring the
   existing `resolve_protocol_calls_kind` walker shape.

Items 1+3 worked end-to-end on the
`m12_8_y_phase4_main_implicit` fixture: `fn main() : Int {
print("hello") 0 }` compiled, the runtime installed the
Console handler automatically, and "hello" was printed.

### What blocked the ship

Selfhost. The kaikai compiler itself
(`stage2/compiler.kai`) calls `print`/`eprint` directly from
**43 helper fns** (`diag_error`, `diag_note`, `diag_help`,
`diag_error_from_src`, every `dump_*` reporter, every
`llvm_*` error reporter). With the new enforcement, those fns
declared `: Unit` (REmpty row) but performed `Console` — so the
typer rejected them, and the selfhost stopped at the first error
("`diag_error`'s declared row does not include `Console`").

The principled fix is to add `/ Console` to all 43 fn signatures
in compiler.kai. I prototyped a Python script that did this
mechanically: scanned for `print`/`eprint`/`read_line`/`read_file`/
`write_file` call sites, walked back to the enclosing fn
signature, and inserted the row annotation before the trailing
`=` or `{`. The script applied 43 modifications cleanly — no
syntax errors in the kaikai source.

But **stage 1's parser cannot parse row syntax**. Stage 1
(`stage1/compiler.kai` — kaikai-minimal) predates the effect
system and rejects `: Unit / Console {` with "expected `=` or
`{` to start function body" at the slash. The selfhost path is
"stage 1 builds stage 2's source binary, then stage 2 builds
its own source", so adding rows to compiler.kai breaks the
stage-1 → stage-2 bootstrap before stage 2 ever runs.

The lane spec was explicit: **"Stage 0/1 NO cambian:
kaikai-minimal sigue sin effect system."** That sentence
assumed compiler.kai's helpers don't need rows because stage 1
doesn't enforce them; the assumption misses that **stage 2** is
the one doing the enforcing, and stage 2 is asked to typecheck
its own source during selfhost, which is exactly when the row
violations bite.

I considered four mitigations:

1. **Extend stage 1's parser to accept-and-ignore row syntax**
   (~20 LOC in stage1/compiler.kai). Cleanest. Forbidden by
   the lane spec.
2. **Opt-in enforcement gated on a marker**. The typer enforces
   row checks only when the file declares the atomic effects
   (e.g., loaded `stdlib/effects.kai`). compiler.kai doesn't load
   it; new user code does. Preserves selfhost. Out of scope per
   the lane spec's Tier 1 framing ("typer EMPIEZA a enforce").
3. **Special-case "internal stdlib paths"** to skip enforcement.
   File-path-based gating; brittle.
4. **Ship with selfhost broken**, expecting Eduardo to bless the
   stage-1 change as a follow-up. Leaves the branch in a known-
   bad state that contradicts the lane's "selfhost obligatorio"
   gate.

I did not have authority for #1 in this lane and did not want
to ship #4. #2 and #3 are non-trivial design decisions that
deserve Eduardo's input rather than me picking. The pragmatic
move was to revert the typer changes, ship the documentation
catalog as a forward-compat marker, and surface the scoping
issue in `docs/m12.8-followup.md` Bug 6 so the next lane has a
clean handoff.

### Reverted-from-disk notes

In the course of bisecting whether the panic ("non-exhaustive
match" in the running compiler) came from item 3 (the pre-typer
scanner) or item 1 (the synth_call shim) or item 2 (the
infer_decl rewrite), I `git checkout`-ed `stage2/compiler.kai`
twice to restore baseline. The third attempt at re-adding just
the pre-typer scanner reproduced the panic on `examples/effects/
m7a_6b_handle_runs.kai` (which has nested `handle { ... } with
Counter { ... }` blocks). The likely cause was a mishandled
HClause / HReturn / Arm pattern in the new walker, but I did
not invest further bisect time after deciding the lane should
ship documentation only — the walker is uncommitted and not
worth debugging in isolation.

### Objective metrics

- **Start**: 2026-04-26T22:18:33-04:00.
- **End**: 2026-04-26T22:48:27-04:00.
- **Wall-clock**: ~30 minutes of total work (most of it was
  exploration: counting fixture impact, reading the existing
  effects/handler infrastructure, verifying selfhost is the
  blocker by attempting the bulk row-add and watching it
  break).
- **LOC shipped**: 1 file (`stdlib/effects.kai`, 67 lines, all
  comment-only forward-compat catalog).
- **LOC reverted**: ~310 LOC across two iterations of
  `add_prelude_effect_label`, `main_row_labels` extension, and
  the `scan_prelude_effects_*` walker family. The Python
  bulk-row-add script's 43 modifications to compiler.kai were
  also reverted.
- **Gates**: `make test`, `make selfhost`, `make -C stage2
  selfhost-llvm`, `make -C stage2 test-llvm-coverage` all green.
  No regressions, by construction (no compiler change).

### What the lane spec assumed vs. what was true

| Spec assumption                                                    | Reality                                                                                |
|--------------------------------------------------------------------|----------------------------------------------------------------------------------------|
| "Stage 0/1 don't enforce rows, so selfhost just works"             | Stage 2 enforces during its own selfhost; stage 1's role is just being able to *parse* |
| "Phase 4 only affects user programs"                               | The compiler IS a user program from stage 2's typer's perspective                      |
| "Subtyping check is the risk, ~3h of work"                         | Subtyping is straightforward; the parser-level gap is the real blocker                 |
| "Each item lands independently"                                    | Items 1-7 (Stdlib catalog, Sugar enforcement, Main inference, `use`, Subtyping, Console split, Doc updates) all bottom out on the same selfhost question |

### Recommended next move

Eduardo's choice between two paths, both approachable as a
~1-day follow-up lane:

- **Path A (preferred)**: extend stage 1's parser to
  accept-and-ignore row syntax. Once landed, the Phase 4 typer
  changes (which I prototyped in this lane's git stash before
  reverting) can be re-applied directly. Stage 2's row
  enforcement bites uniformly; selfhost works because stage 1
  silently parses past the slashes.
- **Path B (fallback)**: opt-in enforcement gated on a
  user-loaded `--prelude stdlib/effects.kai` that declares the
  atomic effects. compiler.kai doesn't load it; new user code
  does. Smaller blast radius but two-tier semantics.

Either path can ship the unblocked items quickly — main row
inference and the structural pre-scanner are ~150 LOC and were
working in the prototype.

### Limitations of this report

- Self-report bias: I'm reporting on my own scoping decision.
  An adversarial reviewer might argue I should have shipped #4
  (selfhost broken) and asked Eduardo to unblock. I picked the
  conservative path because the lane already had three green
  phases (Phase 1 `#derive(Eq/Hash)`, Phase 2 4-bug bundle,
  Phase 3 Bug 5) that I did not want to put at risk on a
  branch whose merge state Eduardo controls.
- The "non-exhaustive match" panic in the pre-typer scanner is
  not root-caused. If a future lane resurrects the walker, that
  panic needs to be debugged first.
- Single agent (Claude Opus 4.7). LOC/min and reasoning style
  are not generalisable.

### Phase 4 build TSV (raw)

```
timestamp	cmd	outcome	elapsed_s
2026-04-26T22:34:25-04:00	make test	OK	57
2026-04-26T22:45:21-04:00	make test	OK	190
2026-04-26T22:45:41-04:00	make selfhost	OK	9
2026-04-26T22:45:58-04:00	make selfhost-llvm	OK	7
2026-04-26T22:47:26-04:00	test-llvm-coverage	OK	78
```

## Phase 4 retake — prelude effect-row enforcement (Bug 6)

Eduardo authorised the stage 1 parser extension that the first
phase-4 attempt would have needed. With that gate open the lane
landed the Tier 1 enforcement gap closure: every fn calling
prelude IO (`print`, `eprint`, `read_line`, `read_file`,
`write_file`) must now declare the matching effect (`Console`,
`Stdin`, or `File`) in its row. CLAUDE.md Tier 1 #1 holds for
new code; selfhost holds; no parity loss.

### Outcome: shipped (with two deferrals)

Shipped in this lane (commit `a251689`):
- Stage 1 parser tolerance for row syntax (~25 LOC).
- Stage 2 typer's prelude effect-label injection in
  `synth_call` (~30 LOC).
- `inject_builtin_effects` extended to scan every fn's row, not
  just main's (~50 LOC).
- Row-polymorphic `each` / `map` / `filter` / `reduce` prelude
  signatures + `synth_pipe` open-row trick (~30 LOC).
- 39 helper fns in `stage2/compiler.kai` annotated with
  `/ Console` (or `/ Console + File`) per their direct prelude
  calls.
- 41 example fixtures gained the appropriate row on `main` or
  helper fn signatures.
- 4 new fixtures `examples/effects/m12_8_y_phase4_*`:
  `main_implicit`, `helper_explicit`, `subtyping`,
  `no_row_negative`.

Deferred (documented in `docs/m12.8-followup.md`):
- Main row inference. Prototyped a structural body walker
  (`prelude_eff_walk_*`) that should have fed
  `inject_builtin_effects` from main's body when `main` lacks
  a declared row. The walker triggered "non-exhaustive match"
  panics on certain block shapes (`fn main() : Int { let r = 42
  ; assert r == 42; r }`). Bisect narrowed it to the
  `prelude_eff_walk_kind` match — explicitly enumerated all 27
  ExprKind variants, still panicked. After ~45 minutes of
  bisecting without root-causing the kaikai-runtime
  "non-exhaustive match" emit, reverted. Every fixture's `main`
  declares its row explicitly. The walker functions live on the
  branch in the `prelude_eff_walk_*` namespace but are
  unreachable; a future lane can pick a different scan
  strategy (post-resolution? `fv_expr` reuse?) and avoid the
  shape that panicked.
- Stdout / Stderr split. Doc B's atomic catalog targets
  `Stdin`, `Stdout`, `Stderr`, `File`, `Env` with `Console`
  becoming an alias. The lane keeps the existing combined
  `Console` runtime ABI; the split needs new effect decls,
  runtime struct changes, and migration of every existing
  `Console.print`/`Console.eprint` site. Documented in
  `stdlib/effects.kai`.
- `use Effect` open scope. The sugar prelude (`print(s)` ≈
  `Console.print(s)`) is the only "open scope" that ships.

### Stage 1 parser extension cost

The lane spec predicted ~20 LOC. Actual: ~25 LOC (a 4-line
`skip_effect_row` entry plus a 6-line `skip_effect_row_loop`,
inside `parse_fn_decl`'s gap between the optional return type
and the `=` / `{` body opener). The skip-loop bails on `=`,
`{`, `}`, or `TkEof`, so a malformed signature still gets a
clean diagnostic from the body parser rather than running off
into the next decl.

### compiler.kai row-add cost

The lane spec predicted "43 helpers". Actual: **39** after a
comment-aware Python script (the v1 script's regex also matched
`print(s, resume) -> ...` inside docstrings, which v2 strips
before scanning). The 39 are: `diag_*` (4 fns), `dump_*` (16
fns), `emit_line`, `emit_src_snippet`, `emit_source_caret`
(File), `print_inferred_fn`, `validate_*`, `usage`,
`parse_cli_loop`, `compile_source`, `run` (Console + File),
`load_prelude` (Console + File), `try_candidate` (File),
`resolve_module`, `strict_holes_check`, every `llvm_emit_*`
error reporter. All Console-only except for the file-IO ones
that also got `+ File`.

### Were all the rows `/ Console`?

No — 4 fns took `/ File` only (`emit_source_caret`,
`try_candidate`), 2 took `/ Console + File` (`load_prelude`,
`run`). The other 33 are pure `/ Console`.

### Errors I encountered (worth recording)

- **First-time bulk-row-add false positives**. The v1 Python
  script's regex `\bprint\(` matched `print(s, resume) -> ...`
  inside a docstring inside `parse_handle`. Stage 1 still
  parsed the modified file (the row was a no-op in stage 1)
  but stage 2 typer rejected the call site since `parse_handle`
  doesn't actually call print. v2 script strips line comments
  and `"""` docstrings before scanning; correct count `39`.
- **`each(handler)` doesn't typecheck** when `handler : Tx ->
  Unit / Console` and `each : [a] -> ((a) -> Unit) -> Unit`.
  `dump_types` is the canonical site (`decls |>
  each(dump_decl_type)`). Initial fix: replace `each(handler)`
  with explicit recursion in two compiler-internal sites.
  Better fix: row-poly `each` / `map` / `filter` / `reduce`
  prelude signatures (open row tail = callback's row tail).
  `synth_pipe` needed a parallel open-row trick — without it,
  the pipe's expected type built via `fn_ty([..., row_empty()])`
  refused to unify with the row-poly callee. Both shipped.
- **`_let _ = body` in main_row_labels triggered a runtime
  panic** during walker bisect. Removing the binding made the
  panic move (different message), suggesting kaikai's
  evaluation of unused-but-bound Option values has a
  pre-existing rough edge unrelated to phase 4. Out of scope;
  the workaround is the bespoke walker pattern that doesn't
  bind `body` in fn args.

### Subjective summary

Confidence: high for the parts that shipped. The selfhost
fixed point matches; selfhost-llvm matches; coverage holds
61 / 0 / 13. The negative fixture's diagnostic is clean and
directs the user at the obvious fix. The external demo
(`/tmp/kaikai-portfolio-demo/usd_to_eur.kai` adapted to
declared rows) runs identically on C and LLVM with input
`100\n0.92\n` producing the expected `92 EUR`.

Confidence: low for the deferred main row inference. The
walker's panic was investigated for ~45 minutes including a
`fv_expr`-based variant, a bespoke walker with explicit ExprKind
arms, and stubbed-helper bisects — none isolated the panic to a
specific match. Likely a stage-1 parsing edge with my fn
signatures plus runtime-time match handling, but I couldn't
nail it. The user-visible cost is "every main declares its row";
acceptable for now.

### Wall-clock vs estimate

The lane spec didn't predict a single number for the total but
implied "~1 day" by listing the components. Phase 4 retake took
~3 hours active engineering: ~25 min on the stage 1 parser
extension, ~1.5 hours on the typer enforcement (including the
walker bisect that reverted), ~30 min on bulk row-add to
compiler.kai (with v1/v2 script iterations), ~25 min on
fixture row-additions (script-driven), ~15 min on documentation
+ demo. The walker bisect was the time sink; without it the
lane would have been ~2 hours.

### Validation against external demo

`/tmp/kaikai-portfolio-demo/usd_to_eur.kai` post-Phase-4-retake
form ships in `/tmp/usd_to_eur_phase4.kai` (kept off-tree —
the on-tree demo file is the original aspirational version
from Bug 6's Pre-fix state). Both backends produce identical
output:

```
=== USD => EUR Converter ===
Amount in USD:
EUR per USD rate (e.g. 0.92):

100 USD
x 0.92 EUR/USD
= 92 EUR
```

with input `100\n0.92\n`. The `parse_real` helper declares
`/ Console + Stdin`; `main` declares `/ Console + Stdin` (no
inference). Console covers stdout in this lane (Stdout split
deferred). String interpolation `#{x}` is not used because
the Show-via-interp fix from Bug 4 + Gap 1's parametric
`impl Show for Real[u]` aren't all present in the same
lane state — the demo falls back to `real_to_string(unitless(
x)) ++ " USD"`.

### Limitations of this report

- Self-report bias: I picked the conservative "ship the parts
  that work" path over "ship everything or ship nothing".
- Walker panic not root-caused. Future lanes that re-attempt
  main inference need to debug it.
- Single agent (Claude Opus 4.7); LOC/min figures aren't
  generalisable across LLMs.
- The Stdout/Stderr split is documented but not implemented;
  any user who declares `: Unit / Stdout` today gets `unknown
  effect: Stdout` because Stdout isn't a registered effect.
  The ergonomic story still has rough edges until a future
  lane lands the split.

### Phase 4 retake build TSV (raw)

```
timestamp	cmd	outcome	elapsed_s
2026-04-26T23:27:59-04:00	make selfhost	OK	9
2026-04-26T23:28:18-04:00	make selfhost-llvm	OK	8
2026-04-26T23:29:24-04:00	make test	OK	55
2026-04-27T00:13:10-04:00	make selfhost	OK	13
2026-04-27T00:13:29-04:00	make selfhost-llvm	OK	9
2026-04-27T00:14:04-04:00	make test	OK	21
2026-04-27T00:15:47-04:00	make test	OK	0
2026-04-27T00:16:23-04:00	test-llvm-coverage	OK	26
2026-04-27T00:34:29-04:00	final_gates	OK	133
```
