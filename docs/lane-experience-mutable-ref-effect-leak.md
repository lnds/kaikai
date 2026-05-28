# Lane experience — Mutable effect-leak for `Ref` writes

**Date:** 2026-05-27 (overnight autonomous lane)
**Scope:** fix the effect-leak check so a function that writes a `Ref[T]`
via `Mutable.ref_set` (or the `:=` / `@` sugar, issue #275) cannot escape
declaring `/ Mutable`. Before the fix such a program compiled clean and
segfaulted at runtime.

## Scope as planned vs as shipped

Planned: find the hole in the effect-leak check that lets `Mutable.ref_set`
slip without `/ Mutable`, fix it, add a negative fixture. Shipped exactly
that, plus a deliberate *non-change* (see below) that bounds the lane
correctly against a deeper, separate bug.

## The bug

`fn helper(a: Ref[Int]) : Int = { Mutable.ref_set(a, 3); 0 }` compiled with
no `/ Mutable` on the row. `fn main() : Int / Console = { let a =
Mutable.ref_make(0); a := 3; ... }` compiled and then **segfaulted** (the
dispatched `ref_set` hit a NULL `Mutable` handler that was never installed,
because the row never carried `Mutable`).

Root cause (single point): `is_mutable_array_write_name` (infer.kai) — the
demand classifier the masking pass consults — recognised only `array_set` /
`array_grow` / `Mutable.array_set` / `Mutable.array_grow`. It did **not**
list `Mutable.ref_set`. So a body whose only mutation was a ref write had
"no recognised demand", `all_demands_local` returned vacuously true, and
`mask_local_mutable_demand` stripped `Mutable` from the row. The leak-check
(`check_body_row`) then saw a clean row and accepted the program.

The label *was* being raised correctly by `synth_op_call_with_scheme_keys`
(the #275 comment "effect-row injection rides for free" is true). The bug
was purely that the *masking* pass erased it.

## The fix (minimal surface)

One `else if name == "Mutable.ref_set"` arm added to
`is_mutable_array_write_name`. Now `all_demands_local_kind` routes a
`Mutable.ref_set` call through `demand_is_local`, which checks the target
(arg 0): a **parameter** Ref is never local → `Mutable` stays on the row →
the program is rejected at compile time with `effect not handled: Mutable`.

## The deliberate non-change (this is the load-bearing decision)

The obvious companion change — teaching `rhs_is_local_array_origin` to treat
`Mutable.ref_make` as a local-cell origin, so a *locally-constructed* Ref
masks `Mutable` like a local Array does — was **written and then reverted**.

Why: `array_set` is safe to mask because it has the bare-builtin bypass
(#558) and runs without any handler installed. `Mutable.ref_*` ops do NOT
have that bypass — they route through handler dispatch, which needs
`Mutable`'s default handler installed, which only happens when `Mutable` is
on the row. If we masked `Mutable` for a local Ref, the row would lose
`Mutable`, the handler would never install, and the dispatched `ref_*` op
would **segfault on a NULL handler**. Verified empirically: with the
ref_make-as-local change, `let r = Mutable.ref_make(0); Mutable.ref_set(r,
42); Mutable.ref_get(r)` in a `/ Console` function compiled (exit 0) and
then crashed (exit 139).

So Ref writes are conservatively **never masked**: any body that mutates a
Ref must declare `/ Mutable`. Every existing Ref fixture already declares
it, so nothing regresses. The Ref-local masking becomes safe to enable
only once `ref_*` gains a direct, handler-free lowering — which is the
KAI_REF first-class rework (the next lane). A code comment at the
`rhs_is_local_array_origin` site records this dependency.

## Fixtures

- `examples/negative/mutable/param_ref_write_no_row.kai` (+`.err.expected`)
  — the Ref companion to `param_array_write_no_row.kai`. Writing a Ref
  parameter without `/ Mutable` must be rejected with `effect not handled:
  Mutable`. This is the regression canary for the bug shape.

Coverage gap closed: there was no negative fixture for ref-write-on-param
(only the array analogue). Now there is.

## What did NOT regress (verified)

- Array-local masking positives (`array_local_masks_mutable`,
  `array_read_only_no_mutable`) still compile without `/ Mutable`.
- All 8 negative mutable fixtures fail with golden match.
- The 3 Ref fixtures (`ref_sugar_basic`, `ref_sugar_cross_fn`,
  `mutable_ref_basic`) all declare `/ Mutable` and still compile.

## Cost vs estimate

Estimate: contained, one classifier arm + one fixture. Actual: matched. The
only surprise was the reverted companion change — discovering that Ref
masking is unsafe (unlike Array masking) because of the handler-dispatch
asymmetry. That discovery is itself valuable: it pins down exactly what the
KAI_REF rework must deliver before Ref-local masking can turn on.

## Follow-up for next lanes

- KAI_REF first-class rework: give `ref_*` a direct lowering (no handler
  dispatch), then Ref-local masking can be enabled safely (re-add the
  `Mutable.ref_make` origin + `Mutable.ref_set` to
  `rhs_is_local_array_origin`).
- Then Front A (RC churn): InferState hot fields become `Ref[T]` cells.
