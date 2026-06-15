# Lane experience — general Mutable masking (issue #531)

## Scope as planned vs as shipped

**Planned (brief):** replace the ad-hoc `Mutable` masking — string whitelists
(`is_mutable_array_write_name`), hardcoded type checks (`ty_is_mutable_ref`),
and the recent Ref-backed-record fixpoint that fed a *blanket reject*
(`dispatch_mutable_type_names` + `ty_is_dispatch_mutable`) — with a uniform
mechanism: install the `Mutable` default handler UNCONDITIONALLY at the base
of the evidence stack, so masking `Mutable` for ANY local mutable value
(Array, Ref, HashMap, future Matrix/Tensor) is always sound. Thin the masking
pass down to pure escape analysis plus a declarative list of base-installable
effects.

**Shipped:** exactly that, plus one piece the brief did not anticipate (the
`Subst` selfhost regression, below). The direct-vs-dispatch distinction is
gone; only escape matters. `local_hashmap_no_row` flipped from negative to
positive; #531's exact acceptance program runs.

## The root cause (confirmed, not just hypothesised)

The brief's diagnosis held up under the code: the `Mutable` default handler
was installed only around `kai_main` and only for effects in `main`'s inferred
row (`emit_c.kai` `emit_main_wrapper`, `default_setups_for`; the native mirror
is `kir_default_handlers`, kir_lower.kai). Masking dropped `Mutable` from a
fn's row; if that fn's local Ref/HashMap then dispatched `ref_get`/`hashmap.get`
through the evidence stack, there was no `EvMutable` installed → SIGSEGV. Array
worked by accident: `array_set` is a direct `kai_prelude_array_set` C call, no
handler needed. The fix removes the accident: the handler is ALWAYS there.

## Design decisions (two asu consultations)

**1. Unconditional install without breaking LIFO teardown.** Both backends
filter a fixed `install_order` against `main_row` to decide which defaults to
push; push and pop derive from the SAME filtered row, so the teardown is LIFO-
balanced by construction. The intervention is a single shared helper,
`ensure_unconditional_defaults(main_row)` (emit_shared.kai), that adds the
base-installable effects (today: `["Mutable"]`) to `main_row` before the
filter, **deduplicated**. asu confirmed dedup is load-bearing: it keeps
`main_row` a SET, so a `/ Mutable` already in main's row stays a single
push/pop pair. Verified the runtime `kai_evidence_pop()` is generic (just
`evidence_top = parent`, never touches `handle_jmp`/`discard_slot`), so the
3-arg no-jmp_buf default node pops uniformly — asu's one open worry, closed.

**2. The escape-analysis residual.** With the default guaranteed, the masking
collapses to: a callee `/ Mutable` is masked iff every argument that CARRIES
MUTATION is a local, non-parameter, non-escaping `EVar`. asu's verdict (Option
A): keep the record fixpoint as a uniform "does this type carry mutation?"
predicate; delete only the blanket-reject branch and the string whitelist. The
fixpoint was never the problem — the dispatch-REJECT was.

## The base-installable-effect criterion (general, documented)

An effect `E` is **base-installable** (eligible for the unconditional install)
iff **(a)** it has a non-parametric default handler installable statically (no
user-supplied init), AND **(b)** its observable effect dies with the local
value. `Mutable` qualifies (its default forwards each op to a `kai_prelude_*`
C primitive, carries no state). `Stdout` fails (b) — a flush must be observable
per call, not deferred to a base handler nobody asked for. `State`/`Spawn`/
`Cancel` fail (a) — they need a user-supplied initial value or scheduler.
Generalising the install to other effects is purely additive (extend
`unconditional_default_effects`) and deliberately out of scope here — the user
called only `Mutable`.

## Structural surprise the brief did not anticipate: the `Subst` selfhost break

First green-on-fixtures pass broke `make selfhost`: the compiler's own
`mk_fresh_unit_subst_loop` (which threads a `Subst` parameter through
`subst_unit_extend`) suddenly demanded `/ Mutable`. Cause: I had widened the
record fixpoint to follow `Array` fields, not just `Ref`. `Subst` is
`{ slots: Array[..], ... }` — an Array-backed record the compiler mutates
in-place under a linear-use discipline (`array_set(s.slots, ...)` returns the
same array), and it has NO `Ref`. Following `Array` made every `Subst`-threading
helper carry `Mutable`.

