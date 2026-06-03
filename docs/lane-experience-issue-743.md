# Lane experience — issue #743: field access on a generic effect-op payload

## The reported cause was wrong; the real one is a timing problem

The issue (and my first attempt) blamed effect-row inference: "the
generic effect op's payload tyvar is never bound to the row's ty_args."
That framing sent the fix into `synth_op_call_with_scheme_keys` to
unify the op's fresh payload tyvar against the *declared* row's
ty_args. That first fix shipped, went green on its own fixtures, and
**broke a different fixture** (`m7b_11_followup_diag_type_args`) — it
was reverted (commit c7e3163).

The actual root cause, verified with a `TRACE` print inside
`synth_field` on a clean baseline:

```
effect Repo[a, i] { find(id: i) : Option[a] }
type Box = { id: String }
fn get(id: String) : String / Repo[Box, String] = match Repo.find(id) {
  Some(b) -> b.id      # <- rejected: "`id` is a function in scope, not a field"
  None    -> "none"
}
```

- `Repo.find` instantiates `∀a,i. i -> Option[a]` with fresh `?a, ?i`;
  the argument pins `?i = String`; `?a` stays **free**.
- The scrutinee is `Option[?a]`; `synth_arms` does
  `check_pattern(Some(b), Option[?a])`, binding `b = ?a` (still free).
- `synth(body)` then walks `b.id`. In `synth_field`,
  `apply_ty(sub, ty(b))` is a **free tyvar**, not `TyCon(Box)`. It falls
  to the `_` arm → `field_non_record_or_partial_app` → finds `id` as a
  function in scope → bogus UFCS diagnostic.
- `?a` is only pinned to `Box` *later*, by `check_body_row` unifying the
  inferred row `Repo[?a, String]` against the declared `Repo[Box, String]`.

`--dump-typed` shows `b : Box` and `Option[Box]` — but that is the
*finalised* post-substitution type. At the moment `synth_field` had to
decide field-vs-UFCS, the receiver was still free. **It is a timing
problem: an irreversible decision (field vs UFCS) taken with incomplete
information, not an effect-propagation problem.**

The owner caught this: the symptom "`b.id` says `id` is a function when
`b` is obviously `Box`" is a symbol-resolution shape, not an effects
shape. The whole "sources of truth / effect row" framing was a detour.

## Why the first fix broke m7b_11

```
fn ask_int_from_str_decl() : Int / Reader[String] = Reader.ask() + 1
```
Here the body `?a + 1` *determines* `?a = Int`, contradicting the
declared `Reader[String]`. The fixture expects the row-mismatch
diagnostic `effect not handled: Reader[Int]`. The first fix bound `?a`
eagerly to the declared `String`, so `String + 1` produced an
*arithmetic* error and the row-mismatch never fired. The two fixtures
pull opposite ways only if you let the declared row drive the payload:
repro's body *consumes* the payload (compatible with binding it),
m7b_11's body *determines* it (binding it eagerly clobbers the body).

The deferred-constraint fix sidesteps the conflict entirely: it never
touches op-call inference, so m7b_11's body still freely infers
`Reader[Int]` and `check_body_row` reports the mismatch unchanged.

## The fix: deferred field-access constraint (asu's call, Opt 1(a))

When `synth_field` meets a receiver whose resolved type is an unbound
`TyVarT`, it does **not** guess UFCS. It records a `DF(recv_id, field,
result_id, line, col)` in a new `InferState.deferred_fields` worklist,
stamps the access with a **fresh result tyvar** (not `TyAny`, so
downstream uses stay connected through unification), and moves on.
`infer_decl` drains the worklist **after** `check_body_row` — by then
the receiver is resolved:

- concrete record with the field  → unify the result with the field type.
- concrete record without it       → missing-field error on the concrete
  type. *This closes the Tier 1 escape*: `b.zzz` on a generic-effect
  payload is now rejected at compile time instead of compiling silently
  / stamping TyAny.
