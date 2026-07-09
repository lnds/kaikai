# Lane experience — #1148: native nested-nullary tag not discriminated in a cons-arm slot

## Scope as planned vs as shipped

**Planned** (issue): the native backend does not discriminate a nullary
variant nested in a cons-pattern head slot (`[PM(_, _, PConst), ...rest]`
vs `[PM(_, _, POther), ...rest]`) — the first arm matches both tags, a
silent miscompile. Fix the KIR lowering, fixture with two-backend proof,
sweep for sibling sites.

**As shipped**: exactly that, and the sweep showed the same dropped-test
defect covered a wider family than the issue's shape — payload-bearing
nested ctors, literal slots, variant-record heads, and tuple heads (a
tuple pattern desugars to a `PVariantRecord` of `Pair`/`Triple`) were all
tag-only-tested in cons-arm and record-field position. One shared helper
fixes the family. Two compiler files touched, two fixtures (each wired to
both a C golden and the native parity walk).

## Root cause — what the lowering emitted before vs after

The C-direct oracle's `emit_pat_test` recurses into every sub-pattern, so
a cons head `PM(n, _, PConst)` emits `tag == PM && tag(slot 2) == PConst`.

The native KIR lowering (`kir_lower_match.kai`) classified that head as
discriminating (`psub_is_discriminating` → `PVariant` → true) and routed
it to `lm_head_seal_test`, whose `PVariant(cn, _)` arm **dropped the
sub-patterns** and emitted only the head's own tag test:

```text
before:  .t3 = kai_tag_eq(.t2, 13)        # tag(head) == PM — both arms pass
after:   .t3 = kai_tag_eq(.t2, 13)        # tag(head) == PM
         .t4 = proj .t2.2
         .t5 = kai_tag_eq(.t4, 11)        # tag(slot 2) == PConst — discriminates
```

Since sibling arms share the head tag (`PM`), every scrutinee took the
first arm. In #1139 this made the natively-built compiler classify every
purity mark as `PConst` (746 `((const))` stamps vs the oracle's 90),
`println` included.

The same tag-only seal sat in two more places in the same file:
`PVariantRecord` heads (discriminating fields never tested — the
`lm_head_test_fields` arm for it was unreachable dead code, since a
`PVariantRecord` is always classified discriminating and diverted to the
seal path first) and `lm_field_test_on` (a ctor with discriminating
payload nested in a record field, tag-only again).

## Fix

The recursive machinery already existed in `kir_lower_variant.kai`:
`var_seal_variant` tests a slot's tag and descends into the payload's
discriminating sub-slots via `var_emit_slot_tests`. But it projects the
slot itself; the cons-arm path already holds the projected head value.

- `kir_lower_variant.kai`: extracted `var_seal_ctor_of` — the full tag +
  payload-sub-slot test over an *already-projected* value.
  `var_seal_variant` now projects and delegates to it; the now-dead
  `var_seal_tag` is removed.
- `kir_lower_match.kai`: `lm_head_seal_test` and `lm_field_test_on` route
  `PVariant(cn, subs)` through `var_seal_ctor_of`, and
  `PVariantRecord(cn, flds)` through a new `lm_seal_ctor_record` (tag
  test, then the existing `lm_field_tests` over the discriminating
  fields — fields live in this module, keeping the import direction).

Test-before-bind order is preserved: the binds
(`lm_bind_head_value` → `bind_pattern_fields_any`) were already sound and
unchanged; they were just reachable through a vacuously-true test. No RC
change — the added tests read borrowed projections (`KProj` /
`kai_op_field_borrow`) and immortal Bool results, same as every existing
test seal.

## Structural surprises

- **The compiler itself contains the bug shape.** `match_has_any_guard`
  (`kir_lower_walk.kai`) matches `[Arm(_, Some(_), _), ..._]` vs
  `[Arm(_, None, _), ...rest]`. Under a natively-built compiler this
  returned true for every non-empty match, silently routing all matches
  to the linear guard chain — when targeting native. It never tripped the
  native-selfhost byte-identity gate because that gate's SELF-COMPILE
  probe runs `--emit=c`: the natively-executed compiler's *native*
  lowering path is never exercised by the gate. #1139's purity divergence
  was visible only because purity stamping affects C emission too.
  Follow-up worth weighing: a native-target probe
  (`kaic2-native --emit=native` vs the oracle's) would close that
  blindness.
- **Tuples are in the blast radius.** `(PConst, v)` in a cons head is a
  `PVariantRecord` of `Pair`, so `[(PConst, v), ...rest]` vs
  `[(POther, v), ...rest]` was miscompiled the same way. Covered by
  `lm_seal_ctor_record` and the sibling fixture.
- A `PVariantRecord` nested in a *positional variant slot* (e.g.
  `Some(PM { kind: PConst })`) still falls to the unconditional-pass arm
  of `var_seal_test` in the variant switch path — a sibling latent gap in
  a different decision tree, out of this lane's cons-arm scope.

## Fixtures

- `examples/llvm/nested_nullary_tag_in_cons_arm.kai` + `.out.expected` —
  the issue's repro. Red on native before the fix (all three marks
  printed `const`; C printed `const/other/other`), green after.
- `examples/llvm/nested_ctor_tags_in_cons_arm_siblings.kai` +
  `.out.expected` — payload-vs-nullary (`Some(_)` vs `None`, the
  compiler's own shape) and the tuple-head nullary-field shape.
- Both fixtures are duplicated into `examples/match/` so the C backend
  asserts the golden in tier1 (`test-match` walks that directory), while
  the `examples/llvm/` copies ride the tier1-native parity walk
  (native vs C-direct oracle).

## Verification

- Repro + sibling fixture: native output byte-identical to C after the
  fix (both diverged before — native took the first arm for every tag).
- `--emit=kir` shows the nested `proj` + `kai_tag_eq` seal per arm.
- `make -C stage2 test-kir` green — no golden drift (the KIR goldens do
  not cover this shape).
- `make -C stage2 test-match` green including the two new goldens.
- Serial backend parity (`BACKEND_PARITY_JOBS=1`, llvm+perceus+effects)
  green; tier0 green locally; heavy gates (native selfhost byte-id,
  modular selfhost, full parity) on CI.

## Cost vs estimate

Small lane, as briefed: the zone hypothesis (`kir_lower_match.kai`) was
correct on first read, and the fix reuses the sibling module's existing
recursion. Most of the time went to the sweep and to proving why the
selfhost gate had been blind to a bug sitting in the compiler's own
lowering code.

## Follow-ups left for next lanes

- Native-selfhost gate blindness: the SELF-COMPILE probe only exercises
  `--emit=c` (see Structural surprises).
- `var_seal_test`'s unconditional-pass arm for `PVariantRecord` in a
  positional variant slot (variant switch path, not cons-arm).
- The `purity_const_syms` workaround in `purity.kai` (verdict-bound +
  predicate) is sound with and without this fix and was left untouched;
  reverting it to the cons-pattern form is optional cleanup, not owed.