Fix: gate the record CLOSURE on `Ref` only (mirrors pre-#531 exactly). The
direct `Array[_]`/`Ref[_]` argument case stays covered by `ty_is_mutable_ref`
(so `array_set(arr_param, ...)` still rejects); only the transitive *record*
closure is Ref-gated. The distinction is real: a shared `Ref` cell is an
observable first-class mutation site; an Array embedded in an opaque record is
a linearly-used buffer the caller does not witness. This is the Tofte-Talpin
fresh-vs-borrowed line — `Subst` is borrowed-but-linear, `HashMap`'s `buckets`
Ref is the observable cell.

A second subtlety the brief did not call out: a qualified `Mutable.<op>` op-
call (`array_set`, `ref_set`, …) has its callee rewritten to `EVar("Mutable.op")`
whose `.ty` is the op's RESULT type, not a `/ Mutable` fn type (the effect lives
on the caller's row). So `ty_callee_has_mutable` alone misses it; the demand is
recognised by the `Mutable.` dispatch-key prefix (`callee_is_mutable_op`) — an
effect-level check, NOT a per-op whitelist (every Mutable op flows through it).

## The local-origin rule (Tofte-Talpin fresh-vs-borrowed)

For masking to fire, the mutated value must be recognised as locally
constructed. The generalised `rhs_is_local_origin` (asu-validated): an alias
`EVar(n)` propagates locality from `n` (never decided by type); an
`if`/`match`/block-tail is local iff every tail branch is; an in-place op
(`array_set` of an already-local arg) propagates from that arg; **any other
call is a fresh-value constructor iff its result type carries mutation AND no
argument carries mutation** — a call that received no mutable value cannot
return an alias of one, so it must have built a fresh one (`array_make`,
`Mutable.ref_make`, `hashmap.empty`, `from_pairs`). This closes the aliasing
hole (`identity_helper(param_ref)` takes a mutable arg → not a constructor →
non-local) without points-to analysis.

## The double-handler LIFO distinguisher (asu-mandated)

A user `handle { } with Mutable { }` pushes ABOVE the unconditional base node,
so LIFO dispatch resolves a `Mutable` op inside the handle body to the USER
clause. The distinguisher fixture
(`examples/effects/mutable_user_handler_over_default.kai`) proves it: the user
`array_get` clause returns a sentinel (999) instead of the real element, so the
assert `n == 999` holds ONLY if the user handler ran. Output 7 (the real value)
would mean the base default shadowed the user handler — it does not.

Note: this fixture is `main : Int` with `assert`, NOT `main : Unit / Console`.
A pre-existing typer interaction rejects a `with Mutable { array_set(...) ->
resume(array_set(...)) }` clause when `main` also carries `Console` — the
clause's `array_set` raises `Mutable` and the Console-bearing main's row
inference disagrees. This is orthogonal to #531 (the precedent
`m7b_2b_mutable_intercept.kai` already uses `main : Int` for the same reason);
left as-is.

## Fixtures added

- `examples/effects/local_ref_masks_mutable.kai` (+`.out.expected` = `3`) —
  #531's exact acceptance program: a local `Ref` masked, `main : Unit / Console`.
- `examples/effects/local_hashmap_masks_mutable.kai` (+`.out.expected` = `2`) —
  a local `HashMap` masked (the former `local_hashmap_no_row` negative, flipped).
- `examples/effects/mutable_user_handler_over_default.kai` — the LIFO
  distinguisher (assert-based, exit 0).
- Removed `examples/negative/mutable/local_hashmap_no_row.{kai,err.expected,flags}`
  (its premise — "a local HashMap can never mask" — is exactly what #531
  invalidates).
- All other `negative/mutable/*` fixtures (param Array/Ref/HashMap writes,
  field writes, escaping mutation) stay negative and PASS — the contract for
  observable mutation is unchanged.

## Coverage gaps / follow-ups

- **Native parity verified in CI, not locally.** The local LLVM-18 static build
  is absent on this machine (`llvm-config --includedir` empty; brew ships
  LLVM 22, whose C API differs from CI's 18.1.3). The native change is the
  minimal symmetric mirror of the C change (`kir_default_handlers` applies the
  same `ensure_unconditional_defaults` before the same filter; the install/
  teardown consumers are untouched), so it is correct by construction — but the
  runtime parity gate runs on CI (tier1-native + tier1-backend-parity).
- **Console + user-Mutable-clause typer interaction** (above) is a separate,
  pre-existing rough edge, not introduced here.
- **Generalising the unconditional install to other base-installable effects**
  is additive (extend `unconditional_default_effects`) and intentionally
  deferred — no second effect qualifies under (a)+(b) today.

## Cost vs estimate

The mechanism was small (one shared helper + symmetric one-line call sites in
both backends). The bulk of the work was the masking thinning in `infer.kai`
(~560 lines touched, mostly deletions and a `dmut`-threading parameter added
across the `collect_local_*` walk) and the two non-obvious discoveries the
brief did not predict: the op-call callee `.ty` carries the result type not the
row (so the dispatch-key prefix check is required), and the `Subst` Array-record
selfhost break (so the record closure must stay Ref-gated). Both were caught by
the selfhost + negative-fixture gates, not by the positive fixtures alone —
which is the lesson: byte-id selfhost is the real gate here, the ergonomics
fixtures are necessary but not sufficient.
