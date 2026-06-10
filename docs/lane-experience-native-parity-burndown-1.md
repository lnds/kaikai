# Lane experience — native-parity burn-down 1 (unbound-register + missing-symbols)

## Scope as planned vs as shipped

**Planned (brief):** the first native-parity burn-down lane. Close the two
largest *mechanical* gap families measured by Lane 1.5 — `unbound-register`
(35 fixtures) and `missing-symbols` (24) — verifying each closed fixture
against the C-direct oracle, and lower `tools/native-parity-baseline.txt`
in the same PR. The brief flagged the precedent: a few root causes cover
many fixtures (one unlisted handler produced 344 empty walk fns in Lane 1.2).

**Shipped:** the precedent held strongly. The 24 `missing-symbols` fixtures
were **one** root cause; the 35 `unbound-register` fixtures were **four**
distinct lowering bugs in `lower_var` / the pattern-binder lowering, of
which the mechanical ones closed and the rest resolved to *other* families
(nested-variant-test, switch-codegen, output-mismatch) that were masked
behind the unbound-register abort. Net: **30 gaps closed** (168 → 138),
both families' mechanical cores cleared, the residue documented with its
root cause for the next lane.

The count is 30, not 59, because **resolving the mechanical bug surfaces
the next bug on the same fixture.** A fixture that aborted at "unbound
register" now links and runs — and several then diverge or crash from a
*different* family (a nested variant tag-test that was never emitted, a
duplicate switch case, a wrong computed value). The brief anticipated this
("if a fix surfaces a new failure of another family, document it"); it is
the dominant shape of the lane.

## Root causes found and fixed

**missing-symbols (24 → 0): one symbol.** Every one of the 24 fixtures
failed to link with `Undefined symbols: _kaix_assert`. The KIR lowering
emitted `KPrim("kai_assert", [cond])` — a one-arg prim that maps to
`kaix_assert` in the native walk — but the runtime only ever defined
`kaix_assert_check(cond, msg)` (the C-direct oracle's `emit_assert_stmt`
target). The lowering was *doubly* wrong: wrong symbol AND it dropped the
message. Fix: lower `SAssert(cond, opt_msg)` to
`KPrim("kai_assert_check", [cond, KStrV(msg)])` (mirror of emit_c's
`msg_lit`), and add a mixed-signature `nemit_assert_check` native arm
(`void(ptr, i8*)`, message as a de-escaped global string via
`llvm_build_string_span`). The 24th fixture (`contracts_passing`) confirms
the message path: native output is byte-identical to C-direct.

**unbound-register: four bugs in one family.**

1. **const / prelude-fn-value in `lower_var` (~10 fixtures).** An `EVar` in
   value position that named a top-level `const` (`ANSWER`, `real_pi`) or a
   prelude builtin used first-class (`int_to_string` passed to `map`) fell
   through to a bare `KVar` the backend never bound. The C-direct oracle
   inlines a const's literal body (`const_body_of` → `emit_expr`) and builds
   a closure over the prelude thunk (`prelude_find` → `EP`). Fix: thread the
   top-level consts into `LowerSt`, and walk the SAME cascade in `lower_var`
   — nullary-ctor → const-inline → fn-value (now incl. the 4 first-class
   prelude thunks `kaix_{print,eprint,write_stdout,int_to_string}_thunk`,
   already in `runtime_llvm.c`) → local binder.

2. **record destructure in `SLet` (1 fixture).** `let { p: { x, y } } = b`
   bound nothing — `lower_let_bind` only handled `PBind`/`PWild`, dropping
   any destructuring pattern to a value-discarding `KDo`. Fix: route a
   record/variant/as `SLet` pattern through the shared binder lowering.

