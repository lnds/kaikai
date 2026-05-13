# Lane experience — issue #535: protocol impl validation

## Scope as planned vs as shipped

**Planned (brief):** three impl-validation shapes the dispatch resolver
accepted silently:

1. Missing required method — `impl P for Box { fn foo }` when `P`
   declares `foo` and `bar`.
2. Method signature mismatch — `impl P for Box { fn foo(b) : String }`
   when `P` declares `foo(x: Self) : Int`.
3. Method arity mismatch — `fn foo(b: Box) : Int` against
   `foo(x: Self, n: Int) : Int`.

**Shipped:** all three shapes, plus one extra check the brief did not
anticipate — *unknown method in impl* (an `impl P for T { fn frobnicate
... }` whose name does not appear in the protocol's op list). The
existing fixtures + dispatcher already covered the bug, but the
implementation surface naturally produced the diagnostic, so it stays
in.

One semantic refinement to the return-type check, forced by the
stdlib audit (see "Structural surprises" below): heterogeneous
parametrised impls (`impl Add[Complex] for Real`) skip the return-
type comparison. The param-position checks still run, so genuine
drift on `self` / `rhs` is still caught.

## Design decisions + alternatives considered

### Where the check runs

- **Chosen:** new `validate_impl_methods` pass after
  `collect_proto_decls` in `lower_protocols`, before
  `lower_impls`. The protocol registry is already built; impl decls
  still carry their original method list. One traversal per impl;
  one pass total.
- **Alternative considered:** fold the check into the existing
  `validate_impls_loop` (orphan + duplicate-impl). Rejected because
  that loop runs *before* `collect_proto_decls`, so it would need to
  re-walk the decl stream to find the protocol's op list. Two passes
  for one piece of state is the kind of redundancy that drift survives
  on; a single new pass is cleaner.
- **Alternative considered:** validate inside the typer, once impl
  methods have semantic types. Rejected because the bug class
  (missing-method, arity) is purely structural and does not need
  inference. Doing it pre-typer means the error fires before any
  cascading typer noise.

### How signatures get compared

The protocol declares `Self` and tparams (`a` in `protocol Add[a]`);
the impl uses concrete heads. Comparison runs at the *head-name*
level after substitution:

- `Self` → impl target head (`Box`, `Complex`, `Real<u>` → `Real`).
- Protocol tparams → impl bracket args (`impl Add[Real] for Complex`
  → `a → Real`).
- Legacy un-parametrised impl (`impl Add for Int`) → every protocol
  tparam falls back to the target head (homogeneous default, matching
  what `lower_impls` already does for the mangling key).

Head-name only (via `proto_type_name`) — *not* structural unification.
Two reasons:

1. The pre-typer pass has no unifier; building one here would
   duplicate logic that already lives behind `unify` / `ty_eq`.
2. The head-name collapse already matches what dispatch uses for
   mangling (`Real<u>` and `Real` both dispatch to `__pimpl_*_Real_*`),
   so any deeper check would invent precision the dispatch layer does
   not have.

Deeper structural comparison is a follow-up — the typer's `ty_eq`
could be re-run on resolved impl/proto signatures once both are
lowered to `Ty`, catching cases the head-name collapse misses (e.g.
`Array[Byte]` vs `Array[Int]`). Not in scope for #535; opened as a
follow-up issue at lane close.

### Return-type check is conditional on homogeneity

Initial implementation flagged every return-type mismatch. Selfhost
turned on the stdlib's heterogeneous arithmetic impls:

```kai
impl Add[Complex] for Real {
  fn add(self: Real, rhs: Complex) : Complex = ...
}
```

`protocol Add[a]` declares `add(self: Self, rhs: a) : Self`, so the
expected return is `Real`, but the impl correctly returns `Complex`
(promotion semantics — `Real + Complex` *should* be `Complex`). The
protocol's `Self` return cannot express the promotion because `Add`
does not parametrise the return type.

Three exits were on the table:

1. Re-shape `protocol Add[a]` to add a return tparam (`protocol
   Add[a, r] { add(self: Self, rhs: a) : r }`). Touches every
   numeric impl in the stdlib and downstream user code.
2. Drop the heterogeneous impls. Loses `Real + Complex` and friends.
3. Skip the return-type check for heterogeneous impls and document
   the gap.

Chose (3) — keeps the validation honest about what it can verify and
defers the protocol-shape redesign to a separate lane. The param-
position checks still run, which is where the headline bug class
(`Self` substituted incorrectly) actually lives.

## Structural surprises

- **`length` vs `list_length` in compiler.kai.** Stage 2 carries
  `list_length` everywhere (5+ call sites); `length` is exported from
  `stdlib/core/list.kai` for user code. The first implementation pass
  used `length` and selfhost rejected. Trivial fix, but a hint that
  the compiler still has a private fork of list helpers.
- **`proto_op_names` shrinks per recursion.** The first cut of
  `check_missing_methods` computed the "protocol declares ops X, Y, Z"
  list inside the loop, after recursion had already stripped the head
  op. Result: only the missing op was named, not the full set.
  Hoisting the all-names computation up to the caller before the
  recursion fixed it.
- **Heterogeneous Add return type.** Already covered above — surfaced
  by tier1 (`modules-derive-export`), not by the headline test set or
  selfhost.

## Fixtures added + coverage

Promoted from `examples/negative/silent_contract/` to
`examples/negative/protocols/`:

- `impl_missing_required_method.kai` + `.err.expected`
- `impl_method_signature_mismatch.kai` + `.err.expected`
- `impl_method_arity_mismatch.kai` + `.err.expected`

`tools/test-negative.sh` count: 87 → 90 PASS (three new). README in
`silent_contract/` updated to strike the row.

Coverage gaps:

- No fixture for the *unknown method in impl* shape (the extra check
  that fell out of the implementation). Cheap follow-up.
- No fixture for heterogeneous-impl return mismatch — that path is
  deliberately skipped, so a fixture would document the gap rather
  than enforce it. Will live under `silent_contract/` if filed.
- No fixture exercising the `proto_tparams_for` lookup with a multi-
  tparam protocol (`protocol P[a, b]`). The closest current usage is
  the single-tparam `Add[a]` family.

## Real cost vs estimate

- Brief estimate: 3-4 days (Linus).
- Actual: single session (~2-3h, autonomy run).

The fast path: the registry + lowering infrastructure was already
load-bearing for orphan / duplicate-impl, so the new pass dropped in
with one call-site edit in `lower_protocols`. The two stalls were
both auditable surprises: `length` vs `list_length` (selfhost), and
heterogeneous Add return (tier1).

## Hallazgo colateral — dispatcher dedup is opname-only

`stage2/compiler.kai:49241` — `dispatcher_in_acc` dedupes the
generated dispatcher stubs by opname alone, not by `(pname, opname)`.
Two protocols declaring an op with the same name (e.g. `protocol A {
foo(x: Self) : Int }` and `protocol B { foo(x: Self) : String }`)
would produce one dispatcher, and only the first protocol's
signature would survive into the typer. Today no two stdlib
protocols collide on op name, so this is latent — but it is a fix
that belongs to the dispatch resolver, not to impl validation. Filed
as a separate issue at lane close.

## Follow-ups left for next lanes

- Deeper structural signature check via `ty_eq` once impl + proto
  signatures are resolved (the head-name shortcut here is the v1
  floor, not the ceiling).
- Protocol-shape redesign for `Add[a]` / `Sub[a]` / `Mul[a]` / `Div[a]`
  to express return-type promotion (separate tparam, or HKT-light
  shape). Needed before the return-type check can re-enable for
  heterogeneous impls.
- `dispatcher_in_acc` dedup bug (linked above).
- Fixture for *unknown method in impl* under
  `examples/negative/protocols/`.
