# Protocols for kaikai

## Status: Landed (m12.8)

Stage 2 supports `protocol`, `impl`, and `#[derive(...)]` syntax with the
five stdlib protocols (`Show`, `Eq`, `Ord`, `Hash`, `Serialize`) defined
in `stdlib/protocols.kai`. The compiler resolves dispatch at the AST
level (between inference and monomorphisation), so each statically-typed
call site emits a direct `__pimpl_<P>_<T>_<op>` call on both backends
(C and LLVM). See `docs/lane-experience-m12.8.md` for the implementation
retrospective and v1 limitations.

Single-dispatch ad-hoc polymorphism via explicit `protocol` declarations
and `impl` blocks. Modeled on Clojure protocols, Elixir protocols, Go
interfaces (with explicit declaration), and the lightweight subset of
Rust traits.

**Not Haskell typeclasses.** No higher-kinded types, no constraint
propagation in signatures, no functional dependencies, no type families,
no overlapping instances. Single-dispatch only.

This document is the spec; `proposed-extensions.md §28` is the catalog
entry; `stage2-design.md §m12.8` is the schedule.

## Motivation

Three concrete pain points in current kaikai:

1. **`Money<USD>` printing**: `decimal_to_string(m.amount) ++ " USD"`
   on every interpolation. With `protocol Show` + `impl Show for
   Decimal[u: Measure]`, becomes `#{m.amount}` automatic.
2. **Generic equality / comparison / hashing**: every record / sum
   type written today needs ad-hoc `eq_<type>`, `hash_<type>`,
   `cmp_<type>` functions. With `protocol Eq` / `Ord` / `Hash`,
   one `impl` block covers it.
3. **Serialization**: `to_json(x: Money) : String`, `from_json(s:
   String) : Result[Money, JsonError]` per type. With `protocol
   Serialize`, opaque types (Decimal, BcryptHash) get the same
   surface as records.

Without protocols, each of the three has to be solved independently
with bespoke conventions or magic-name hacks. With one mechanism, all
three (and future cases) compose.

## Design

### Declaration

```kai
protocol Show {
  show(x: Self) : String
}

protocol Eq {
  eq(a: Self, b: Self) : Bool
}

protocol Ord {
  cmp(a: Self, b: Self) : Int
}

protocol Hash {
  hash(x: Self) : Int
}
```

`Self` is a reserved type-parameter referring to the type the protocol
is implemented for. Every protocol op takes `Self` (or values of
`Self`) at least once.

A protocol may declare default implementations:

```kai
protocol Show {
  show(x: Self) : String
  show_with(x: Self, prefix: String) : String =
    prefix ++ show(x)         # default; can be overridden per-impl
}
```

Default impls dispatch to other ops of the same protocol, so they are
written in terms of the protocol's own surface — no recursion into
external state.

### Implementation

```kai
impl Show for Int {
  show(n) = int_to_string(n)
}

impl Show for Money[u: Measure] {
  show(m) = decimal_repr(m.amount) ++ " " ++ unit_name(u)
}

impl Eq for Point {
  eq(a, b) = a.x == b.x && a.y == b.y
}
```

Polymorphic impls (impl for a parametric type) work when the
parameters are concrete instances of `Unit` / `Int` / etc.; the
compiler monomorphizes per concrete instantiation just like any
generic function.

### Polymorphic impls with bounded type parameters (issue #174)

A polymorphic impl whose body recurses on the type variable —
`impl[T] Show for [T]` calling `show(x : T)` per element — needs to
know `T` itself implements `Show`. Family 2 (impl-site bounded
constraint) makes that requirement explicit:

```kai
impl[T : Show] Show for [T] {
  fn show(xs : [T]) : String = match xs {
    []  -> "[]"
    [x] -> "[" ++ show(x) ++ "]"
    [x, ...rest] -> "[" ++ show(x) ++ ", " ++ show(rest) ++ "]"
  }
}
```

Bounds stack with `+`:

```kai
impl[T : Show + Eq] Show for [T] { ... }
```