3. **nested sub-pattern binds in `match` arms (a slice).** A variant arm's
   sub-pattern that was itself a plain record (`Pair { fst: _, snd: x }`) or
   a record field that nested a record never bound its inner names. Fix:
   recurse `bind_pattern_fields` into nested plain-record sub-patterns
   (mirror of emit_c's `emit_pat_binds_variant`). Closed
   `nested_record_pattern` and `rc_discipline_record_variant`.

4. **default-arm binder (3 fixtures).** A `match` whose catch-all arm binds
   the scrutinee — `x : LowerSlug -> shout(x)` (a `PNarrow`), `s -> ...` (a
   `PBind`) — lowered the body without binding the name. Fix:
   `lower_default_arm` binds the default pattern (`bind_default_pattern`)
   before the body.

## Structural surprises the brief did not anticipate

1. **`missing-symbols` was a lowering bug, not a runtime-emission gap.** The
   family signature ("the object references a symbol the native path never
   defines") read as "add the missing shim." The real fix was upstream: the
   KIR lowering emitted the WRONG symbol. The shim (`kaix_assert_check`)
   already existed. One-line family.

2. **A naïve nested-bind fix is a false-green hazard (asu review, guard A).**
   The instinct — recurse the binder lowering into *every* nested
   sub-pattern — silently miscompiles a nested VARIANT sub-pattern. A nested
   `PVariant` (`Node(_, Node(Red, a, ..), ..)`, `Some(JReal(r))`) carries a
   tag discriminant that NO test has emitted; binding its inner names
   against an untested value compiles but diverges at runtime. The first
   draft did exactly this and `reuse_nested_subpattern` *passed* — but only
   because the one test datum happened to take the matching branch (the
   inner node WAS `Red`). With a non-matching datum it would diverge. The
   fix routes nested variant sub-patterns to a loud trap
   (`bind_unsupported_nested`) that leaves the binder unbound so the backend
   refuses the module — turning a false-green into an honest gap. The
   discriminant: a nested sub-pattern is mechanical iff every decision node
   in it is already covered by an emitted test; a nested record (single
   shape, no tag) qualifies, a nested variant does not. This cost one
   apparent "pass" (`reuse_nested_subpattern`) but the pass was a lie.

3. **One mechanical fix unmasks 2-3 downstream families per fixture.** After
   the binder fixes, the rb-tree / regex fixtures advance from "unbound
   register" to "Duplicate integer as switch case" (nested variant arms
   sharing a tag need a decision tree, not one `KSwitch`); the JSON fixtures
   to "nested variant needs a tag test"; `collatz`/`factorials` link but
   compute the wrong value (output-mismatch). These are real, distinct
   burn-down items now *visible* because the link/bind no longer aborts
   first. Documented, not chased.

## Fixtures added / coverage gaps

- No new positive fixtures: the closed fixtures ARE the regression
  coverage, locked by lowering `tools/native-parity-baseline.txt` 168 → 138
  (the set-equality ratchet rejects any of the 30 re-diverging).
- `docs/native-parity-gaps.md` regrouped: `missing-symbols` → 0;
  `unbound-register` 35 → the residual nested-variant-test slice, with the
  newly-unmasked families (switch-duplicate, output-mismatch) attributed.

## New module + quality

`stage2/compiler/kir_lower_bind.kai` (148 LOC, `km score` **A+ 95.0**,
cogcom avg 0.9 / max 1) — the pattern-binder lowering, split out of
`kir_lower_walk.kai` (which the added recursion would have pushed further
over the size cap). Mirror of the `kir_lower_match` / `kir_lower_rec` split:
base ← bind ← walk, no `lower_expr` recursion, acyclic. Carries guard A as
its load-bearing soundness invariant (the test/bind split note at the top).

## Real cost vs estimate

The diagnosis dominated: capturing the exact unbound-register name per
fixture (`int_to_string` vs `ANSWER` vs `x`/`y`/`n`/`r`) split the 35 into
four root causes immediately, and the 24 missing-symbols collapsed to one
`grep` of the link error. The asu consult on the test/bind boundary (guard
A) was the highest-leverage step — it caught a false-green before it shipped
and drew the exact line between this lane (mechanical binds) and the next
(nested-variant-test decision trees). Most wall-clock went to kaic2 rebuilds
(libLLVM link) and the 513-fixture ratchet run.

## Follow-ups for next lanes

- **nested-variant-test** (json `r`/`n`, binserialize `Some([x,y])`): a
  nested variant/list sub-pattern in a match arm needs a tag/length TEST in
  the decision tree (oracle = emit_c `emit_pat_test_list` /
  `emit_pat_binds_variant` test arms). `bind_unsupported_nested` traps these
  loud today; closing them is control-flow lowering, not a bind.
- **switch-duplicate** (rb-tree `balance_*`/`is_red`, regex `subsume_*`):
  variant arms that share a top tag but discriminate on a nested sub-variant
  lower to a `KSwitch` with duplicate integer cases; needs a nested decision
  tree under the matched case. Same family as nested-variant-test.
- **output-mismatch unmasked** (collatz, factorials, hashmap/hashset, ...):
  link now, compute wrong — pre-existing codegen bugs the unbound-register
  abort hid. Separate family.
- **handler-state unbound** (`log` in m7b writer): the handler state free
  name is not bound on the native walk — effects family, not this one.
- **forth multi-scrutinee** (`match a, b`): the parser desugars to nested
  EMatch with duplicated fall-through arms; the native lowering diverges on
  that shape — its own item.