- any other concrete type           → field-on-non-record error.
- still unbound at drain time        → "type annotation needed" (the body
  never pins the receiver; mirrors OCaml warning 18, consistent with
  "safety beats ergonomics").

This is sound and single-pass: no solver, no backtracking. It postpones
a deterministic decision that was being taken at the wrong time, and
drains a finite worklist (one entry per syntactic field access on a free
receiver) once. Precedent: OCaml/Elm/Koka all defer field access until
bidirectional context resolves the receiver; none guess by field name.

## Why not bind the receiver by field-name lookup (Opt 1(b))

Searching `recs` for "the record that declares field `id`" turns field
access into backward structural inference (record polymorphism), which
HM-nominal kaikai forbids, is ambiguous when two records share a field
name, and would degrade the T4 missing-field check. Rejected.

## Implementation surprises

- **Result must be a fresh tyvar, not `TyAny`.** The pre-existing
  non-record fallback stamped `TyAny` "for graceful cascade". If the
  deferred path did that, `b.id`'s uses in the body (unifying with the
  other match arm `"none" : String`) would lose the connection and the
  drain could not back-propagate. The fresh tyvar lets the arm unify
  pin it, and the drain confirms consistency.
- **Drain strictly after `check_body_row`**, not before — that pass is
  what pins `?a → Box`.
- **`InferState` flat-record tax again.** The new `deferred_fields`
  slot touched ~20 constructors (a `perl` one-liner over the closing
  pattern covered 15; the initial constructor, the `[d, ...]` diag-push
  variant, and the `synth_lambda` merge were hand-edited). `ret_ty`,
  the reverted `decl_row`, now `deferred_fields` — third decl-scoped
  field; if a fourth lands, fold them into a `DeclCtx` sub-record.

## Fixtures

- `examples/effects/issue_743_generic_effect_payload_field.kai`
  (+ `.out.expected` = `found`) — positive: full triple condition,
  type-checks and runs on C + LLVM.
- `examples/effects/issue_743_generic_effect_payload_bad_field.kai`
  (+ `.err.expected` = `no field 'zzz' on Box`) — negative: the Tier 1
  escape canary, rejected on the concrete type.
- `stage2/Makefile` `test-issue-743` (wired into `test` + `.PHONY`).

Verified by hand and kept as notes, not pinned as fixtures (they belong
to the typer's general machinery, not this bug shape): the
"annotation needed" path (`fn pick(o: Option[a]) : Int = match o {
Some(b) -> b.foo ... }` → clean diagnostic, no crash) and m7b_11
staying green (the regression the first fix introduced is gone).

## Load-bearing sites

- `stage2/compiler/infer.kai` — new `DeferredField` type;
  `InferState.deferred_fields` + ~20 constructor updates;
  `st_push_deferred_field` setter; `synth_field`'s new `TyVarT` arm +
  `ty_var_id` helper; `drain_deferred_fields` /
  `drain_one_deferred_field`; the drain call in `infer_decl` after
  `check_body_row`.

## Cost vs estimate

The reverted first attempt was the expensive part — it cost a full
baseline-vs-HEAD mapping to discover it had introduced exactly one
regression (m7b_11) while every other CI red was pre-existing (mostly
LLVM). Once the owner reframed the bug as symbol-resolution and a TRACE
confirmed the receiver was free at the field site, the correct fix
followed asu's Opt 1(a) directly. The deferred constraint is ~70 lines
plus the constructor tax.

## Follow-ups

- The pre-existing LLVM TCO bug (a `String` param corrupted across a
  tail-recursive call — `rep(i,n,acc) = ... rep(i+1,n,acc)` prints `2`
  not the string) is what reddened `test-modules-path` in CI. Separate
  lane; this fix does not touch it.
- proposed-extensions #12 (`f.field` always field access, UFCS needs
  `f.method()`) remains the edition-boundary change that would retire
  the field-vs-UFCS ambiguity by construction. Independent of this fix.