**Where bounds are allowed.** Protocol bounds appear on **impl-site**
type parameters (`impl[T : Eq] ... for ...`) and, since #877, on
**free-fn** type parameters (`fn dedup[T : Eq](xs : [T])`,
`fn sum[T : Numeric](xs : [T])` — the stdlib aggregates use this).
They do **not** appear on `type`/record declarations: `type M[T : Add]`
is a parse error ("type-parameter kind must be `Type` or `Measure`").
The line that keeps single-dispatch protocols clear of Haskell
typeclasses / HKT / constraint propagation (Tier 1 #3) is *how* the
bound is honoured, not *where* it may be written: dispatch resolves
per monomorphised instance, never by a constraint that travels with
the signature. **Known gap:** a free-fn bound is currently NOT
enforced at the call site — `fn f[T : Eq](xs : [T])` whose body does
not call `eq` accepts a `T` without `Eq`; the error only appears when
the body dispatches the op on a concrete type lacking it. The bound is
effectively documentation until the body forces the op. Tracked
separately.

**How dispatch resolves.** The monomorphiser specialises the impl
once per concrete `T'` (e.g. `Int`, `String`, ...). Inside each
specialisation the call to `show(x : T)` becomes `show(x : T')`,
which the post-specialisation rewrite redirects to the matching
`__pimpl_Show_<T'>_show` direct call. No dictionary, no hidden
parameter — every dispatch is a direct call, preserving Tier 1 #2.

**When the bound is missing.** A polymorphic impl whose body recurses
on the type variable but does NOT carry the corresponding bound is
accepted by the parser; the post-monomorphisation dispatch validator
then reports a typed compile-time error pointing at the offending
`show(x)` call inside the impl body for any concrete `T'` without
the required impl. Pre-#174 this case panicked at runtime with the
opaque `__protocol_dispatch_Show_show` message.

**Non-recursive polymorphic impls** (the body never dispatches a
protocol op on `T`) do not need a bound and continue to work as
before:

```kai
impl[T] Show for [T] {
  fn show(xs : [T]) : String = "list-of-something"   # OK, no recursion on T
}
```

### Use sites

Protocol ops are called as ordinary functions:

```kai
let s = show(my_money)              # dispatches to Show for Money[u]
let same = eq(p1, p2)               # dispatches to Eq for Point
let order = cmp(t1, t2)             # dispatches to Ord for Transaction
```

In string interpolation, `Show` is invoked implicitly when an
interpolation expression has a type that has `impl Show`:

```kai
"alice: #{m}"
# m: Money[USD]
# expands at compile time to "alice: " ++ show(m)
```

If the type has no `impl Show`, the typer emits an explicit error
pointing at the missing impl — no silent fallback to bytes or
`<opaque>`.

### Coherence rules

**One impl per (protocol, type) pair.** If a second `impl Show for
Money[u]` appears in the same compilation unit, the resolver emits a
duplicate-impl error.

**Orphan rule** (Rust-style): an `impl P for T` is allowed only when
**P** is declared in the current module **or** **T** is declared in
the current module. This prevents two foreign packages defining
conflicting impls on a type they both import.

The orphan rule keeps coherence local: the compiler can determine the
single applicable impl by searching only the modules transitively
imported. No global registry.

### What protocols cannot do (deliberately)

- **No constraints in signatures**: there is no `fn sort[T : Ord](xs:
  [T]) : [T]`. Generic functions take an explicit comparator
  (`fn sort[T](xs: [T], cmp: (T, T) -> Int) : [T]`). If you want the
  comparator to come from a protocol, the call site passes `cmp` from
  `Ord` explicitly:
  ```kai
  list.sort_by(transactions, cmp)   # cmp: (Transaction, Transaction) -> Int
                                    # provided by impl Ord for Transaction
  ```
  This is intentional. Keeping constraints out of signatures is the
  sharp distinction from Haskell typeclasses and the main reason
  compile times stay flat.

- **No higher-kinded types**: `protocol Functor[F[_]]` does not parse.
  Type parameters of protocols are first-order (`protocol P[a]`,
  lowercase per kaikai's tparam convention), and the only way `Self`
  is parametric is when the impl target is parametric (`impl Show for
  Money[u: Measure]`). Issue #180 implements `protocol P[a]` for
  single-dispatch parametrized protocols — the impl table key remains
  `(P, Self)`, so adding `[a]` does NOT introduce multi-method
  dispatch (see §*Single-dispatch parametrized protocols (#180)*
  below).

- **No multi-method dispatch**: dispatch always uses `Self` (the
  first-position type). For two-arg dispatch (e.g. `convert(from:
  T, to: U)`), the user writes a free function and dispatches manually
  via match. **However**, single-dispatch *parametrized* protocols
  (`protocol P[A] { ... }`) cover the most common multi-type use
  cases without violating single-dispatch — see §*Multi-method
  dispatch — analysis* below.

- **No overlapping impls**: the orphan rule plus single-impl coherence
  rules out the situation Haskell `OverlappingInstances` has to handle.

### Resolution algorithm

For a call `op(x, ...)` where `op` is declared in `protocol P`:

1. The typer infers `T = type-of(x)`.
2. The resolver looks up `(P, T)` in the impl table built from
   transitively imported modules.
3. If exactly one impl is found, the call is rewritten to the impl's
   function. Monomorphization proceeds normally.
4. If zero impls are found, the typer emits "no impl of `P` for `T`",
   listing the imports that would provide one if any exist in the
   ecosystem (LLM-friendly hint).
5. If more than one impl is found, the orphan rule was violated
   somewhere upstream — the resolver emits the conflicting modules.

The lookup is `O(1)` hash by `(P, T)` key. No constraint propagation,
no unification beyond what monomorphization already does.

### Runtime cost

For monomorphic call sites (the typer can statically pick the impl
during monomorphization), the call is **direct** — no indirection.
Same cost as a regular function call.

For sites that genuinely need late binding (passing a value through
generic code that does not see the concrete type until runtime), the
cost is **one vtable indirection** — same as Go interfaces, Clojure
protocols, Elixir protocols. Negligible for all practical purposes.

The runtime emits one **vtable per protocol per implementing type**,
declared `static` in the C / LLVM output. Tables are linked at compile
time; no dynamic registry.

### Codegen sketch (C backend)

```c
/* per protocol declaration */
typedef struct kai_show_vtable {
  KaiValue *(*show)(KaiValue *self);
} kai_show_vtable;

/* per (protocol, type) impl */
static kai_show_vtable kai_show_vtable_for_money_usd = {
  .show = kai_money_usd_show,
};

/* per call site */
KaiValue *kai_call_show(KaiValue *self) {
  kai_show_vtable *vt = kai_lookup_vtable(KAI_PROTOCOL_SHOW, self->type_tag);
  return vt->show(self);
}
```

For statically-resolved sites, the lookup is elided and the call is
direct.

## Stdlib protocols (the v1 set)

The m12.8 milestone shipped five protocols; issue #258 added
`Default` for type-driven default values:

| Protocol | Ops | Use cases |
|---|---|---|
| `Show` | `show(x: Self) : String` | interpolation, debug, logging |
| `Eq` | `eq(a: Self, b: Self) : Bool` | equality, set membership, dedup |
| `Ord` | `cmp(a: Self, b: Self) : Int` | sort, min, max, ordered containers |
| `Hash` | `hash(x: Self) : Int` | hash maps (m14), hash sets, memoization |
| `Serialize` | `to_string(x: Self) : String`, `from_string(s: String) : Result[Self, String]` | json/csv/fix bindings via wrappers |
| `Default` | `default() : Self` | accumulators, container clearing, generic zero values |

Stdlib provides default impls for all primitives (`Int`, `Real`,
`Bool`, `Char`, `String`, `Unit`, `[a]`, `Option[a]`, `Result[a, e]`,
records auto-derived via #derive, sum types auto-derived). User-defined
opaque types must `impl` themselves. The parametric containers carry
`Show` / `Eq` / `Ord` / `Hash` (see `stdlib/protocols.kai`); `Serialize`
on parametric containers needs return-type-driven dispatch and is
deferred to a later increment.

Adding another protocol to stdlib in v1 (e.g. `Numeric`, `Monoid`)
requires a separate proposal — the v1 set stays intentionally tight.

### `Default` (issue #258)

```kai
protocol Default {
  default() : Self
}

impl Default for Int    { fn default() : Int    = 0 }
impl Default for Real   { fn default() : Real   = 0.0 }
impl Default for Bool   { fn default() : Bool   = false }
impl Default for String { fn default() : String = "" }
impl[T] Default for [T]       { fn default() : [T]       = [] }
impl[T] Default for Option[T] { fn default() : Option[T] = None }
```

Canonical defaults: numeric zero, false, empty string, empty list,
absent option. `Result[T, E]` is intentionally absent — neither
`Ok(default()) : Result[T, _]` nor `Err(default()) : Result[_, E]`
is a privileged choice, so callers construct results explicitly.

`default() : Self` has Self only in the return position, so v1
single-dispatch resolves it the same way `Serialize.from_string` and
`From[a].from` do — via a `let x : T = default()` annotation that
pins Self before the dispatcher runs. Generic free-fn helpers like
`fn make[T : Default]() : T = default()` are out of reach until
free-function tparam bounds gain protocol awareness; the bound is
already accepted in `impl[T : ...]` headers, so polymorphic impls
that recurse through `default()` work today.

The polymorphic impls intentionally drop the `T : Default` bound —
the bodies (`[]`, `None`) are constants in `T` and never recurse,
which matches issue #174 case 3. The Show/Eq/Ord/Hash impls in #175
keep their bound only because their bodies do recurse on `T`.

Use cases: accumulator initial values, generic record clearing
(`uira/boids.kai` has `iclear` / `rclear` exactly because this was
missing), and the post-v1 `Array[T]` zero-fill helper. Auto-derive
for user records is a separate feature and not part of v1.

## Auto-derivation via `#[derive(...)]` annotation

For records and sum types whose fields all already have `impl P`, the
user can derive trivially:

```kai
#[derive(Show, Eq, Hash, Ord)]
type Account = { id: AccountId, balance: Decimal[USD], txs: Int }

#[derive(Show, Eq, Hash)]
type Shape = Circle(Real) | Rect(Real, Real) | Empty_
```

The compiler generates the obvious structural impls:
- **Records** — field-by-field for every protocol.
- **Sum types** — outer-match per variant for `Show` and `Eq` (the Eq
  arm rematches the second argument with the same variant and conjoins
  per-field `eq` calls; the wildcard returns `false`); per-variant tag
  combined with field hashes via `acc * 31 + hash(field)` for `Hash`.
  The chosen tag-and-fold scheme preserves the Hash↔Eq invariant: two
  values judged equal by the derived `Eq` produce the same `Int` from
  the derived `Hash`.

The annotation is one of three `#`-prefixed meta-instructions
currently recognised by the parser. The others are `#[unstable]`
(issue #602), which marks a `pub` declaration as outside the
edition stability contract — see `docs/editions.md` for the
opt-in workflow — and `#[doc("...")]` (issue #681), which attaches
documentation text to a declaration (or, in module position, to the
file). Other annotations (`#deprecated`, etc.) live in their own
whitelist per `proposed-extensions.md §27`.

`#[derive(...)]` is **opt-in**: if the user does not write the annotation, no
impl is generated. This avoids surprising user-defined `impl Show for
Account` getting shadowed by an auto-derived one.

For sum types, `#[derive(Eq)]` and `#[derive(Hash)]` reject at compile
time when any variant carries a field whose head type lacks a
reachable `impl P` (explicit `impl P for T`, another `#[derive(P)]`, or
a builtin from stdlib). The diagnostic names the variant and the
offending field type. Records continue to fall through to the
dispatcher's runtime panic for missing field impls; promoting that
to a compile-time error is left for a follow-up.

### Supported protocols and shapes

| Protocol | Records | Sum types | Notes |
|---|---|---|---|
| `Show` | ✓ | ✓ | renders `Type { f: v, ... }` and `Variant(arg, ...)` |
| `Eq`   | ✓ | ✓ | record fields conjoined with `and`; sum types validated against field-impl set |
| `Hash` | ✓ | ✓ | polynomial mix (`h * 31 + h_next`); sums validated against field-impl set |
| `Ord`  | ✓ | ✓ | lexicographic by declaration order; sums by variant position then payload |
| `Serialize` | — | — | needs return-type-driven dispatch (post-v1) |

### Order semantics for `#[derive(Ord)]`

Records compare lexicographically in declaration order: the first
field whose `cmp` returns non-zero decides the result. Sum types
compare by **variant declaration position** first (variant 0 < variant
1 < ...); within the same variant, payloads compare lexicographically
by position. The synthesised `cmp` for

```kai
#[derive(Ord)]
type Tier = Bronze | Silver | Gold | Platinum
```

returns `-1` for `cmp(Bronze, Silver)`, `1` for `cmp(Gold, Bronze)`,
and `0` for `cmp(Platinum, Platinum)`. The same rule extends to
variants with payload: `cmp(Circle(99), Square(0)) == -1` because
`Circle` is declared before `Square`, regardless of the inner radius.

`#[derive(Ord)]` is consistent with `#[derive(Eq)]`: when both are
derived (or `Eq` is hand-written to mirror the same field traversal),
`cmp(a, b) == 0` ↔ `eq(a, b)`. The compiler does not enforce this
across hand-written impls; users who derive only one of the two
should keep them in sync.

### Field-impl validation

Before lowering a `#[derive(Ord)]` annotation, the compiler walks each
field (or variant payload) and confirms the head type appears either
in a same-unit `impl Ord for T` or in another `#[derive(Ord)]`. Missing
impls are rejected at typer time with a diagnostic naming both the
offending field and its type, e.g.:

```
foo.kai:7:1: error: cannot `#[derive(Ord)]` for `Tagged`: no `Ord`
impl for field `tag` of type `String` (declare `impl Ord for String`
or `#[derive(Ord)]` on `String`)
```

The same kind of validation runs for `#[derive(Eq)]` and `#[derive(Hash)]`
on sum types (m12.8.x); the gap remaining is records with `Show` /
`Eq` / `Hash` annotations referencing missing field impls — those still
fall through to the dispatcher's runtime `panic`. Tightening that
case to a compile-time error is a follow-up captured in
`docs/m12.8-followup.md` (retired 2026-05-02; see git history).

## Composition with other features

### With UoM (m12.5)

```kai
unit USD
unit EUR

impl Show for Decimal[u: Measure] {
  show(d) = decimal_repr(d) ++ " " ++ unit_name(u)
}

let usd = 100.50<USD>
let eur = 89.75<EUR>
println("balances: #{usd}, #{eur}")
# → "balances: 100.50 USD, 89.75 EUR"
```

The `u: Unit` parameter monomorphizes per-currency at the call site;
the `unit_name` runtime helper provides the symbol.

### With refinements (m12.6)

```kai
type Email = String where matches ~r/^[^@]+@[^@]+$/

impl Show for Email {
  show(e) = e   # already a string
}
```

Refined types `T where P` inherit `impl P` from their base type
unless the user provides a more specific impl.

### With effects

Protocols are **pure** — `impl P for T` ops cannot have effect rows.
If a use case needs effects (e.g. `print` to console), the operation
goes via an effect, not a protocol.

```kai
impl Show for Money[u: Measure] {
  show(m) = ...   # pure: returns String
}

# Wrong shape — not allowed:
# impl Audit for Account { log(a) : Unit / Audit ... }
# Use an effect, not a protocol, for behaviour with effects.
```

This keeps protocols compositional and predictable. A future protocol
extension to support effect rows is possible but explicitly out of v1.

#### Why the rule (the four reasons)

The hard rule exists for four reasons, ordered by load-bearing:

1. **Single-dispatch + effect rows = combinatorial complexity in the
   vtable.** Today `kai_show_vtable` is a struct with one function
   pointer. With per-impl effect rows, the vtable would need to carry
   row information so the runtime can know what handlers must be
   installed at the dispatch site. The clean monomorphic path lost.
2. **Cross-impl row inference fights Tier 1 #3.** If `impl Audit for
   Account { log(a) : Unit / Log + Time }` and `impl Audit for
   Transaction { log(a) : Unit / Log }`, then a generic call
   `log(x)` where `x: T` has a row that depends on the resolved impl
   — that is row polymorphism, which the current typer does not
   propagate.
3. **Caller predictability.** A user who writes `log(x)` wants to
   know which handlers to install in `handle { ... } with X { ... }`.
   If the row varies per impl, the caller cannot know what to expect
   without seeing the concrete type — defeats local reasoning.
4. **Conceptual cleanliness.** Protocols describe *what a value is*
   (Show, Eq, Ord). Effects describe *what a computation does*
   (Console, File, Net). Mixing them blurs the line that the rest of
   the language depends on.

Reasons (1)–(3) are technical and load-bearing. Reason (4) is
cultural and informs which use-cases are actually a misfit (effects
that *describe* a value belong in `Show` / `Serialize`; effects that
*do* something belong in an effect declaration).

#### Use cases that pressure the rule

Four shapes have been considered for kaikai. None is acute today;
documented here so future contributors can match a real case to a
proposal rather than re-derive the catalog.

1. **Structured logging.** `protocol Logable { to_log_event(x: Self)
   : LogEvent / Log }` — every type decides how it serialises into a
   log event. Today: free function `to_log_event_account(a) : ... /
   Log` per type, with manual dispatch.
2. **Serialization with effects.** `Serialize.to_string` is pure
   today. A type whose serialisation reads `Time.now()` for a
   timestamp, or `Random.uuid()` for an ID, must compute those
   effects beforehand and pass values in.
3. **Iterators backed by I/O.** `protocol Iterator[T] { next(self:
   Self) : Option[T] / Io }` — a stream over a file or socket would
   want this. Today these are written as effects directly, no
   protocol.
4. **Validation with external lookups.** `protocol Validate {
   check(x: Self) : Result[Unit, Error] / DbLookup }` for
   validations that consult a database. Today: free function.

Cases 2–4 imply an effect row that varies per impl (different types
need different effects to validate / serialise / iterate). Case 1 is
uniform — every `Logable` impl needs the same `Log` row.

#### The four design alternatives (none chosen, all evaluated)

For any future revisit, four families have been considered. They
differ in expressiveness vs. typer cost.

##### A. Status quo (the current rule)

`impl P for T` is pure. Use-cases above are written as free
functions with manual dispatch.

- **Pros**: zero typer cost, zero runtime cost, single-dispatch
  surface remains the simplest possible. No risk of slipping toward
  typeclasses or row polymorphism.
- **Cons**: loses dispatch ergonomics for cases 1–4. The user
  matches manually on the type or threads function pointers.
- **When this is right**: when no case of 1–4 dominates real code.
  Today's posture.

##### B. Uniform effect row declared by the protocol

```kai
protocol Logable / Log {
  to_log_event(x: Self) : LogEvent
}
```

The protocol declares its row at declaration site. Every `impl
Logable for T` must have *exactly* that row — no more, no less.
The vtable layout is unchanged because the row is fixed in advance,
not per-impl.

- **Pros**: caller predictability is total (every site sees the
  protocol's declared row). Vtable simplicity preserved. No
  cross-impl row inference. Closest to "protocol describes a
  uniform contract".
- **Cons**: rigid — case 2 (serialisation that needs `Time` for
  one type, `Random` for another) does not fit. Forces the
  protocol's row to the union, polluting impls that do not use all
  of it.
- **When this is right**: case 1 (logging) is the only real case;
  the implementations are uniform across types.

##### C. Per-impl effect row, monomorphic-only

Each impl declares its own row:

```kai
impl Logable for Account     { to_log_event(a) : LogEvent / Log + Time }
impl Logable for Transaction { to_log_event(t) : LogEvent / Log }
```

At monomorphic call sites (the typer knows `T = Account`), the row
is instantiated from the resolved impl. Polymorphic call sites
(`fn debug[T : Logable](x: T)`) carry a row hole that must be closed
at the next monomorph step or rejected.

- **Pros**: covers cases 2–4 cleanly when call sites are monomorphic
  (the dominant case). Zero runtime cost.
- **Cons**: introduces row polymorphism at protocol boundaries. The
  typer must close the row at every monomorph site — a non-trivial
  pass. Risk of typer slowdown.
- **When this is right**: cases 2 or 3 dominate, and most call sites
  are monomorphic.

##### D. Per-impl row bounded by a declared upper bound

```kai
protocol Logable / e where e ⊆ Log + Time + Io {
  to_log_event(x: Self) : LogEvent / e
}

impl Logable for Account     { to_log_event(a) : LogEvent / Log + Time }
impl Logable for Transaction { to_log_event(t) : LogEvent / Log }
```

The protocol declares the *maximum* row any impl may take; each
impl picks a subset. Callers see the upper bound conservatively —
worst-case row is known at the declaration.

- **Pros**: caller can install handlers for the upper bound and be
  safe. Per-impl flexibility preserved. No full row polymorphism —
  only subset checking, decidable.
- **Cons**: heavier syntax (`/ e where e ⊆ ...`). The protocol
  author must think about the row at declaration time.
- **When this is right**: cases 2–4 dominate AND callers want
  predictability. The closest to "row polymorphism without the
  cost".

#### Recommendation if the rule is reopened

If a real case from (1)–(4) becomes load-bearing in kaikai or in a
downstream framework (`ahu`, `kohau`, `henua`, `manutara`),
re-evaluate in this order:

1. Does case 1 (uniform logging) cover the need? If yes, **B** is
   the safest reopening — minimum typer impact, maximum caller
   predictability.
2. Do cases 2–3 (per-type variation) dominate? Pick **C** if most
   call sites are monomorphic, **D** if callers need to plan
   handler installation in advance.
3. Otherwise, A (status quo) wins by default. The cost of opening
   the rule must be justified by a concrete case, not "it would be
   nice to have".

This sub-section is documentation of analysis, not a commitment.
v1 ships with **A** (no effects in protocols). Reopening requires a
fresh proposal with a concrete use case from the catalog above.

## What this is not

To make the boundary with Haskell typeclasses unambiguous:

| Capability | Haskell typeclass | kaikai protocol |
|---|---|---|
| Instance declaration | `instance Show Int where ...` | `impl Show for Int { ... }` |
| Higher-kinded types | `class Functor f where fmap :: (a -> b) -> f a -> f b` | **not supported** |
| Multi-param classes | `class Convert a b where ...` | **not supported** (single dispatch only) |
| Functional dependencies | `class C a b | a -> b` | **not supported** |
| Type families | `type family F a` | **not supported** |
| Constraints in signatures | `Show a => a -> String` | **not supported** |
| Superclass constraints | `class Eq a => Ord a where ...` | **not supported** (each protocol is independent) |
| Overlapping instances | `OverlappingInstances` extension | **not supported** (orphan rule + coherence) |

What kaikai protocols **do** support is exactly what Go interfaces +
Clojure protocols + Elixir protocols support: **single dispatch by
type tag, with explicit declaration, on a closed set of impls per
compilation unit**.

### Interaction with union types (issue #187)

`impl P for U` where `U` is a union (`type U = A | B | C`) is **not
allowed in v1**. Single-dispatch resolves on a head type tag; a
union has no single head. Implement `P` for each component
separately, and narrow at the call site:

```kai
type IdentityError = AccountNotFound | KycExpired
type AuthError     = InsufficientBalance | OverDailyLimit
type QueryErr      = IdentityError | AuthError

impl Show for IdentityError { ... }
impl Show for AuthError     { ... }

fn render(err: QueryErr) : String = match err {
  ie : IdentityError -> show(ie)     # dispatches on IdentityError
  ae : AuthError     -> show(ae)     # dispatches on AuthError
}
```

A future `impl P for U` semantics that walks each variant of each
component is conceivable but not proposed; v1 keeps unions and
protocols orthogonal. See `docs/unions-design.md` *With protocols*
and `docs/unions.md` *Out of scope (v1)* for the rationale.

## Multi-method dispatch — analysis

A real fintech / domain-modeling target (ahu Q3) raises the question:
how does kaikai handle dispatch by more than one type? Currency
conversion (`USD → EUR`), wire format encoding (`(Entity, FixMsg)`),
context-sensitive serialization, cross-type validation — all involve
two or more types in the dispatch decision.

This section documents the analysis. **kaikai chose B** (single-
dispatch parametrized) over **C** (multi-method dispatch real). The
reasoning is preserved here so future contributors and downstream
framework authors (`ahu`, `kohau`, `henua`, `manutara`) can match a
concrete case to a proposal rather than re-derive the catalog.

### The four alternatives

#### A. Status quo — free functions

Every conversion / cross-type operation is a free function with a
naming convention (`money_usd_to_eur`, `decode_fix_to_transaction`).
No dispatch.

- **Pros**: zero language cost. Predictable. Already works.
- **Cons**: zero discoverability. `O(N²)` explosion when many
  type pairs are involved. No polymorphism.
- **When right**: small, fixed sets of conversions.

#### B. Single-dispatch parametrized — `protocol P[A]` (chosen)

```kai
protocol From[A] {
  fn from(a: A) : Self
}

impl From[Money[USD]] for Money[EUR] { ... }
impl From[Money[EUR]] for Money[USD] { ... }

let eur : Money[EUR] = from(my_usd)   # Self resolved by annotation
```

The protocol carries a type parameter `A` alongside the implicit
`Self`. Dispatch remains single-dispatch by `Self`. `A` is
monomorphized at impl-site. Vtable layout `(P, Self) -> fn`
unchanged.

- **Pros**: covers fintech cases 1, 3, 4, 5 (currency conversion,
  wire format decode, context-sensitive serialization, cross-type
  validation). No vtable change, no orphan-rule complication.
  Bidirectional inference at the call site (same machinery typed
  holes use). Precedent: Rust `From<A> for B`.
- **Cons**: requires call-site annotation when `Self` is not
  determined by the arguments. Syntactically asymmetric for
  bidirectional cases (two impls `From[USD] for EUR` and
  `From[EUR] for USD`).
- **When right**: most fintech use cases. The chosen path.

Implementation tracked in **issue #180**.

#### C. Multi-method dispatch — `protocol P { op(a: A, b: B) }` real

```kai
protocol Convert[From, To] {
  convert(x: From) : To
}

impl Convert[Money[USD], Money[EUR]] { fn convert(m) = ... }
```

The vtable is keyed by `(P, T1, T2)`. Lookup is `O(1)` but the
table grows with the cross-product of type pairs.

- **Pros**: mathematically correct for multi-type dispatch. Covers
  truly bidirectional comparators (`cmp(USD, EUR)` and `cmp(EUR,
  USD)` as one mechanism, not two impls).
- **Cons**:
  - Breaks "single-dispatch by `Self`" — Tier 1 #3 commit.
  - Orphan rule generalises poorly: who owns `Convert[USD, EUR]`?
    The USD module, the EUR module, neither?
  - Resolution complexity grows with parametrized types
    (`impl Convert[Money[USD], Money[EUR]]` vs `impl
    Convert[Money[u], Money[v]]` ambiguity).
  - Dispatch by-value-of-arbitrary-fn (Clojure's actual strength)
    requires runtime dispatch tables, not vtables.
- **When right**: domains where every operation depends on multiple
  types symmetrically. Empirically rare — see §*Why not C* below.

#### D. Convention + helper — manual dispatch with type witness

```kai
fn convert[A, B](x: A, target: TypeOf[B]) : B = match (x, target) {
  ...
}
```

Equivalent to A but with a runtime type witness. Adds nothing
over A.

- **Pros**: none over A.
- **Cons**: hides the dispatch in a switch.
- **When right**: never.

### Why not C — empirical evidence from Clojure

Clojure has both single-dispatch protocols (which kaikai's m12.8
imitated) and multimethods (`defmulti` / `defmethod` with arbitrary
dispatch functions). Fifteen years of community practice show six
dominant use cases for multimethods:

| Domain | Multi-dispatch genuinely needed? | kaikai covers with B + sum types? |
|---|---|---|
| 1. Event-driven / state machines | partial — by value, not by type | yes, exhaustive pattern matching |
| 2. Polymorphic serialization (per-context) | partial | partial — B covers, not extensible post-hoc |
| 3. Compiler / AST passes | no — closed sum type | yes, pattern matching is superior |
| 4. Math / unit conversion | no — UoM is type-level | yes, UoM (m12.5) + B |
| 5. Visitor / tree transforms | no | yes, pattern matching |
| 6. Plugin extensibility | partial — open-world dispatch | partial with B (orphan-rule-bounded) |

Four of six dominant Clojure-multimethod patterns are **better
covered** by kaikai's existing pattern matching + sum types. The
remaining two are **adequately covered** by B with a small
ergonomic gap.

The genuinely irreducible case where C wins over B is **dispatch by
runtime value (not type) extensible post-hoc** — and that is
expressible in kaikai with `Map[K, V]` + first-class functions
(`Map.from([(Compact, ser_compact), ...])`), not a language
feature.

### Why C costs more than it gains for kaikai

Even setting aside Tier 1 #3, C carries technical costs that B
avoids:

1. **Vtable layout change.** From `(P, Self) -> fn` to `(P, T1,
   T2, ..., Tn) -> fn`. The lookup is still `O(1)` by hash, but
   every caller site needs to compute an n-tuple key. Not free.
2. **Orphan rule generalises poorly.** With single-arg dispatch,
   `impl P for T` is allowed when `P` or `T` is local. With
   multi-arg dispatch, "local" must extend to "at least one of P,
   T1, T2, ... is local" — and that breaks the cleanest version
   of the rule (any of the participants gives ownership). Two
   third-party libraries can both legitimately claim ownership of
   `Convert[A, B]` if they own `A` and `B` respectively.
3. **Resolution with parametrized types becomes ambiguous.** `impl
   Convert[Money[USD], Money[EUR]]` and `impl Convert[Money[u],
   Money[v]]` both apply to a call `convert(usd_amount, eur_amount)`.
   The "most specific" rule (Haskell's `OverlappingInstances`,
   Julia's method ambiguity) is exactly the complexity kaikai
   chose to avoid in m12.8.
4. **Code navigation cost.** "Which impl runs at this call?"
   becomes harder when the answer depends on the cross-product of
   inferred types at the call site. LSP / `kai type --json`
   queries become more expensive.

### When C should be reopened

Reopen the multi-method discussion **only when** a concrete kaikai
or downstream-framework use case shows that B + sum types + pattern
matching is genuinely insufficient. Specifically:

- A documented fintech (or other domain) operation that has no
  clean expression as `protocol P[A]` with `A` resolved by
  bidirectional inference.
- A documented case where dispatch must depend on two or more
  *open-world* type sets simultaneously (third-party library
  authors extending in both directions independently).
- Performance evidence that B's per-conversion impl explosion
  dominates compile time or binary size.

Until one of these manifests, C remains rejected. B (issue #180) is
the path forward.

### Cross-references

- **Issue #180** — implementation of B (`protocol P[A]`,
  bidirectional inference at call site, fintech Q3 motivation).
- **Issue #174** — polymorphic-impl runtime panic. Affects B's
  impls whose body dispatches on `A`. May need to land first.
- **m12.5 UoM** — covers cases where the multi-type dispatch is at
  the *value-arithmetic* level (`Decimal<USD> + Decimal<USD>` vs
  `Decimal<USD> + Decimal<EUR>`).

## Implementation cost

| Component | Lines | Notes |
|---|---|---|
| Parser: `protocol` + `impl` keywords + bodies | ~50 | `Self` reserved; parameter parsing |
| Resolver: build impl table per module + orphan check | ~60 | `(protocol, type) -> fn` map |
| Typer: resolve protocol calls; substitute impl fn at monomorph | ~50 | reuses the existing monomorphization pass |
| Codegen: emit vtable structs + lookup helpers | ~50 | C backend; LLVM mirror in m7c-style follow-up |
| Stdlib: declare 5 protocols + impls for primitives | ~80 | Show/Eq/Ord/Hash/Serialize for Int/Real/Bool/Char/String/Unit/list/Option/Result |
| `#[derive(...)]` annotation: auto-generate structural impls | ~50 | walks record / sum type definition |
| Tests + fixtures | ~80 | one fixture per protocol, positive + negative |
| Doc updates: this file + CLAUDE.md clarification + proposed-extensions §28 | ~30 | + cross-refs |
| **Total** | **~450 lines** | **~2-3 days at observed velocity** |

## Risks

1. **Slope to "we need a bit more"**. The first user request will be
   `protocol Functor[F[_]]` or `protocol Convert[A, B]`. Mitigation:
   the design doc (this file) lists what is out of scope as
   "explicitly not supported". Re-opening means a separate proposal
   with explicit cost analysis, not extension of this one.

2. **Coherence in selfhost**. The kaikai compiler itself uses many
   ad-hoc functions (`int_to_string`, `decimal_repr`, etc.). Migrating
   them to protocols is opt-in, not required — the compiler can keep
   the explicit functions while user code adopts protocols. If the
   migration of stdlib to protocols breaks selfhost, revert that
   piece.

3. **Compile-time impact**. The orphan check + impl table build
   adds work. Expect 2-5% slower compile per file. Mitigation:
   measure on selfhost; if it crosses 10%, revisit the algorithm
   (likely a O(n²) somewhere).

4. **Diagnostics quality**. "no impl of `P` for `T`" with bad span
   information is a confusing error. Budget specific work for clean
   diagnostics with suggested impl skeleton (LLM-friendly:
   `--impl-suggest-json` may be worth a follow-up).

## Decision posture

Schedule as **m12.8 — Single-dispatch protocols**, after m12.7 axiom
and before m13 bench + bit ops. Rationale: the typer is maturest
post-m12.7 (refinements + contracts + UoM all landed), so the dispatch
infrastructure plugs into a stable foundation. Lands as its own
milestone with a fresh worktree.

Cost: 2-3 days. Lower-bound for serious value: stdlib's 5 protocols
ship with primitive impls, `#[derive(...)]` works, demos can use `#{...}`
freely.

## Single-dispatch parametrized protocols (#180)

Issue #180 lands `protocol P[a]` — a protocol declares one or more
type parameters (alongside the implicit `Self`) bound at impl-site:

```kai
protocol From[a] {
  from(x: a) : Self
}

impl From[Money[USD]] for Money[EUR] {
  fn from(m: Money[USD]) : Money[EUR] = ...
}
```

Surface contract:

- **Tparam names**: lowercase (`a`, `other`, `ctx`, …). Uppercase
  forms (`A`, `Other`) are valid syntax but are rejected later by
  the implicit-tparam classifier — keep to lowercase.
- **Impl-site arguments are concrete**. Polymorphism in `a`
  (`impl[T] From[T] for X`) is out of scope for v1; that requires
  #174 work and is deferred.
- **Default-Self backwards compat**. Legacy `impl P for X` parses
  unchanged and is treated as `impl P[X] for X` (the homogeneous
  case). Existing primitive impls do NOT need migration.
- **Dispatch table key stays `(P, Self)`**. The proto-arg list lives
  inside the impl's mangled C name (`__pimpl_<P>_<T>_<op>__a_<arg>`)
  so multiple `impl P[A] for T` instances with different `A` coexist
  in the same compilation unit.

### Bidirectional inference

When `Self` does not appear in any of an op's parameter types
(`from(x: a) : Self`), the call site has no Self informant in its
argument list. The compiler resolves Self from the surrounding
**expected type** — typically a let-binding annotation:

```kai
let eur : Money[EUR] = from(my_usd)   # OK: Self pinned to Money[EUR]
let eur = from(my_usd)                # error: no impl of From for Money[USD]
```

The unification step in `synth_stmt SLet` already pins the call's
return type when an annotation is present; the post-inference
rewrite (`try_rewrite_proto_call_with_ret`) then reads that pinned
type and dispatches accordingly. Function return positions and
explicit type ascriptions on sub-expressions provide the same hook
in their respective phases (today's implementation only honours
let-binding annotations; broader bidirectional inference is a
follow-up).

### Heterogeneous arithmetic

`Add[a] for X` lets `synth_binop` route across types:

```kai
impl Add[Real]    for Complex { ... }
impl Add[Complex] for Real    { ... }

let z = 2.0 + 3.0i              # Real + Complex
let w = complex.mk(0.0, 3.0) + 2.0   # Complex + Real
```

`synth_binop` consults a snapshot of the post-`lower_protocols` impl
table (carried in `InferState.proto_impls`) and routes to
`__proto_<op>` whenever a (lhs, rhs) pair has a registered impl
(homogeneous, prim/nonprim, or two distinct nonprim heads). When no
impl exists the binop falls through to the standard mismatch
diagnostic.

### Out of scope (v1)

- Polymorphic impl in `a` (`impl[T] From[T] for X`) — issue #174.
- Multi-method dispatch (`(P, T1, T2, ...) -> fn`) — Tier 1 #3 commit.
- `#[derive(...)]` synthesis for parametrised protocols — follow-up.
- Bidirectional inference beyond let-binding annotations (function
  return type, explicit type ascriptions on sub-expressions) —
  follow-up.

## Operator-overload protocols (Wave Op series)

The arithmetic-style operators promote to single-dispatch protocols
following the same recipe as `Add` / `Sub` / `Mul` / `Div` (#246):

| Operator | Protocol | Method | Notes |
|---|---|---|---|
| `+`  | `Add` | `add` | issue #246 / #180 |
| `-`  | `Sub` | `sub` | issue #246 / #180 |
| `*`  | `Mul` | `mul` | issue #246 / #180 |
| `/`  | `Div` | `div` | issue #246 / #180 |
| `%`  | `Rem` | `rem` | Wave Op-1 — `Int` and `Real` impls (issue #364 wired the libm `fmod` binding) |

Primitive `Int <op> Int` and `Real <op> Real` arithmetic for `+`,
`-`, `*`, `/` stays on the unify-and-emit fast path — the typer
rewrites to the protocol method only when both operands resolve to
a concrete non-primitive type (or when issue #180's heterogeneous
routing matches a registered impl). `//` (integer division)
intentionally does **not** overload — modulo is the protocol case
for user types (rationals, modular integers, residue rings); `//`
keeps its Int-only contract.

`%` is the exception: `Real % Real` always routes through `Rem.rem`
because stage0's primitive `%` is Int-only. The `Real` impl
delegates to the `real_rem` libm binding (`fmod`), so `r1 % r2`
returns the IEEE-754 remainder — `5.5 % 2.0 == 1.5`. NaN propagates
on a zero divisor (`a % 0.0` is NaN, matching the rest of the libm
surface from issue #343); inspect with `real_is_nan` at
boundaries.

### Pipe operators are convention-based, not protocol-based (#594)

`|` (map pipe), `||` (flat-map pipe), and `|?` (filter pipe) do
**not** dispatch through a protocol. Two of the structural rules
this document enumerates make the protocol path unworkable for
them, and a third makes the cost of a custom annotation surface
strictly worse than convention:

| Rule | Why pipes can't ride a protocol |
|---|---|
| **No HKT** (Tier 1 #3, "Fast compilation") | `map`'s shape is `F[A] -> (A -> B) -> F[B]` — the type constructor `F` is bound, not a concrete type. Single-dispatch protocols require a concrete `Self` in first position; they cannot abstract over the head constructor. |
| **No effect rows in protocol ops** (§"With effects" rule above) | The function arg to `map` carries an effect row, and the row must propagate to `map`'s own row. Protocol op signatures are pinned to closed rows; the protocol ban on effect rows in op signatures rules out the canonical `map` shape. |
| **Annotation surface vs convention** | A `#[pipe_dispatch]` annotation costs a parser change, a new visible concept for ~5 stdlib + future package decls, and a test surface. Convention is free at the parser, costs ~70 LOC inside the typer, and mirrors how qualified-name resolution already works for `EModCall`-driven calls. |

Instead the typer keeps a per-compile **head-owner cache**:
`[(head_type_name, [declaring_module])]`, built once from the
post-`expand_imports` decl stream + module table. Any `pub type T`
exported by a module whose `pub fn map / flat_map / filter` follow
the canonical signatures participates in `|`, `||`, `|?`
automatically — no annotation, no compiler change. ahu's future
`Source[T, e]`, henua's `EventBus[E]`, manutara stream/sink types
opt in by declaring `pub type` + the canonical fns.

The canonical signatures (LHS receiver first, function/predicate
second, return wraps the same head type):

```text
pub fn map[A, B, e](xs: T[A], f: (A) -> B / e) : T[B] / e
pub fn flat_map[A, B, e](xs: T[A], f: (A) -> T[B] / e) : T[B] / e
pub fn filter[A, e](xs: T[A], p: (A) -> Bool / e) : T[A] / e
```

A head carrying an `(element, row)` parameter list — a streaming
carrier like `Stream[t, e]` (or ahu's `Source[t, e]`) whose effect
row is itself a type parameter — rides the pipes the same way. The
canonical shape becomes `map[A, B, e](xs: T[A, e], f: (A) -> B / e) :
T[B, e]`: the element unifies against the first slot, the row threads
through the trailing row-kind slot. Dispatch is by head *name* and is
arity-agnostic, so no extra opt-in is needed; the row slot threads via
a row carrier (`row_arg_carrier`), keeping the effect visible in the
type (the carrier is the fix for #773). The single-parameter `T[A]`
form is the special case where `T` is pure.

Diagnostic shape when conventions are violated (anchored at the
user's pipe call site):

| Cache state | Diagnostic |
|---|---|
| `HLNone` (no module declares the head) | `no module declaring type ` `T` ` is in scope` |
| `HLOk(mod)` but no `mod::op` qualified entry | `module ` `mod` ` declares ` `T` ` but does not export ` `op` |
| `HLAmbiguous([mod_a, mod_b])` | `ambiguous head type ` `T` ` — ` `mod_a` ` and ` `mod_b` ` all declare it. Use qualified type annotation.` |
| `mod::op` exists but signature does not unify | standard call-inference signature mismatch (the same diagnostic the explicit `mod.op(xs, f)` form would produce) |

Implementation: `build_head_owner_map` (`stage2/compiler.kai`) +
`find_head_owner` (linear lookup on the small cache list) +
`synth_pipe_dispatch` (the rewrite path). The seeded `List → list`
entry preserves the pre-#594 dispatch surface for `[T] | f` —
`core/list.kai` ships the canonical fns and the parser-stamped
`TyListT` flows through the same code path as a user `pub type`.
