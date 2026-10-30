# Union types

This is the user-facing reference. For the design rationale and
the decision log, see `docs/unions-design.md`.

## What unions are

A **union type** names exactly the set of nominal types whose
values it can carry. The motivating use case is composing errors
across bounded contexts without paying the wrapper-sum-type tax:

```kaikai
type IdentityError   = AccountNotFound | KycExpired | Frozen
type AuthError       = InsufficientBalance | OverDailyLimit
type QueryBalanceErr = IdentityError | AuthError

fn check_identity(id: Int) : Result[Int, IdentityError] =
  if id > 0 { Ok(id) } else { Err(AccountNotFound) }

fn check_auth(acc: Int) : Result[Int, AuthError] =
  if acc < 100 { Ok(acc * 10) } else { Err(OverDailyLimit) }

fn query_balance(id: Int) : Result[Int, QueryBalanceErr] = {
  let acc = check_identity(id)!      # IdentityError <: QueryBalanceErr
  let bal = check_auth(acc)!         # AuthError     <: QueryBalanceErr
  Ok(bal)
}

fn main() : Unit / Stdout = match query_balance(7) {
  Ok(b)  -> Stdout.print("balance #{b}")
  Err(_) -> Stdout.print("denied")
}
```

No wrapper variants (`QBIdentity(IdentityError)`), no
`map_err(QBIdentity)` ceremony, no `From` impls. The signature
declares exactly the errors `query_balance` can surface, and `!`
propagates each subsystem's error through the implicit upcast.

## Syntax

```kaikai-fragment
type T = A | B | C
```

`|` always means union of types. **The rules below apply to the
multi-component form** (two or more components separated by `|`).
The single-component form `type Foo = T` (no `|`) is a *transparent
type alias*: `Foo` and `T` are interchangeable in every type
position. A one-member sum takes a leading bar: `type Foo = | MkFoo`.

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

```kaikai
type Color = Red | Green | Blue
# Declares 4 types: Red, Green, Blue (auto-declared unit-like) and
# Color = Red | Green | Blue.

type Tree[a] = Leaf | Node(a, Tree[a], Tree[a])
# Declares Leaf (auto-declared unit) and Node[A] (auto-declared
# record { 0: A, 1: Tree[A], 2: Tree[A] }) and Tree.

type Meters = Int    # single component: a transparent alias

fn size(t: Tree[Int]) : Int = match t {
  Leaf          -> 0
  Node(_, l, r) -> 1 + size(l) + size(r)
}

fn main() : Unit / Stdout = {
  let m : Meters = 3
  let n : Int = m
  Stdout.print("#{size(Node(1, Leaf, Node(2, Leaf, Leaf)))} #{n}")
}
```

Every sum-type declaration `type T = A | B | ...` is a union under
this model; no separate sum-type form exists.

## Implicit upcast (D3)

A value of a component type can flow wherever the union is
expected, without explicit conversion — at a `let` annotation or at
a function-call site:

```kaikai
type IdentityError   = AccountNotFound | KycExpired | Frozen
type AuthError       = InsufficientBalance | OverDailyLimit
type QueryBalanceErr = IdentityError | AuthError

fn classify(e: QueryBalanceErr) : String = match e {
  AccountNotFound -> "account missing"
  _               -> "other"
}

fn main() : Unit / Stdout = {
  let id_err : IdentityError = AccountNotFound
  let qb_err : QueryBalanceErr = id_err      # IdentityError <: QueryBalanceErr
  Stdout.print(classify(qb_err))
  Stdout.print(classify(OverDailyLimit))     # AuthError <: QueryBalanceErr
}
```

The upcast is **bounded**: only one step. `T <: U` holds when `U`
is a union and `T` is a direct component of `U`. It does **not**
chain across `T <: U <: V`:

```kaikai-neg
type IdentityError = AccountNotFound | KycExpired
type AuthError     = InsufficientBalance | OverDailyLimit
type QueryErr      = IdentityError | AuthError
type RoutingError  = | NoRouteFound
type AppErr        = QueryErr | RoutingError

fn handle_app(e: AppErr) : Int = 0

fn main() : Int = {
  let id : IdentityError = AccountNotFound
  handle_app(id)       # ERROR: IdentityError is not a component of AppErr.
}
```

The diagnostic names the union and says to lift through `QueryErr`
first: bind `let q : QueryErr = id` and pass `q`.

This is intentional. Chained subtyping makes inference brittle and
diagnostics hard to phrase; the user writes the intermediate
binding when composing across more than one layer.

### Propagation into `match` arms and `if/else` branches

The upcast also fires inside `match` arms — and, by the same rule,
inside `if/else` branches — when the surrounding expression's
result type is constrained externally. Both the **annotated return
type** of the enclosing function and the **annotation on a `let`**
whose RHS is a `match` or `if/else` are propagation sites: the
expected union shape is in scope when each arm or branch body
unifies, so a narrower component-typed result flows into the wider
declared type without a `let` lift:

