# Design: union types for kaikai

Status: **implemented** (2026-05-03). Issue #187 superseded the
original "additive `union` keyword" proposal (issue #184) and
implemented a unified model where `|` always means union; see
`docs/unions.md` for the user-facing reference.

> **Historical note**: this document is preserved as the design
> specification of record. The original *Proposal* and
> *Implementation phases* sections describe the additive-keyword
> approach (issue #184). The shipped feature (issue #187, PRs
> #189–#193) replaced that with the unified `|`-always-means-union
> model documented in `docs/unions.md`. Decisions D1–D6 in this
> doc were re-litigated for the unified model and re-recorded in
> issue #187's body. Consult `docs/unions.md` for what shipped;
> consult this doc for the alternatives that were rejected and
> the risk register that informed phasing.

Original document follows.

## Motivation

### The DDD / ledger composition problem

Domain-driven design produces bounded contexts. Each context defines
its own error type, ignorant of the others:

```kai
# Identity bounded context:
type IdentityError = AccountNotFound | KycExpired | Frozen

# Authorization bounded context:
type AuthError = InsufficientBalance | OverDailyLimit | RegulatoryHold

# Routing bounded context:
type RoutingError = NoRouteFound | CorrespondentDown | CutoffPassed

# Settlement bounded context:
type SettlementError = InvariantViolation | ReconciliationFailed | OutOfBalance

# Audit bounded context:
type AuditError = WriteRejected | StreamFull | ChainBroken
```

Application-layer operations cross several bounded contexts. Each
operation needs a return type that names exactly the errors it can
surface:

```kai
fn transfer(req: TransferRequest)
  : Result[???, Receipt]    # crosses Identity + Auth + Routing + Settlement + Audit

fn query_balance(account_id: AccountId)
  : Result[???, Balance]    # crosses Identity + Auth

fn reconcile(period: Period)
  : Result[???, Reconciliation]   # crosses Settlement + Audit

fn refund(tx_id: TxId)
  : Result[???, Refund]     # crosses Identity + Auth + Settlement + Audit
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

type ReconcileError =
  | RSettlement(SettlementError)
  | RAudit(AuditError)

type RefundError =
  | RIdentity(IdentityError)
  | RAuth(AuthError)
  | RSettlement(SettlementError)
  | RAudit(AuditError)
```

Four wrappers, all envelope **subsets** of the same underlying
errors. The wrapper prefix renames the same error in each context
(`TIdentity` vs `QBIdentity` vs `RIdentity` for `IdentityError`).
This is **DRY violation at the type level**, and it scales `O(N²)`
in subsystems.

### The "god error" antipattern that emerges

Teams hitting this pattern collapse to a single shared `AppError`:

```kai
type AppError =
  | Identity(IdentityError)
  | Auth(AuthError)
  | Routing(RoutingError)
  | Settlement(SettlementError)
  | Audit(AuditError)

fn transfer(req)    : Result[AppError, Receipt]
fn query_balance()  : Result[AppError, Balance]    # lies: cannot fail with Routing
fn reconcile()      : Result[AppError, _]          # lies: cannot fail with Identity
```

The error type **lies**: it claims `query_balance` may fail with
`RoutingError`, but the implementation never produces that variant.
The compiler cannot enforce the truthful subset. Callers
defensively pattern-match on every variant.

This is **the failure mode that union types fix**: the function
signature should declare exactly the subset of errors it can
produce, not a superset.

### Empirical confirmation: kaikai today

Verified during the design conversation (2026-05-03) on
`bin/kai 0.37.0`:

- Sum-type variants are **labels of the parent type, not types
  themselves**. `type T = A | B` does not introduce types `A` or
  `B`; the `A` in `fn f(x: A)` is parsed as a free type variable.
- `type Z = T | W` (where `T` and `W` are pre-existing types)
  parses successfully but **declares two new zero-arg constructors
  named `T` and `W`**, completely unrelated to the types so named.
  The compiler does not warn about the name collision.

The latter is a **silent semantic mismatch footgun**: a programmer
writing TypeScript-style `type Z = T | W` expecting union of types
gets a sum type with new constructors that happen to share names
with existing types in scope. The compiler accepts code that
violates programmer intent.

## Proposal

Add a single new top-level declaration form:

```kai
union <Name> = <Type1> | <Type2> | ... | <TypeN>
```

`union` is a **new reserved keyword**, distinct from `type`. The
syntax `T | W` inside a `union` declaration means **union of the
types T and W previously defined**, not new constructors.

### Surface examples

```kai
type IdentityError = AccountNotFound | KycExpired | Frozen
type AuthError     = InsufficientBalance | OverDailyLimit | RegulatoryHold
type RoutingError  = NoRouteFound | CorrespondentDown | CutoffPassed

union QueryBalanceErr = IdentityError | AuthError
union TransferErr     = IdentityError | AuthError | RoutingError | SettlementError | AuditError
union RefundErr       = IdentityError | AuthError | SettlementError | AuditError

fn query_balance() : Result[QueryBalanceErr, Balance]
fn transfer()      : Result[TransferErr, Receipt]
fn refund()        : Result[RefundErr, Refund]
```

The function signatures now declare **exactly** which errors can
surface. No wrappers. No DRY violation. The compiler enforces the
subset truthfully.

## Design decisions

The conversation that produced this document explicitly resolved
six design decisions. Each is recorded here with the chosen path
and the rationale.

### Decision 1 — Surface keyword: `union` distinct from `type`

**Chosen**: `union` is a new top-level keyword.

**Alternative considered**: overloading `type` with disambiguation
rules. Rejected because:

- The footgun (`type Z = T | W`) is exactly the rule "vertical bar
  in `type` declarations means new constructor". Changing that rule
  silently breaks every existing sum-type declaration.
- A new keyword is **explicit**: the user opts into union semantics
  by writing `union`. Sum types stay as they are.

The keyword choice (`union` vs `typedef`, `alias`, `unite`, etc.)
follows precedent: Ceylon, TypeScript, Scala 3, C all use `union`
or related vocabulary for the same concept. `union` is the most
common term and reads correctly.

### Decision 2 — Semantic model: sugar lowering to a generated sum type (Model C)

**Chosen**: Model C in the conversation — surface is union, lowering
is to a compiler-generated sum type. The user sees and reasons about
unions; the typer normalizes them; codegen produces sum types
identical to today's lowering.

**Alternatives considered**:

- **Model A — pure sugar with implicit conversions**: `union E = A | B`
  generates `type __E = __FromA(A) | __FromB(B)` and the typer
  inserts conversions invisibly. Rejected because the implicit
  conversions break "no implicit coercion" semantics in subtle ways.

- **Model B — first-class union type**: union is a primitive type
  carried through the typer with no lowering. Rejected because the
  runtime cost is non-trivial and Tier 1 #2 (runtime efficiency) is
  load-bearing.

Model C concentrates the cost in the typer (where it can be bounded)
and produces runtime that is identical to today's sum-type code.
Pattern matching, dispatch, codegen all reuse existing
infrastructure.

### Decision 3 — Construction: implicit upcast on assignment

**Chosen**: a value of a component type can be used wherever the
union type is expected without explicit ceremony.

```kai
let id_err : IdentityError = AccountNotFound
let qb_err : QueryBalanceErr = id_err   # implicit upcast: IdentityError <: QueryBalanceErr
```

The typer accepts the assignment because `IdentityError` is one of
the components of `QueryBalanceErr`. No `into()`, no `as`, no
`From::from` needed.

**Why implicit and not explicit**: the use case is error
propagation across DDD layers. Forcing `into(id_err)` at every
boundary defeats the purpose. The typer enforces the rule that the
component must be in the union — there is no silent loss of
information.

This is **bounded subtyping**: `T <: U` only when `U` is a union
that explicitly contains `T`. No structural subtyping, no
covariance/contravariance issues, no width/depth subtyping. The
simplest possible subtyping rule.

### Decision 4 — Pattern matching: exhaustive over all components, with type narrowing allowed

**Chosen**: pattern matching on a union type requires covering
every variant of every component. Type-narrowing patterns are
allowed for whole-component matches.

```kai
match err : QueryBalanceErr {
  AccountNotFound      -> ...   # IdentityError variant
  KycExpired           -> ...
  Frozen               -> ...
  InsufficientBalance  -> ...   # AuthError variant
  OverDailyLimit       -> ...
  RegulatoryHold       -> ...
}
```

Or with type-narrowing:

```kai
match err : QueryBalanceErr {
  ie : IdentityError -> handle_identity_layer(ie)
  ae : AuthError     -> handle_auth_layer(ae)
}
```

The two forms are interoperable — a single `match` may mix
variant patterns and narrowing patterns. Exhaustiveness is checked
across the combined patterns.

**Type narrowing** (`bind_name : ComponentType`) is a new pattern
shape. It binds the value as the component type and is exhaustive
over the union if every component is covered.

### Decision 5 — Algebraic properties: associative, commutative, idempotent

**Chosen**: unions normalize automatically.

- **Associative**: `(A | B) | C ≡ A | (B | C) ≡ A | B | C`. Nested
  unions flatten.
- **Commutative**: `A | B ≡ B | A`. Order does not matter for
  type identity.
- **Idempotent**: `A | A ≡ A`. Duplicates collapse.

This means:

```kai
union E1 = A | B
union E2 = B | C
union E3 = E1 | E2     # normalizes to A | B | C (B appears once)
```

```kai
union F1 = A | B
union F2 = B | A       # F1 ≡ F2 — same type
```

The typer normalizes unions to a canonical form (sorted by some
deterministic order, deduplicated, flattened) for equality
checking.

**Why this matters**: it makes union composition *predictable*.
DDD apps compose subsystems freely without worrying about
declaration order or duplicate components.

### Decision 6 — Restriction: `union` only composes pre-existing types

**Chosen**: `union` cannot declare variants with payloads inline.
The components must be types declared elsewhere (sum types, records,
unions, primitives).

```kai
# OK:
type Success = { value: Value }
type Failure = { reason: String }
type Pending = unit
union ParseResult = Success | Failure | Pending

# REJECTED at parse time:
union ParseResult = Success(Value) | Failure(String) | Pending
#                          ^^^^^^^             ^^^^^^^^
#                   payload syntax not allowed in `union`
```

**Why**: this preserves the separation between `type` (declares new
nominal types) and `union` (composes existing types). Without this
restriction, `union` becomes a competing form for declaring sum
types — defeating the goal of an additive feature.

The diagnostic for the rejected form points the user at `type` if
they want to declare a new sum type, or at extracting the components
into separate `type` declarations if they want a union.

## What unions are NOT

Following the precedent established by `docs/protocols.md` *What
this is not*:

| Capability | Common in other languages | kaikai unions |
|---|---|---|
| Structural subtyping | TypeScript `{x: int}` is `{x: int, y: string}`'s parent | **not supported** |
| Width / depth subtyping on records | TypeScript object subtyping | **not supported** |
| Covariance / contravariance | Scala variance annotations | **not supported** |
| Polymorphic variants | OCaml `` [`A | `B] `` row types | **not supported** |
| Tagged unions with inline payloads | TypeScript discriminated unions inline | **not supported** (use `type` first) |
| Implicit downcast (union → component) | TypeScript narrowing infers automatically | **not supported** without explicit pattern |

What kaikai unions **do** support is **bounded nominal subtyping
limited to the relationship "component appears in the union"**.
Nothing more.

## Interaction with existing features

### With pattern matching exhaustiveness

A `match` over a union must cover every variant of every
component. The exhaustiveness checker walks the components,
collects their variants, and verifies each is covered (either
explicitly or via a type-narrowing pattern).

Counterexample diagnostic: as today, the typer reports specific
missing variants. With unions, the report identifies the
component-of-origin: `"missing variant InsufficientBalance from
AuthError in QueryBalanceErr"`.

### With `Result[E, T]` + `!` propagation

```kai
fn outer() : Result[QueryBalanceErr, Balance] = {
  let v = check_identity(req)?         # returns Result[IdentityError, _]
  #       ^^^^^^^^^^^^^^^^^^^^
  #       IdentityError <: QueryBalanceErr, so ! propagates correctly
  let b = check_auth(v)?               # returns Result[AuthError, _]
  Ok(b)
}
```

`!` works transparently because the implicit upcast (Decision 3)
applies to the `Err` payload during propagation.

This **subsumes** the auto-`From` extension to `!` (deferred to
post-#180). With unions, `!` does not need to invoke `From::from`
— the upcast is structural at the union level.

### With protocols (m12.8)

`impl P for U` where `U` is a union is **not allowed** in v1.
Protocols dispatch by the head type tag; a union does not have a
single head type. The user implements `P` for each component and
the call site narrows to the component before dispatch.

If a future use case demands "`impl P for U`" semantics (dispatch
that walks each variant of each component), it can be revisited as
a separate proposal. v1 keeps unions and protocols orthogonal.

### With effects

Unions are pure types. They have no effect-row component. No
interaction with the effect system beyond being usable as the
return type of an effectful function.

### With `#derive`

`#derive(Show)`, `#derive(Eq)`, etc. on a union: the compiler walks
the components and verifies each has `impl P` reachable, then
generates a synthetic impl for the union that pattern-matches.

This is the same field-impl validation that `#derive` already does
for record fields and sum-type payloads, applied to union
components.

### With UoM and refinements (m12.5, m12.6)

Orthogonal. Unions compose at the type level; UoM and refinements
operate within a type. No interaction expected.

## Migration path for existing code

**Zero migration required.** Unions are additive. Every existing
`type` declaration remains valid with identical semantics. Every
existing wrapper sum type (`type AppError = FromA(...) | ...`)
remains valid. Code can be migrated to unions opportunistically,
file by file, with no big-bang rewrite.

The footgun warning (`type Z = T | W` where `T` and `W` are
pre-existing types) is a separate item — see *Implementation phases*
Phase 5.

## Risks

### 1. Typer normalization complexity

Normalizing unions (associativity, commutativity, idempotence,
flattening nested unions) requires care. Edge cases:

- `union E1 = A`, `union E2 = E1`, is `E2 ≡ A`? Yes, by idempotence
  + flattening of singleton.
- `union E = A | A | A`: normalizes to `A`, but should this trigger
  a warning? Decision: yes, warn but accept.
- Unions of generic types: `union F[T] = Option[T] | Result[String, T]`
  — does this work? Decision: out of scope for v1. Unions in v1
  compose **only monomorphic types**.

### 2. Diagnostic quality on union mismatches

Error messages on union construction must be clear:

```
foo.kai:42:15: type mismatch
  expected: QueryBalanceErr (= IdentityError | AuthError)
  found:    RoutingError
  note: RoutingError is not a component of QueryBalanceErr
  hint: declare `union TransferErr = QueryBalanceErr | RoutingError`
        and use that, or extend QueryBalanceErr
```

Budget specific work for diagnostics. Bad error messages on type
mismatches kill the value proposition.

### 3. Selfhost regression

stdlib loads in the bootstrap chain. The typer changes affect
stage 1 and stage 2. Any union appearing in `stdlib/protocols.kai`,
`stdlib/core/*.kai`, etc., must compile correctly through stage 0
→ stage 1 → stage 2.

Mitigation: the milestone lane runs `make selfhost` and `make
selfhost-llvm` continuously during development. Land in chunks; do
not flip stdlib usage until the typer changes have stabilized.

### 4. Drift from "few forms, each with clear intent"

Unions are a second form for "type whose values are one of several
shapes". Sum types are the first. The justification is **clear
intent difference**:

- **Sum types** declare a new closed family of variants tied
  together by domain meaning (`Shape = Circle | Square` — same
  domain).
- **Unions** compose pre-existing types from possibly different
  bounded contexts (`QueryBalanceErr = IdentityError | AuthError` —
  different domains, transient composition).

This intent difference is the same that distinguishes `|>` (apply)
from `|` (map) — both pipe-shaped, both legitimate. Documentation
must articulate the difference clearly so users do not view this as
two ways to do the same thing.

### 5. Implicit upcast breaks "no implicit coercion"

The implicit upcast (Decision 3) is the one place where the
"explicit conversions only" rule yields. The justification is the
DDD use case: requiring `into(err)` at every error boundary defeats
the proposal.

Mitigation: the upcast is **bounded** (only component-to-union, no
chains, no structural matching). The user can always read the type
and know exactly what upcasts are possible.

### 6. Interaction with future features

Unions interact with:

- **`protocol P[A]` (#180)**: unions could be `A`. Decision: out of
  scope for v1; revisit when #180 lands.
- **Polymorphic-impl runtime panic (#174)**: unions might exacerbate
  the panic if a union is passed where dispatch on its variants is
  needed. Decision: union impls are forbidden in v1 (interaction §
  *With protocols*); revisit when #174 is resolved.

## Implementation phases

The implementation lane has six phases, each landing a working,
tested subset.

### Phase 1 — Parser + AST + resolver

- Add `union` keyword to lexer (stage 0, stage 1, stage 2).
- Parser accepts `union <Name> = <Type1> | <Type2> | ...`.
- Resolver registers unions in a separate namespace from `type`.
- Validation: every component is a previously-declared type. Inline
  payload syntax rejected at parse time with the diagnostic from
  Decision 6.
- No typer / codegen yet — declarations parse but using a union is
  still a typer error.
- Fixtures: positive parse, negative parse (inline payload), name
  collision with `type`.

### Phase 2 — Typer: union as first-class type

- Internal AST representation of unions (`TyUnion(components: [Ty])`).
- Normalization function: associative flattening + commutative sort
  + idempotent dedup. Used wherever union types are compared.
- Subsumption rule: `T <: U` when `U` is a union and `T` is in
  `U`'s normalized component list.
- Bidirectional propagation: when expected type is a union and
  found type is a component, accept; when expected is a component
  and found is a union, reject (no implicit downcast).
- Fixtures: positive (each composition pattern), negative (mismatch
  on missing component, mismatch on different unions).

### Phase 3 — Pattern matching: variant patterns + type narrowing

- Exhaustiveness checker walks union components and collects
  variants from each.
- New pattern shape: `bind_name : ComponentType` (type narrowing).
  Parser, AST node, typer, exhaustiveness, codegen.
- Mixed-pattern matches (some arms use variant patterns, others
  use narrowing) handled by checking coverage at the union level.
- Fixtures: positive (all variants covered), negative (missing
  variant of a specific component), mixed-form positive.

### Phase 4 — Codegen lowering (Model C)

- The codegen pass lowers a `TyUnion` to a generated sum type with
  one variant per component (`__From<ComponentName>`).
- Implicit upcasts compile to constructor invocations.
- Pattern matching compiles to nested `match` (outer dispatches on
  the wrapper variant, inner dispatches on the component's
  variants).
- The C and LLVM backends both implement the lowering.
- Fixtures: end-to-end runnable programs using unions, with stdout
  output verified.

### Phase 5 — Footgun diagnostic

- When `type Z = T | W` is declared and `T` (or `W`) shadows a
  previously-declared type, the parser emits a warning:
  ```
  foo.kai:5:1: warning: variant `T` of `type Z` shadows the type
    `T` declared at bar.kai:3:1
    note: this declares a new zero-arg constructor named `T`,
    not a union of types
    hint: did you mean `union Z = T | W`?
  ```
- Configurable via `--strict-shadow` to make it an error.
- Fixture: shadowing case with expected diagnostic.

### Phase 6 — Documentation, stdlib, examples, demos

- Update `docs/design.md`, `docs/protocols.md` (interaction note),
  `docs/effects.md` (no interaction expected), `docs/syntax-sugars.md`
  (no interaction).
- Write `docs/unions.md` (the user-facing reference, distinct from
  this design doc).
- Update `docs/errors-conventions.md` to add a *Pattern C — unions*
  alongside Pattern A (wrapper + map_err) and Pattern B (auto-From
  post-#180).
- Migrate `stdlib/protocols.kai` impls? **No** — Phase 6 is
  documentation only. Stdlib migration is a follow-up after the
  feature has bedded in.
- Example demos: at least one in `examples/unions/` showing the
  DDD/ledger composition pattern. Probably 2-3 fixtures total.
- Update `demos/` if any cross-subsystem patterns benefit. Conservative.

### Phases NOT in v1 (deferred follow-ups)

- **stdlib protocol impls migrated to unions**: deferred. Existing
  impls work; user code can adopt.
- **Generic unions**: `union F[T] = Option[T] | Result[String, T]` —
  out of scope. Monomorphic only in v1.
- **Union with HKT** (when #180 lands): re-evaluate.
- **`impl P for U` where U is union**: forbidden in v1, may revisit.

## Estimated cost

| Phase | Lines | Effort |
|---|---|---|
| 1. Parser + AST + resolver | ~120 | 1-2 days |
| 2. Typer (normalization, subsumption) | ~250 | 3-5 days |
| 3. Pattern matching + narrowing | ~180 | 2-3 days |
| 4. Codegen lowering | ~150 | 2-3 days |
| 5. Footgun diagnostic | ~40 | 0.5 day |
| 6. Documentation + stdlib + examples | ~400 | 2-3 days |
| **Total** | **~1140 lines** | **~2 weeks** |

Comparable in scale to m12.8 (single-dispatch protocols) which
landed in similar effort. Significantly larger than #180 (`protocol
P[A]`) which is ~345 lines.

## Decision posture

This is a **milestone-level lane**, not a single PR. The size and
cross-stage impact require:

- A dedicated worktree (per `/wt-claude` or equivalent).
- Continuous selfhost validation throughout.
- Phased landing (each phase its own PR; the milestone closes when
  Phase 6 lands).
- Pre-implementation review of this design doc by the integrator.

**Roadmap impact**: ahu Q3 fintech app planning should account for
unions as a planned feature. m12.6 wave 2 (#83, #84, #157) and #180,
#182 are independent and continue.

The implementation lane is **not in this worktree**. This worktree
is design-conversation only. Implementation is tracked in the
companion milestone issue.

## Cross-references

- **Design conversation**: worktree `design-conversation`,
  2026-05-03. The conversation covered DDD/ledger error
  composition, identified the wrapper `O(N²)` problem, surveyed
  alternatives (typeclasses, OCaml polyvariants, TypeScript unions),
  empirically verified the `type Z = T | W` footgun, and converged
  on `union` keyword + Model C semantics.
- **Issue #182**: `Result` / `Option` API expansion. Independent
  but complementary — `map_err` is useful with or without unions.
- **Issue #180**: `protocol P[A]` parametrized. May interact with
  unions when future generic unions land.
- **Issue #174**: polymorphic-impl runtime panic. Affects any
  feature that dispatches at runtime, including future
  union-protocol interaction.
- **`docs/errors-conventions.md`**: documents Pattern A (wrappers +
  map_err) which unions improve upon. To be updated post-landing
  with Pattern C.
