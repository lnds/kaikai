# Lane experience — issue #903: unsound `Mutable` masking escape

## Scope as planned vs as shipped

**Planned:** split the mutation-carrier predicate so a record with a transitive
`Array` field counts as observable when forwarded to a `/ Mutable` callee, while
locally-constructed Arrays keep masking. One-line seed change expected.

**Shipped:** the predicate split was one line (`["Ref"]` → `["Ref", "Array"]`),
but it surfaced a much larger truth: the compiler's own type inferencer threads a
`Subst` (a record of `Array` slots) through its entire unification backbone, and
that threading is mutation the row was silently hiding. Closing the soundness hole
honestly required:

1. The seed change (carrier set now includes `Array`).
2. A `/ Mutable` sweep across ~109 inferencer signatures — the closure of the
   unifier (`unify_env`, `unify_heads`, `unify_row`, `unify_unit`, the
   `mk_fresh_*subst` family, `st_unify`, `st_instantiate*`, `synth*`, handle
   clauses) — everything that threads `Subst`/`InferState` and transitively calls
   `subst_extend`. The cascade stops at `infer_decl`, where the `InferState` is born
   local, threaded, and discarded (only `TypedDecl` escapes). `pub fn infer_program`
   stays clean (`/ Console + File`, no `/ Mutable`): the typer's public border does
   not leak the effect.
3. Masking-completeness extensions so user code with the same *local* threading
   shape keeps masking (below).

## The boundary, stated precisely

A mutation carrier passed to a `/ Mutable` callee is **local (maskable)** vs
**observable (`/ Mutable` required)** on this rule:

- **Observable**: the carrier is a parameter the caller shares, or it aliases one
  (`forward(b: Box)`, `sneaky(b){writer(b); b}`, `Box{arr: param_arr}`,
  `wrap(shared)`). The mutation reaches the caller.
- **Local**: the carrier originates locally and no alias of a shared value reaches
  the callee. Three origin shapes count as local:
  - a local `EVar`;
  - a record literal whose carrier fields are all local-origin
    (`Box{arr: array_make(..)}`) — but `Box{arr: param_arr}` is not (it aliases the
    param's buffer);
  - a fresh constructor / in-place thread: a call whose result carries mutation and
    whose carrier args are all local-origin (`string_builder.new()`,
    `string_builder.append(local_sb, s)` — append returns the same buffer it grew).

### Why the inferencer can't mask but `foldl` can — the load-bearing distinction

Both `Subst`-threading and `foldl(xs, new(), \b -> append(b))` are linear
threading: a carrier is created, mutated in place, and the old alias is discarded.
Neither's safety is provable from the *body* of an intermediate function — a
parameter is indistinguishable shared-vs-linear from inside.

The difference is **where the applier lives**:

- `Subst` is threaded by ~56 remote callers of `st_unify`, dispersed across the
  call graph. The locality of the seed `subst_empty()` is invisible at `unify_env`.
  No local rule can recover it → declare `/ Mutable` honestly, mask once at the root
  (`infer_decl`) where the `InferState` is born and dies in one scope.
- A `foldl`'s applier (the `ECall`) and its `init` arg live in the *same expression*
  the masking is already walking. The locality of `init` is co-local with the lambda.
  So the lambda's accumulator params **inherit** the locality the `init` proved at
  this call site — but only after `mutable_callee_args_all_local` confirms every
  carrier arg of the HOF (including `init`) is local. A shared `init`
  (`foldl(xs, shared, ...)`) sets that check false and the params do not inherit
  locality. The inheritance is conditional on the init check, never independent.

That co-locality is the exact line: **a local masking rule exists when the applier
is co-local with the lambda; it does not when the applier is a remote caller.** One
case became the sweep (declare honest), the other became masking (recognise local).

## Design alternatives rejected (via asu consult)

- **B3 — "callee returns same carrier type ⇒ linear"**: unsound. `sneaky(b){writer(b); b}`
  returns the same-typed carrier yet the mutation is observable. Linearity is a
  property of the caller's use graph, not the callee's signature.
- **B2 — `#[opaque_buffer]` author opt-in (region/ST style)**: an `unsafe` in
  disguise. Without rank-2 verification (HKT is banned, Tier 1 #3) the attribute is
  believed unchecked; the day a caller retains the old `Subst` it lies silently. Pays
  permanent language surface to silence one module's internal signatures.
- **last-use of the body**: anti-correlated with the truth. `forward.b` is single-use
  (looks safe) and `st_unify.st` is multi-use (looks dangerous) — exactly backwards,
  because a parameter is always caller-shared and the safety lives in the callers.

## Structural surprises

- The cascade was expected to stop in a small unifier sub-tree. It does not — `Subst`
  lives inside `InferState`, threaded by the ~212-fn `/ Console + File` backbone. The
  real cut is `infer_decl`, where the whole `InferState` is local. The sweep was
  measured empirically (iterative build → annotate next wave → converge) at ~109
  signatures, not hand-estimated.
- The sweep is **not** noise that makes `/ Mutable` meaningless. The signal lives at
  the module border (`infer_program` stays clean) — "the inferencer is a subsystem
  that mutates" is a true, discriminating statement against lexer/parser/formatter.
- `string_builder.append`'s in-place threading (`mixed1 = append(mixed0)`) needed the
  fresh/in-place-carrier origin rule; the array-only whitelist (`array_set`/`array_grow`)
  did not cover an `EModCall` `/ Mutable` op that returns its grown buffer.

## Fixtures added

- `array_record_forward_requires_mutable` (`.err.expected`) — the #903 reject.
- `array_record_forward_explicit` (`.out.expected` `0\n7`) — same with `/ Mutable`.
- `array_record_local_masks_mutable` (`.out.expected` `9`) — local-record-construct
  + mutate + return masks (the load-bearing non-regression).

Wired into `test-effects` (positive masks + negative reject blocks). The five
soundness edges (HOF-local mask, shared-init reject, captured-shared reject,
fresh-result-escapes mask, wrap-shared reject) were verified during development; the
three fixtures cover the user-facing surface, the rest are exercised by selfhost
(the compiler's own `Subst` threading is the in-the-large test of the sweep).

## Coverage gaps / follow-ups

- The masking pass is whole-function all-or-nothing: one non-local demand keeps
  `/ Mutable` on the whole row. Not a soundness gap, but a precision ceiling — a fn
  mixing a local thread and a genuinely-observable mutation declares the effect for
  both. No lane action; noted for anyone surprised by a broad row.
- The `/ Mutable` sweep could in principle be narrowed if the masking ever learns to
  recognise `InferState`-shaped threading, but that needs the remote-applier problem
  solved (it is not locally decidable today), so the honest declaration stands.
