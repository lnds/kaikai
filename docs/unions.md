# Union types

Status: **implemented** (2026-05-03, issue #187, PRs #189–#193).
This is the user-facing reference. For the design rationale and
the decision log, see `docs/unions-design.md`. For the migration
audit that informed the implementation, see
`docs/unions-phase0-audit.md`.

## What unions are

A **union type** names exactly the set of nominal types whose
values it can carry. The motivating use case is composing errors
across bounded contexts without paying the wrapper-sum-type tax:

```kai
type IdentityError = AccountNotFound | KycExpired | Frozen
type AuthError     = InsufficientBalance | OverDailyLimit
type QueryBalanceErr = IdentityError | AuthError

fn check_identity(req: Req) : Result[IdentityError, Account] = ...
fn check_auth(acc: Account) : Result[AuthError, Approved] = ...

fn query_balance(req: Req) : Result[QueryBalanceErr, Balance] = {
  let acc = check_identity(req)!     # IdentityError <: QueryBalanceErr
  let app = check_auth(acc)!         # AuthError     <: QueryBalanceErr
  Ok(load_balance(app))
}
```

No wrapper variants (`QBIdentity(IdentityError)`), no
`map_err(QBIdentity)` ceremony, no `From` impls. The signature
declares exactly the errors `query_balance` can surface, and `!`
propagates each subsystem's error through the implicit upcast.

## Syntax

```kai
type T = A | B | C
```

`|` always means union of types. **The rules below apply to the
multi-component form** (two or more components separated by `|`).
The single-component form `type Foo = T` (no `|`) is a *transparent
type alias*: `Foo` and `T` are interchangeable in every type
position. See `docs/kaikai-minimal.md` lines 194-195 and issue
#376 for the alias contract.

In the multi-component form, the compiler resolves each component
by **three rules**, in order:

1. **Pre-existing type** — if `A` is already declared as a type
   (sum, record, primitive, or another union), the union references
   it.
2. **Undeclared, no payload** — if `A` is not declared and appears
   bare (`A`, no parentheses), it is implicitly declared as a
   nominal **unit-like type** with no fields.
3. **Undeclared, with payload** — if `A` appears as `A(T1, T2)` and
   is not declared, it is implicitly declared as a nominal **record
   with positional fields** of types `T1`, `T2`.

```kai
type Color = Red | Green | Blue
# Declares 4 types: Red, Green, Blue (auto-declared unit-like) and
# Color = Red | Green | Blue.

type Tree[A] = Leaf | Node(A, Tree[A], Tree[A])
# Declares Leaf (auto-declared unit) and Node[A] (auto-declared
# record { 0: A, 1: Tree[A], 2: Tree[A] }) and Tree.
```

Existing kaikai sum-type declarations parse and behave exactly the
same — every `type T = A | B | ...` in stdlib worked unmodified
through all five phases, and selfhost was byte-identical at every
landing.

## Implicit upcast (D3)

A value of a component type can flow wherever the union is
expected, without explicit conversion:

```kai
let id_err : IdentityError = AccountNotFound
let qb_err : QueryBalanceErr = id_err     # IdentityError <: QueryBalanceErr
```

Or at a function-call site:

```kai
fn classify(e: QueryBalanceErr) : String = ...

classify(AccountNotFound)        # AccountNotFound is an IdentityError;
                                 # IdentityError <: QueryBalanceErr.
```

The upcast is **bounded**: only one step. `T <: U` holds when `U`
is a union and `T` is a direct component of `U`. It does **not**
chain across `T <: U <: V`:

```kai
type IdentityError = AccountNotFound | KycExpired
type QueryErr      = IdentityError | AuthError
type AppErr        = QueryErr | RoutingError

let id : IdentityError = AccountNotFound
handle_app(id)       # ERROR: IdentityError is not a component of AppErr.
                     # Go through QueryErr explicitly:
let q : QueryErr = id
handle_app(q)        # OK.
```

This is intentional. Chained subtyping makes inference brittle and
diagnostics hard to phrase; the user writes the intermediate
binding when composing across more than one layer.

### Propagation into `match` arms

The upcast also fires inside `match` arms when the match's result
type is constrained externally — the **annotated return type** of
the enclosing function or the **annotation on a `let`** whose RHS
is the match. The typer drives those positions in check-mode
(`check_match`), so the expected union shape is in scope when each
arm's body unifies. A narrower component-typed arm flows into the
wider declared type without a `let` lift:

```kai
type ErrorA  = NoDefinida(String)
type ErrorB  = DivCero
type ErrorAB = ErrorA | ErrorB

# Before #379: the typer pinned the match's result type from the
# first arm (`Result[ErrorA, _]`), and the recursive call's
# `Result[ErrorAB, Real]` failed to unify.
# After #379: the annotated return type reaches the match arms, so
# `Result[ErrorA, _]` upcasts to `Result[ErrorAB, Real]` per D3.
fn lookup(xs: [(String, Real)], k: String) : Result[ErrorAB, Real] =
  match xs {
    []                  -> Err(NoDefinida(k))
    [(n, v), ...rest]   -> if n == k { Ok(v) } else { lookup(rest, k) }
  }
```

Synthesise mode (`match` in unannotated position) is unchanged: the
typer still infers the arm result bottom-up, and arms with
genuinely incompatible synthesised types are still rejected. The
bidirectional check is purely additive — it only fires when an
expected type is in scope. Argument-position propagation
(`f(match { ... })` where the parameter type is known) is out of
scope for #379 and tracked separately.

## Pattern matching

Two pattern shapes work over union scrutinees, and they
**interoperate** — a single `match` may mix them.

### Variant patterns (the existing form)

```kai
match err : QueryBalanceErr {
  AccountNotFound      -> "id:not-found"
  KycExpired           -> "id:kyc"
  Frozen               -> "id:frozen"
  InsufficientBalance  -> "auth:funds"
  OverDailyLimit       -> "auth:limit"
}
```

Exhaustiveness is checked across all components: every variant of
every component must be covered (or covered by a narrowing arm,
below). Missing variants are reported with their
component-of-origin:

```
non-exhaustive match: missing KycExpired (component of IdentityError)
```

### Type-narrowing patterns: `bind : Type`

A `bind : ComponentType` pattern matches when the scrutinee's
runtime value belongs to the named component, and binds it under
the component type for the body:

```kai
fn classify(err: QueryBalanceErr) : String = match err {
  ie : IdentityError -> handle_identity(ie)   # ie : IdentityError
  ae : AuthError     -> handle_auth(ae)       # ae : AuthError
}
```

This is the canonical idiom for *layered* error handling —
delegate each component to its own handler. Mixing narrowing and
variant arms is fine:

```kai
match err : QueryBalanceErr {
  AccountNotFound  -> "specifically: account missing"
  ie : IdentityError -> handle_identity_default(ie)   # covers KycExpired, Frozen
  ae : AuthError     -> handle_auth(ae)
}
```

Narrowing arms count toward exhaustiveness as if they covered every
variant of the named component.

## `!` propagation across union boundaries

`!` works transparently across the implicit upcast. A function
returning `Result[U, T]` may freely propagate inner results whose
error type is a component of `U`:

```kai
fn run() : Result[QueryBalanceErr, Int] = {
  let v1 = check_id(req)!        # Result[IdentityError, _]
  let v2 = check_auth(v1)!       # Result[AuthError, _]
  Ok(v1 + v2)
}
```

The `!` postfix unifies the inner `Err` type against the enclosing
function's `Err` type using the D3 rule, so `IdentityError` and
`AuthError` both propagate into `QueryBalanceErr` without
`map_err`.

## D2 — name-collision diagnostic

An implicit-declaration component (rule 2 / 3 above) can only be
introduced once. Two unions both implicitly declaring the same
name is rejected:

```kai
type A = Foo | Bar
type B = Foo | Baz
# error: type 'Foo' is declared by both 'type A = ...' and 'type B = ...'
# help:  pre-declare 'type Foo = ...' once and reference it from both unions
```

The fix is to lift the shared name into its own declaration:

```kai
type Foo = MkFoo
type A = Foo | Bar
type B = Foo | Baz       # OK — both unions reference the pre-existing Foo.
```

This is a *deliberate* explicitness: silent shadowing was the
footgun the unified model set out to remove. The Phase 0 audit
verified that no existing fixture across `stdlib/`, `examples/`,
`demos/` triggers D2; every existing declaration parses
identically with no migration.

## Out of scope (v1)

The following compose the unified model but are deferred:

- **Generic unions** — `type F[T] = Option[T] | Result[String, T]`.
  v1 unions compose monomorphic types only.
- **`impl P for U`** when `U` is a union — single-dispatch
  protocols dispatch on a head type tag; a union has no single
  head. Implement `P` for each component and narrow at the call
  site. See `docs/protocols.md` *What this is not*.
- **Stdlib migration to unions** — existing `type T = A | B | ...`
  declarations remain valid (they *are* unions under the unified
  model). Stdlib was deliberately left as-is; unions show up
  organically when composition demands them.
- **Option-Y wrapper layout** — the runtime layout uses Phase 2's
  dual representation (parent-side zero-arg ctors plus inner
  variants). A separate single-tag layout (`__From_C` wrappers)
  was evaluated and shelved as Phase 4's Option Y; the canonical
  user-visible flows work without it.
- **D3-aware diagnostic phrasing** — type-mismatch messages on
  D3-rejected calls today read as the generic `type mismatch in
  function call`. Improving the wording to "X is not a component
  of U; did you mean to add it?" is a follow-up.

## Cross-references

- `docs/unions-design.md` — the design specification (decision
  log D1–D6, what unions are NOT, risk register).
- `docs/unions-phase0-audit.md` — pre-implementation audit that
  validated zero-collision migration.
- `docs/errors-conventions.md` — Pattern C documents unions as
  the canonical replacement for the wrapper-sum-type pattern.
- `docs/protocols.md` *What this is not* — the union/protocol
  interaction rule.
- `examples/unions/` — ten end-to-end fixtures covering every
  user-visible shape, including `ddd_ledger_demo.kai` (a
  three-bounded-context demo using `?` propagation and narrowing).
- Issue #187 — the milestone issue that tracked the redesign.
- PRs #189 (audit), #190 (Phase 1 typer), #191 (Phase 2
  resolver + D2), #192 (Phase 3 narrowing + exhaustiveness),
  #193 (Phase 4 D3 upcast + codegen).
