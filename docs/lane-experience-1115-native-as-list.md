# Lane experience — #1115: native as-binding over list-destructure

## Scope

**As planned:** `whole as [h, ...rest]` (as-binding whose inner is a
list-destructure) compiled correctly on the C-direct oracle but aborted on the
native backend with `unsupported KIR node (subset gap): unbound register h/rest`.
Pure register/scope threading — zero RC risk. Fix the KIR lowering so the inner
`PList`'s cells are emitted, then bind the as-name to the whole scrutinee.

**As shipped:** exactly that, plus two sibling recognisers that the same bug
shape needed for completeness (see below). One compiler file touched
(`stage2/compiler/kir_lower_match.kai`), one regression fixture with golden.

## Root cause

`lm_emit_arm` routed a `PList` arm through `lm_emit_cells` (which emits the
cons/nil discrimination + head/tail binds), but every other pattern — including
`PAs(name, PList)` — fell to `lm_bind_whole`, which binds only the as-name and
branches straight to the body. The inner list's head/tail registers were never
emitted, so the body read them unbound. The C oracle never had this gap: its
`emit_pat_test` delegates a `PAs(_, sub)` to `emit_pat_test(sub)`, and its
`emit_pat_binds` binds the as-name then descends into the inner's binds.

## Fix

Three arms, all mirroring the oracle:

1. `lm_emit_arm` — a new `PAs(name, PList(..))` arm routes the inner list
   through the shared `lm_emit_list_arm` (extracted from the old `PList` arm),
   then binds `name` to the scrutinee in the matched body block.
2. `list_arm_is_catchall` — a `name as [..]` is NOT a catch-all (its inner list
   carries a length test), so it must not terminate the fail chain. Recurses
   through the as-inner.
3. `match_is_list` — recognise `PAs(_, PList)` so a `match` whose sole
   list-shaped arm is an as-binding still takes the list-decision-tree path
   rather than the variant `KTagOf` path (which deref-faults on a list
   scrutinee in native).

The #858 structural incref already covered these binders: `pat_alias_binders`
descends `PAs(name, inner)` and collects `[name, h, rest]` unchanged, so the
arm-body dup in `lower_list_arms` balances the new as-name bind with no extra
work.

## Structural surprises

- The bug was reachable two ways: the common `whole as [h,..] | []` shape (the
  `[]` sibling triggers `match_is_list`), and the arm-only `whole as [h,..] | _`
  shape (needs `match_is_list` to recognise the as-binding itself). Fixing only
  `lm_emit_arm` would have left the second shape routing to `KTagOf`. All three
  recognisers had to move together.
- The existing sugar fixture `m7d_14_at_pattern_basic.kai` was already the exact
  repro (`list as [first, ...rest]`) — but `test-sugars` runs C-only, so it was
  green while native aborted. The regression fixture lives in `examples/llvm/`
  (covered by the backend-parity harness) so native is actually exercised.
- #1116 (Ref-sugar) landed on main mid-lane and migrated the as-binding surface
  from `@` to `as`. The rebase was conflict-free (#1116 touched the parser + the
  `m7d_14_*` fixtures; this lane touched `kir_lower_match.kai` lowering + a new
  `examples/llvm/` fixture — disjoint), but the new fixture's surface had to be
  hand-migrated `@` → `as`. The AST node is still `PAs`; only the spelling moved.

## Fixtures

- `examples/llvm/as_binding_list_destructure.kai` + `.out.expected` — the repro,
  uses `whole`, `h`, and `rest` together; walked by `tools/test-backend-parity.sh`
  (native vs C oracle). Precedent: `examples/llvm/variant_list_slot_sibling_bind.kai`
  (the same "list-slot lowering leaves a sibling register unbound in native"
  shape).

## Verification

- Repro: native output byte-identical to C (was `unbound register` before).
- `test-sugars` green (C path unregressed, all `m7d_14_at_pattern_*` OK).
- Backend-parity over `examples/llvm`: pass, 0 fail.
- Native self-host gate: 0 subset-gap aborts, self-compile byte-identical.

## Follow-ups

None. The C oracle already handled every `PAs(_, X)` shape; this lane closed the
native gap for the list-inner case, the only one that lowered through
`lm_emit_cells`.