```kaikai
type ErrorA  = | NoDefinida(String)
type ErrorB  = | DivCero
type ErrorAB = ErrorA | ErrorB

# The first arm is `Result[_, ErrorA]`; the declared return type
# reaches every arm, so it upcasts to `Result[Real, ErrorAB]`.
fn lookup(xs: [(String, Real)], k: String) : Result[Real, ErrorAB] =
  match xs {
    []                -> Err(NoDefinida(k))
    [(n, v), ...rest] -> if n == k { Ok(v) } else { lookup(rest, k) }
  }

# Same for `if/else`: the `then` branch does not pin the result type.
fn countdown(i: Int) : Result[Real, ErrorAB] =
  if i < 0 {
    Err(NoDefinida("neg"))
  } else {
    countdown(i - 1)
  }

fn main() : Unit / Stdout = {
  match lookup([("a", 1.5)], "a") {
    Ok(v)  -> Stdout.print("#{v}")
    Err(_) -> Stdout.print("missing")
  }
  match countdown(2) {
    Ok(_)  -> Stdout.print("ok")
    Err(_) -> Stdout.print("err")
  }
}
```

Synthesise mode (`match` or `if/else` in unannotated position) is
unchanged: the typer still infers the result bottom-up, and arms /
branches with genuinely incompatible synthesised types are still
rejected. The bidirectional check is purely additive — it only
fires when an expected type is in scope. Argument position
(`f(match { ... })` or `f(if cond { ... } else { ... })` where the
parameter type is known) is not a propagation site: bind the
expression with an annotated `let` first.

## Pattern matching

Two pattern shapes work over union scrutinees, and they
**interoperate** — a single `match` may mix them.

### Variant patterns

```kaikai
type IdentityError   = AccountNotFound | KycExpired | Frozen
type AuthError       = InsufficientBalance | OverDailyLimit
type QueryBalanceErr = IdentityError | AuthError

fn code(err: QueryBalanceErr) : String = match err {
  AccountNotFound      -> "id:not-found"
  KycExpired           -> "id:kyc"
  Frozen               -> "id:frozen"
  InsufficientBalance  -> "auth:funds"
  OverDailyLimit       -> "auth:limit"
}

fn main() : Unit / Stdout = Stdout.print(code(Frozen))
```

Exhaustiveness is checked across all components: every variant of
every component must be covered (or covered by a narrowing arm,
below). Missing variants are reported with their
component-of-origin:

```text
non-exhaustive match: missing KycExpired (component of IdentityError)
```

### Type-narrowing patterns: `bind : Type`

A `bind : ComponentType` pattern matches when the scrutinee's
runtime value belongs to the named component, and binds it under
the component type for the body. This is the canonical idiom for
*layered* error handling — delegate each component to its own
handler. Mixing narrowing and variant arms is fine:

```kaikai
type IdentityError   = AccountNotFound | KycExpired | Frozen
type AuthError       = InsufficientBalance | OverDailyLimit
type QueryBalanceErr = IdentityError | AuthError

fn handle_identity(e: IdentityError) : String = match e {
  AccountNotFound -> "id:not-found"
  KycExpired      -> "id:kyc"
  Frozen          -> "id:frozen"
}

fn handle_auth(e: AuthError) : String = match e {
  InsufficientBalance -> "auth:funds"
  OverDailyLimit      -> "auth:limit"
}

fn classify(err: QueryBalanceErr) : String = match err {
  AccountNotFound    -> "specifically: account missing"
  ie : IdentityError -> handle_identity(ie)   # covers KycExpired, Frozen
  ae : AuthError     -> handle_auth(ae)
}

fn main() : Unit / Stdout = {
  Stdout.print(classify(AccountNotFound))
  Stdout.print(classify(Frozen))
  Stdout.print(classify(OverDailyLimit))
}
```

Narrowing arms count toward exhaustiveness as if they covered every
variant of the named component.

## `!` propagation across union boundaries

`!` works transparently across the implicit upcast. A function
returning `Result[T, U]` may freely propagate inner results whose
error type is a component of `U` — `query_balance` in the first
example above propagates both an `IdentityError` and an `AuthError`
into `QueryBalanceErr` without `map_err`.

## D2 — name-collision diagnostic

An implicit-declaration component (rule 2 / 3 above) can only be
introduced once. Two unions both implicitly declaring the same
name is rejected:

```kaikai-neg
type A = Foo | Bar
type B = Foo | Baz
# error: type 'Foo' is declared by both 'type A = ...' and 'type B = ...'
# help:  pre-declare 'type Foo = ...' once and reference it from both unions

fn main() : Int = 0
```

The fix is to lift the shared name into its own declaration:

```kaikai
type Foo = | MkFoo
type A = Foo | Bar
type B = Foo | Baz       # OK — both unions reference the pre-existing Foo.

fn main() : Int = 0
```

This is a *deliberate* explicitness: silent shadowing was the
footgun the unified model set out to remove.

## Limits

- **Components are names.** A component is a bare name, `Name(T1,
  T2)`, or a qualified `pkg.Name`; an applied type such as
  `Option[T]` or `Box[Int]` is not a component, so
  `type F[T] = Option[T] | Result[String, T]` does not parse.
- **Protocols dispatch on the component.** `impl P for U` on a union
  is a compile error: implement `P` for each component. A call on a
  `U`-typed value runs the impl of the component the value carries,
  through a match over the components the compiler emits, so a value
  answers the same through `U` and through its own type. Every
  component needs an impl; the error names the one that lacks it.
  The receiver must be the only `Self`-typed parameter of the
  operation.

## Cross-references

- `docs/unions-design.md` — the design specification (decision
  log D1–D6, what unions are NOT, risk register).
- `docs/errors-conventions.md` — Pattern C documents unions as
  the canonical replacement for the wrapper-sum-type pattern.
- `docs/protocols.md` — the union/protocol interaction rule.
- `examples/unions/` — end-to-end fixtures covering every
  user-visible shape, including `ddd_ledger_demo.kai` (a
  three-bounded-context demo using `!` propagation and narrowing).
