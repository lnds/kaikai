# Design: unified algebraic types — `|` always means union

Status: **proposed** (2026-05-03). Tracked in #187 (supersedes #184).
Implementation deferred to a dedicated milestone (see *Decision posture*).

This document specifies a redesign of kaikai's algebraic type system.
The change is structural: **eliminate sum types as a separate concept**
and unify everything under "union of types". The `|` operator in a
`type` declaration always means "union of the named types"; component
types that don't yet exist are declared implicitly as nominal unit
types.

This document supersedes the prior `unions-design.md` (additive-keyword
approach, tracked in closed #184). The earlier design added `union` as
a new keyword alongside existing sum types. The unified model is
simpler: one form for "type whose values are one of several shapes",
no new keyword, no footgun.

## Motivation

### The DDD / ledger composition problem

Domain-driven design produces bounded contexts. Each context defines
its own error type, ignorant of the others:

```kai
type IdentityError = AccountNotFound | KycExpired | Frozen
type AuthError     = InsufficientBalance | OverDailyLimit | RegulatoryHold
type RoutingError  = NoRouteFound | CorrespondentDown | CutoffPassed
type SettlementError = InvariantViolation | ReconciliationFailed
type AuditError    = WriteRejected | StreamFull | ChainBroken
```

Application-layer operations cross several contexts. Each operation
needs a return type that names exactly the errors it can surface:

```kai
fn transfer(req: TransferRequest)
  : Result[???, Receipt]    # crosses Identity + Auth + Routing + Settlement + Audit

fn query_balance(account_id: AccountId)
  : Result[???, Balance]    # crosses Identity + Auth
```

Today kaikai requires a **wrapper sum type** per operation:

```kai
type TransferError =
  | TIdentity(IdentityError)
  | TAuth(AuthError)
  | TRouting(RoutingError)
  | TSettlement(SettlementError)
  | TAudit(AuditError)

type QueryBalanceError =
  | QBIdentity(IdentityError)
  | QBAuth(AuthError)
```

Each wrapper is a fresh sum type that re-prefixes the same error
(`TIdentity` vs `QBIdentity` for the same `IdentityError`). This is
**DRY violation at the type level**, scaling `O(N²)` in subsystems.

### The "god error" antipattern

Teams hitting the wrapper explosion collapse to a single shared
`AppError`:

```kai
type AppError =
  | Identity(IdentityError)
  | Auth(AuthError)
  | Routing(RoutingError)
  | Settlement(SettlementError)
  | Audit(AuditError)

fn transfer()      : Result[AppError, Receipt]
fn query_balance() : Result[AppError, Balance]    # lies: cannot fail with Routing
```

The error type **lies**: it claims `query_balance` may fail with
`RoutingError`, but the implementation never produces that variant.
The compiler cannot enforce the truthful subset.

### Why this is a type-system problem, not a sugar problem

The wrapper explosion is a symptom of treating "sum-type variants" as
**labels** of a single closed type rather than as **types**. If
`IdentityError` and `AuthError` were composable as a union without
wrapping, `query_balance() : Result[IdentityError | AuthError, Balance]`
would just work.

The earlier design (#184) proposed adding `union` as a separate
keyword. This design goes further: variants **are** types from the
start. `|` always composes types. There is no separate sum-type concept
to compose around.

## Proposal

A `type` declaration with `|` always declares a union of types:

```kai
type T = X1 | X2 | ... | Xn
```

Where each `Xi` is a type. Resolution rule:

1. If `Xi` is **already declared** as a type (record, union, primitive),
   reference it.
2. If `Xi` is **not declared**, the declaration **implicitly declares**
   `Xi` as a nominal unit-like type (no fields).
3. If `Xi` has the form `Name(T1, T2, ...)`, the declaration
   **implicitly declares** `Name` as a nominal record type with
   positional fields `0: T1, 1: T2, ...`.

`T` is then the union of the resolved component types.

### Surface examples

#### Variant types auto-declared

```kai
type Color = Red | Green | Blue
# Declares 4 types:
#   Red   : nominal unit type
#   Green : nominal unit type
#   Blue  : nominal unit type
#   Color : union Red | Green | Blue
```

The syntax is identical to today's sum-type declaration. The semantic
shift: `Red`, `Green`, `Blue` are now real types, not labels of `Color`.

#### Components with payloads

```kai
type Result[E, T] = Ok(T) | Err(E)
# Declares:
#   Ok[T]     : nominal record { 0: T }
#   Err[E]    : nominal record { 0: E }
#   Result[E, T] : union Ok[T] | Err[E]
```

`Ok(T)` is the **declaration of a nominal type Ok with positional field
T**, not a constructor of Result. The union composes Ok and Err.

#### Composing existing types

```kai
type IdentityError = AccountNotFound | KycExpired | Frozen
type AuthError     = InsufficientBalance | OverDailyLimit
type QueryBalanceErr = IdentityError | AuthError
# QueryBalanceErr is the union of two existing union types.
# Same syntax, same rule. Auto-flattens to:
#   QueryBalanceErr = AccountNotFound | KycExpired | Frozen
#                   | InsufficientBalance | OverDailyLimit
```

The function signature now declares **exactly** which errors can
surface:

```kai
fn query_balance() : Result[QueryBalanceErr, Balance]
fn transfer() : Result[IdentityError | AuthError | RoutingError | SettlementError, Receipt]
```

No wrappers. No DRY violation. The compiler enforces the subset.

## Design decisions

### D1 — Payload pattern unwrap

Pattern `Ok(x)` against a value of type `Ok[T]` binds `x : T`, not
`x : Ok { 0: T }`. The pattern is sugar for `match _ : Ok { okv -> let
x = okv.0 }`. This preserves today's ergonomics — existing code reads
identically.

### D2 — Name collision across declarations

```kai
type A = Foo | Bar
type B = Foo | Baz   # ERROR: Foo already declared in A
```

**Explicit error.** The user must either:

- Rename the second `Foo` (e.g., `BFoo`), or
- Pre-declare `Foo` once and intentionally compose:

```kai
type Foo = unit   # or with payload
type A = Foo | Bar
type B = Foo | Baz   # OK — same Foo, intentionally shared
```

Zero magic, zero surprise. Name collisions are loud, not silent.

### D3 — Records vs unions

Orthogonal. Records (`type Point = { x: Int, y: Int }`) stay as the
product form. Unions (this proposal) are the sum form. The two
algebraic-type forms are independent and compose freely.

```kai
type Point = { x: Int, y: Int }
type Shape = Circle(Point, Int) | Rectangle(Point, Point)
# Circle and Rectangle are nominal records with positional fields.
# Shape is the union.
```

### D4 — `#derive` on auto-declared variants

`#derive(Show)` on a union walks each component. Auto-declared unit
types get `#derive(Show)` automatically (the impl is mechanical: print
the type name). Auto-declared record types get `#derive(Show)` by the
same rule that already applies to records.

```kai
#derive(Show, Eq)
type Color = Red | Green | Blue
# Auto-derives Show and Eq for Red, Green, Blue, and Color.
```

### D5 — Pattern matching syntax

Today's pattern syntax is preserved. `match c { Red -> ... }` matches
when `c : Color` is a `Red`. The resolver sees `Red` as a type-scrutinee
(matches values of type `Red`), not as a label.

Type-narrowing patterns (already in #184's Decision 4) work
identically:

```kai
match err : QueryBalanceErr {
  ie : IdentityError -> handle_identity(ie)
  ae : AuthError     -> handle_auth(ae)
}
```

The `bind : Type` form binds the value as the named component type and
is exhaustive over the union if every component is covered.

### D6 — Algebraic properties

Unions are associative, commutative, idempotent:

- `(A | B) | C ≡ A | B | C` (flatten nested)
- `A | B ≡ B | A` (order doesn't matter)
- `A | A ≡ A` (duplicates collapse)

The typer normalizes to a canonical form (sorted, deduplicated,
flattened) for equality checking.

### D7 — Implicit upcast (component to union)

A value of a component type can be used wherever the union type is
expected. No explicit ceremony.

```kai
let id_err : IdentityError = AccountNotFound
let qb_err : QueryBalanceErr = id_err   # implicit: IdentityError is a component
```

This is **bounded subtyping** — `T <: U` only when `U` is a union and
`T` is a component of `U` (after normalization). No structural
subtyping, no covariance/contravariance, no chains.

In the unified model this is not a separate rule: `id_err : IdentityError`
already has type `IdentityError`, and `IdentityError` is one of the
component types of `QueryBalanceErr`. The typer accepts the assignment
because component-to-union is a valid type relationship by
construction.

## What unions are NOT

| Capability | Common in other languages | kaikai unions |
|---|---|---|
| Structural subtyping | TypeScript object subtyping | **not supported** |
| Width / depth subtyping on records | TypeScript object subtyping | **not supported** |
| Covariance / contravariance | Scala variance annotations | **not supported** |
| Polymorphic variants | OCaml `[`A \| `B]` row types | **not supported** |
| Implicit downcast (union → component) | TypeScript narrowing | **not supported** without explicit pattern |
| Anonymous sum types | OCaml `A + B` inline | **not supported** — must be named with `type` |

What kaikai unions **do** support is **bounded nominal subtyping
limited to "component appears in the union"**. Nothing more.

## Interaction with existing features

### Pattern matching exhaustiveness

A `match` over a union must cover every component (or use a
type-narrowing pattern that covers it). The exhaustiveness checker
walks components and, for each component, walks its variants
recursively until it bottoms out at unit-like types.

Counterexample diagnostic identifies the component-of-origin:

```
foo.kai:42:3: non-exhaustive match
  missing: InsufficientBalance (component of AuthError, in QueryBalanceErr)
```

### `Result[E, T]` + `!` propagation

```kai
fn outer() : Result[QueryBalanceErr, Balance] = {
  let v = check_identity(req)?         # returns Result[IdentityError, _]
  #       ^^^^^^^^^^^^^^^^^^^^
  #       IdentityError component of QueryBalanceErr → ! propagates
  let b = check_auth(v)?               # returns Result[AuthError, _]
  Ok(b)
}
```

`!` works transparently because the implicit upcast (D7) applies to
the `Err` payload during propagation. This subsumes any auto-`From`
machinery that #184 deferred — the upcast is structural at the union
level.

### Protocols (m12.8)

`impl P for U` where `U` is a union is **not allowed** in v1. Protocols
dispatch by the head type tag; a union does not have a single head
type. The user implements `P` for each component and the call site
narrows to the component before dispatch.

This is identical to the rule from #184; the unified model doesn't
change it.

### Effects

Unions are pure types. No effect-row component. No interaction with
the effect system beyond being usable as the return type of an
effectful function.

### `#derive`

Walks components, verifies each has `impl P` reachable, generates a
synthetic impl that pattern-matches. Same as #184's rule. Auto-declared
components get auto-derived impls when the kind is mechanical (Show,
Eq, Hash, Ord on units; Show, Eq, Hash, Ord on records by field).

### UoM and refinements (m12.5, m12.6)

Orthogonal. Unions compose at the type level; UoM and refinements
operate within a type. No interaction.

## Migration path for existing code

**Sintáctically zero migration.** Every existing `type T = A | B`
declaration parses identically. Only the internal semantics change:
each "variant" is now a real type instead of a label.

**Semantic risk**: code that depended on "the constructor `Foo` is not
a type" can fail. Examples:

- A function `fn use(x: Foo)` today fails to compile because `Foo` is
  not a type. After this change it compiles. This is a **breaking
  change in the wrong direction** — code that was rejected may now
  silently compile. The Phase 0 audit identifies these cases.
- Pattern matching on `Foo` today is a label match. After this change
  it is a type match. For unit-like variants the behavior is
  observationally identical, but the typer's representation changes.

The Phase 0 audit (below) measures the actual blast radius before
implementation begins.

## Risks

### 1. Typer normalization complexity

Same as #184. Edge cases (empty union, singleton union, nested unions
with shared components) need explicit handling. Unions of generic
types (`F[T] | G[T]`) are out of scope for v1; monomorphic only.

### 2. Diagnostic quality on union mismatches

```
foo.kai:42:15: type mismatch
  expected: QueryBalanceErr (= AccountNotFound | KycExpired | Frozen
                               | InsufficientBalance | OverDailyLimit)
  found:    NoRouteFound (component of RoutingError)
  hint: extend QueryBalanceErr to include RoutingError, or convert
        explicitly
```

Budget specific work for diagnostics. Bad error messages on type
mismatches kill the value proposition.

### 3. Selfhost regression

stdlib loads in the bootstrap chain. The typer changes affect stages 1
and 2. The Phase 0 audit identifies stdlib sum types that need
verification. Continuous `make tier0` validation throughout the lane.

### 4. Semantic-breaking change

Code that depended on "the variant `Foo` is not a type" can now
compile when it shouldn't. The Phase 0 audit measures this. If the
count is high, the lane needs an opt-in flag (`--legacy-sum-types`)
during transition.

### 5. Implicit upcast breaks "no implicit coercion"

The implicit upcast (D7) is the one place where the "explicit
conversions only" rule yields. Justification: the DDD use case requires
it. Mitigation: bounded (only component-to-union, no chains, no
structural matching).

### 6. Interaction with future features

- **`protocol P[A]` (#180)**: unions could be `A`. Out of scope for
  v1; revisit when #180 lands.
- **Polymorphic-impl runtime panic (#174)**: unions might exacerbate.
  Union-protocol impls forbidden in v1.

## Implementation phases

The implementation lane has six phases. **Each phase is its own PR.**
Phase 0 informs the rest.

### Phase 0 — Audit (no code)

- Count sum types in `stdlib/`, `examples/`, `demos/`. Group by
  shape (unit-only variants, payload variants, mixed).
- Identify name-collision candidates (variant names that appear in
  multiple sum-type declarations across the codebase).
- Identify code that depends on "variant is not a type" (e.g.,
  `fn(x: Foo)` where `Foo` is a variant — should fail today).
- Measure typer surface area: which functions in
  `stage2/compiler.kai` would change.
- Output: a doc `docs/unions-phase0-audit.md` with counts and risk
  assessment. ~1-2 days.

### Phase 1 — Typer foundation

- Introduce `TyUnion(components: [Ty])` AST representation.
- Lower current `TySum` to `TyUnion` internally. Existing code
  re-types identically (each variant becomes its own auto-declared
  type, the sum becomes the union).
- Selfhost MUST stay byte-identical throughout — this is the
  acceptance gate for the phase.
- Normalization function (associative flatten, commutative sort,
  idempotent dedup).
- ~250 lines.

### Phase 2 — Resolver

- `type T = A | B` declares A, B as nominal types (unit or record
  per syntax) if not already declared.
- D2 collision check: error explicitly when a name is re-declared
  across `type` declarations.
- Validate that components of a union resolve to types (not free
  variables).
- ~120 lines.

### Phase 3 — Pattern matching

- Match-by-type instead of match-by-label. Resolver reinterprets
  `Red` in `match c { Red -> ... }` as a type-scrutinee.
- Exhaustiveness checker walks components recursively.
- New pattern shape `bind : ComponentType` for type narrowing.
- ~180 lines.

### Phase 4 — Codegen

- Lower `TyUnion` to the same runtime representation as today's sum
  types (tagged union with discriminator). No perf change.
- Implicit upcasts compile to the same constructor invocations as
  today.
- Pattern matching unchanged at the C/LLVM level.
- ~150 lines.

### Phase 5 — Documentation + examples + DDD demo

- Update `docs/design.md`, `docs/protocols.md` (interaction note),
  `docs/errors-conventions.md` (Pattern C — unions as the canonical
  approach, deprecating Pattern A wrappers).
- Write `docs/unions.md` (user-facing reference).
- Demo in `examples/unions/`: DDD/ledger composition pattern.
- ~400 lines (mostly documentation).

### Phases NOT in v1

- **stdlib protocol impls migrated to unions**: deferred. Existing
  impls work; user code can adopt.
- **Generic unions** (`F[T] | G[T]`): out of scope.
- **Union with HKT** (when #180 lands): re-evaluate.
- **`impl P for U` where U is union**: forbidden in v1.

## Estimated cost

| Phase | Lines | Effort |
|---|---|---|
| 0. Audit | ~0 (doc) | 1-2 days |
| 1. Typer foundation | ~250 | 3-5 days |
| 2. Resolver | ~120 | 1-2 days |
| 3. Pattern matching | ~180 | 2-3 days |
| 4. Codegen | ~150 | 2-3 days |
| 5. Documentation + DDD demo | ~400 | 2-3 days |
| **Total** | **~1100 lines** | **~2 weeks** |

Comparable to #184's estimate, with risk concentrated in Phase 0 and
Phase 1 (where selfhost validation is the gate).

## Decision posture

This is a **milestone-level lane**, not a single PR. Pre-implementation
review of this design doc is required by the integrator. Phase 0
opens the lane; Phase 5 closes the milestone.

## Cross-references

- **Issue #187**: tracks this milestone. Supersedes #184.
- **Issue #184**: closed (not planned). The earlier additive-keyword
  design.
- **Issue #182**: `Result` / `Option` API expansion. Independent.
- **Issue #180**: `protocol P[A]` parametrized. Future generic-union
  interaction is out of scope for v1.
- **Issue #174**: polymorphic-impl runtime panic. Affects union-protocol
  interaction (forbidden in v1, may revisit).
- **`docs/errors-conventions.md`**: Pattern A (wrappers + map_err)
  becomes deprecated post-landing. Pattern C (unions) is the
  canonical replacement.
