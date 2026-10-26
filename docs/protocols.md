# Protocols for kaikai

## Status: Landed

Stage 2 supports `protocol`, `impl`, and `#[derive(...)]`; the stdlib
protocols are defined in `stdlib/protocols.kai` (see *Stdlib
protocols* below). The compiler resolves dispatch at the AST
level (between inference and monomorphisation), so each statically-typed
call site emits a direct `__pimpl_<P>_<T>_<op>` call on both backends
(C and LLVM). A protocol's dispatch identity belongs to the module that
declares it: when two modules declare the same protocol name, each
non-core declaration mangles and keys its impls module-qualified
(`__pimpl_<mod>__<P>_<T>_<op>`), a bare call ranks the calling module's
own protocol first, and only protocols visible to the calling module
(own, `pub`, or root-declared) are dispatch candidates.

Single-dispatch ad-hoc polymorphism via explicit `protocol` declarations
and `impl` blocks. Modeled on Clojure protocols, Elixir protocols, Go
interfaces (with explicit declaration), and the lightweight subset of
Rust traits.

**Not Haskell typeclasses.** No higher-kinded types, no constraint
propagation in signatures, no functional dependencies, no type families,
no overlapping instances. Single-dispatch only.

This document is the spec; `kai info protocols` is the short reference.

## Motivation

Three concrete pain points protocols remove:

1. **Measured-value printing**: `real_to_string(x) ++ " USD"` on every
   interpolation. With `protocol Show` + `impl[u: Measure] Show for
   Real<u>`, `#{x}` renders the unit automatically.
2. **Generic equality / comparison / hashing**: without protocols every
   record / sum type needs ad-hoc `eq_<type>`, `hash_<type>`,
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

The stdlib's core protocols, as `stdlib/protocols.kai` declares them:

```kaikai
protocol Show {
  show(x: Self) : String
}

protocol Eq {
  eq(a: Self, b: Self) : Bool
}

protocol Ord {
  cmp(a: Self, b: Self) : Int
  min(a: Self, b: Self) : Self
  max(a: Self, b: Self) : Self
}

protocol Hash {
  hash(x: Self) : Int
}

fn main() : Unit = ()
```

`Self` is a reserved type-parameter referring to the type the protocol
is implemented for. An op declaration omits `fn` and has no body: a
protocol carries no default implementations, so every `impl` writes
every op.

### Implementation

```kaikai
type Point = { x: Int, y: Int }
type Tagged[T] = { tag: String, value: T }

impl Show for Point {
  fn show(p: Point) : String = "(#{p.x}, #{p.y})"
}

impl Eq for Point {
  fn eq(a: Point, b: Point) : Bool = a.x == b.x and a.y == b.y
}

impl[T] Show for Tagged[T] {
  fn show(t: Tagged[T]) : String = "<#{t.tag}>"
}

fn main() : Unit / Stdout = {
  Stdout.print(show(Point { x: 1, y: 2 }))
  Stdout.print(show(Tagged { tag: "id", value: 7 }))
  Stdout.print("#{eq(Point { x: 1, y: 2 }, Point { x: 1, y: 2 })}")
}
```

Impl ops use the full `fn name(params) : R = body` form. A polymorphic
impl (`impl[T] ... for Tagged[T]`) is monomorphised per concrete
instantiation, like any generic function.

### Polymorphic impls with bounded type parameters

A polymorphic impl whose body dispatches on the type variable —
`impl[T] Show for Bag[T]` calling `show(x : T)` on an element — needs
to know `T` itself implements `Show`. An impl-site bound makes that
requirement explicit:

```kaikai
type Bag[T] = { items: [T] }

impl[T : Show] Show for Bag[T] {
  fn show(b : Bag[T]) : String = match b.items {
    []           -> "Bag()"
    [x, ...rest] -> "Bag(" ++ show(x) ++ ", +" ++ show(length(rest)) ++ ")"
  }
}

fn main() : Unit / Stdout = Stdout.print(show(Bag { items: [1, 2, 3] }))
```

Bounds stack with `+`:

```kaikai
type Bag[T] = { items: [T] }

impl[T : Show + Eq] Show for Bag[T] {
  fn show(b : Bag[T]) : String = match b.items {
    [x, y, ..._] -> if x == y { "Bag(" ++ show(x) ++ " twice)" } else { "Bag(..)" }
    _            -> "Bag(..)"
  }
}

fn main() : Unit / Stdout = Stdout.print(show(Bag { items: [4, 4] }))
```

**Where bounds are allowed.** Protocol bounds appear on **impl-site**
type parameters (`impl[T : Eq] ... for ...`) and on
**free-fn** type parameters (`fn dedup[T : Eq](xs : [T])`,
`fn sum[T : Numeric](xs : [T])` — the stdlib aggregates use this).
They do **not** appear on `type`/record declarations: `type M[T : Add]`
is a parse error ("type-parameter kind must be `Type` or `Measure`").
The line that keeps single-dispatch protocols clear of Haskell
typeclasses / HKT / constraint propagation (Tier 1 #3) is *how* the
bound is honoured, not *where* it may be written: dispatch resolves
per monomorphised instance, never by a constraint that travels with
the signature. A free-fn bound is checked against every concrete type
the function is instantiated at, whether or not the body calls the op:
`count[T : Eq]` applied to a list of a type without `Eq` is rejected,
even when the call reaches it through an unbounded generic caller.

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
the required impl.

**Non-recursive polymorphic impls** (the body never dispatches a
protocol op on `T`) do not need a bound and continue to work as
before:

```kaikai
type Bag[T] = { items: [T] }

impl[T] Show for Bag[T] {
  fn show(b : Bag[T]) : String = "bag-of-something"   # OK, no dispatch on T
}

fn main() : Unit / Stdout = Stdout.print(show(Bag { items: [(x: Int) => x] }))
```

### Use sites

Protocol ops are called as ordinary functions:

```kaikai
#[derive(Show, Eq, Ord)]
type Point = { x: Int, y: Int }

fn main() : Unit / Stdout = {
  let p1 = Point { x: 1, y: 2 }
  let p2 = Point { x: 1, y: 3 }
  let s = show(p1)                    # dispatches to Show for Point
  let same = eq(p1, p2)               # dispatches to Eq for Point
  let order = cmp(p1, p2)             # dispatches to Ord for Point
  Stdout.print("#{s} #{same} #{order}")
  Stdout.print("p1: #{p1}")           # interpolation calls show(p1)
}
```

In string interpolation, `Show` is invoked implicitly when an
interpolation expression has a type that has `impl Show`:
`"p1: #{p1}"` expands at compile time to `"p1: " ++ show(p1)`.

If the type has no `impl Show`, the typer emits an explicit error
pointing at the missing impl — no silent fallback to bytes or
`<opaque>`.

### Coherence rules

**One impl per (protocol, type) pair.** If a second `impl Show for
Point` appears in the same compilation unit, the resolver emits a
duplicate-impl error.

**Orphan rule** (Rust-style): an `impl P[A1, ..] for T` is allowed
only when the current module declares **P**, the head of **T**, or
the head of some protocol argument **Ai**. `impl Add[Complex] for Real`
in the module declaring `Complex` is legal (see *Heterogeneous
arithmetic*); `impl Add[List[Complex]] for Real` is not, because the
argument's head is `List`. "Declares" means the declaration's home
module, never a same-spelled name. This prevents two foreign packages
defining conflicting impls on a type they both import.

Blanket impls (`impl[T] P[T] for X`) would additionally need Rust's
condition that no uncovered type parameter appears before the first
local type.

A builtin type (`Int`, `[a]`, `Option[a]`, ...) is declared by no
module, so it never makes an impl local: the protocol or a protocol
argument must.

The orphan rule keeps coherence local: the compiler can determine the
single applicable impl by searching only the modules transitively
imported. No global registry.

### What protocols cannot do (deliberately)

- **No constraint propagation**: a bound on a generic function
  (`fn sort[T : Ord](xs: [T]) : [T]`, as core declares it) is checked
  against each concrete `T` the function is instantiated at, and the
  op resolves to a direct call in that instance. No dictionary is
  passed and no constraint is inferred into a caller's signature;
  a caller that wants another order passes a comparator:

  ```kaikai
  #[derive(Ord)]
  type Tx = { day: Int, amount: Int }

  fn main() : Unit / Stdout = {
    let txs = [Tx { day: 3, amount: 5 }, Tx { day: 1, amount: 9 }]
    let by_day = sort(txs)                              # bound `T : Ord`, checked at the call
    let newest = sort_by(txs, (a, b) => cmp(b, a))      # explicit comparator
    Stdout.print("#{length(by_day)} #{length(newest)}")
  }
  ```

  This is the sharp distinction from Haskell typeclasses and the main
  reason compile times stay flat.

- **No higher-kinded types**: `protocol Functor[F[_]]` does not parse.
  Type parameters of protocols are first-order (`protocol P[a]`,
  lowercase per kaikai's tparam convention), and the only way `Self`
  is parametric is when the impl target is parametric (`impl[T] Show for
  Bag[T]`). `protocol P[a]` declares a single-dispatch parametrized
  protocol — the impl table key remains `(P, Self)`, so adding `[a]`
  does NOT introduce multi-method dispatch (see §*Single-dispatch
  parametrized protocols* below).

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
4. If zero impls are found, the typer emits "no impl of `P` for type
   `T`" at the call.
5. If more than one impl is found, the orphan rule was violated
   somewhere upstream — the resolver emits the conflicting modules.

The lookup is `O(1)` hash by `(P, T)` key. No constraint propagation,
no unification beyond what monomorphization already does.

### Runtime cost

Generic code is monomorphised, so every call site sees a concrete type
and the call is **direct** — a `__pimpl_<P>_<T>_<op>` function, no
vtable, no dictionary, no dynamic registry. Same cost as a regular
function call.

## Stdlib protocols

`stdlib/protocols.kai` declares:

| Protocol | Ops | Use cases |
|---|---|---|
| `Show` | `show(x: Self) : String` | interpolation, debug, logging |
| `Eq` | `eq(a: Self, b: Self) : Bool` | equality, set membership, dedup |
| `Ord` | `cmp(a: Self, b: Self) : Int`, `min`, `max` | sort, min, max, ordered containers |
| `Hash` | `hash(x: Self) : Int` | hash maps, hash sets, memoization |
| `Serialize` | `to_string(x: Self) : String`, `from_string(s: String) : Result[Self, String]` | text bindings via wrappers |
| `BinSerialize` | `to_bytes`, `from_bytes` | binary encoding |
| `Layout` | `to_bytes`, `from_bytes` | fixed binary layouts (`kai info kinds`) |
| `Json` | `to_json`, `of_json` | the JSON DOM (`#[derive(Json)]`) |
| `Default` | `default() : Self` | accumulators, container clearing, generic zero values |
| `Add[a]` `Sub[a]` `Mul[a]` `Div[a]` `Rem[a]` `Neg` | `add` … `neg` | operator overloading (see below) |

The primitives carry the impls that make sense for them; the
parametric containers (`[T]`, `Option[T]`, `Result[T, E]`, pairs,
triples, quads) carry `Show` / `Eq` / `Ord` / `Hash` under the matching
bound. User types `impl` or `#[derive(...)]` themselves. A builtin
never makes an impl local under the orphan rule (see *Coherence
rules*): wrap it in a type of your own.

### `Default`

```kaikai
protocol Default {
  default() : Self
}

impl Default for Int    { fn default() : Int    = 0 }
impl Default for Real   { fn default() : Real   = 0.0 }
impl Default for Bool   { fn default() : Bool   = false }
impl Default for String { fn default() : String = "" }
impl[T] Default for [T]       { fn default() : [T]       = [] }
impl[T] Default for Option[T] { fn default() : Option[T] = None }

fn main() : Unit = ()
```

Canonical defaults: numeric zero, false, empty string, empty list,
absent option. `Result[T, E]` is intentionally absent — neither
`Ok(default()) : Result[T, _]` nor `Err(default()) : Result[_, E]`
is a privileged choice, so callers construct results explicitly.

`default() : Self` has Self only in the return position, so
single dispatch resolves it the same way `Serialize.from_string` and
`From[a].from` do — from the expected type, such as a
`let x : T = default()` annotation. A generic helper carries the bound:

```kaikai
fn make[T : Default]() : T = default()

fn main() : Unit / Stdout = {
  let n : Int = default()
  let s : String = make()
  let xs : [Int] = default()
  Stdout.print("#{n} [#{s}] #{length(xs)}")
}
```

The polymorphic impls drop the `T : Default` bound — the bodies (`[]`,
`None`) are constants in `T` and never dispatch on it. The
Show/Eq/Ord/Hash container impls keep their bound because their bodies
do. `Default` is not derivable.

## Auto-derivation via `#[derive(...)]` annotation

For records and sum types whose fields all already have `impl P`, the
user can derive trivially:

```kaikai
#[derive(Show, Eq, Hash, Ord)]
type Account = { id: Int, owner: String, txs: Int }

#[derive(Show, Eq, Hash)]
type Shape = Circle(Real) | Rect(Real, Real) | Empty_

fn main() : Unit / Stdout = {
  let a = Account { id: 1, owner: "ana", txs: 3 }
  Stdout.print(show(a))
  Stdout.print(show(Rect(1.0, 2.0)))
  Stdout.print("#{eq(Circle(1.0), Circle(1.0))} #{hash(a) == hash(a)}")
}
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

`#[derive(...)]` is one of four declaration-level attributes. The
others are `#[unstable]`, which marks a `pub` declaration as outside
the edition stability contract (see `docs/editions.md`),
`#[doc("...")]`, which attaches documentation text to a declaration
(or, in module position, to the file), and `#[constructor]` (see
`kai info syntax` §Attributes). An unrecognised attribute parses and is
discarded.

### Equality and order without an impl

`Eq` has one definition: a type is `Eq` when it has an `impl Eq` (written
or derived), or, failing that, when it is a record or sum type whose
every field is `Eq`. `==`, `!=` and a `[T: Eq]` bound all ask that same
question, decided per concrete type at monomorphisation. A written impl
overrides the structural rule, so `#[derive(Eq)]` stays valid and is
redundant. A function type is not `Eq`, nor is a type that holds one
unless it carries its own impl; `f == f` is a compile error.

`Pair`, `Triple` and `Quad` implement `Ord` lexicographically when each
element does. A record or sum type has no automatic `Ord`; `<` on a
record without an `impl Ord` is a compile error.

`#[derive(...)]` is **opt-in**: if the user does not write the annotation, no
impl is generated. This avoids surprising user-defined `impl Show for
Account` getting shadowed by an auto-derived one.

A derive rejects at compile time when a field (or variant payload)
reaches a type that lacks the protocol — see *Field-impl validation*.

### Supported protocols and shapes

| Protocol | Records | Sum types | Notes |
|---|---|---|---|
| `Show` | ✓ | ✓ | renders `Type { f: v, ... }` and `Variant(arg, ...)` |
| `Eq`   | ✓ | ✓ | record fields conjoined with `and`; sum types validated against field-impl set |
| `Hash` | ✓ | ✓ | polynomial mix (`h * 31 + h_next`); sums validated against field-impl set |
| `Ord`  | ✓ | ✓ | lexicographic by declaration order; sums by variant position then payload |
| `Json` | ✓ | — | records only; see `kai info protocols` |

### Order semantics for `#[derive(Ord)]`

Records compare lexicographically in declaration order: the first
field whose `cmp` returns non-zero decides the result. Sum types
compare by **variant declaration position** first (variant 0 < variant
1 < ...); within the same variant, payloads compare lexicographically
by position. The synthesised `cmp` for

```kaikai
#[derive(Ord)]
type Tier = Bronze | Silver | Gold | Platinum

#[derive(Ord)]
type Shape = Circle(Int) | Square(Int)

fn main() : Unit / Stdout = {
  Stdout.print("#{cmp(Bronze, Silver)} #{cmp(Gold, Bronze)} #{cmp(Platinum, Platinum)}")
  Stdout.print("#{cmp(Circle(99), Square(0))}")
}
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

Before lowering a `#[derive(P)]` annotation, the compiler walks each
field (or variant payload) and confirms its type has `P` — an impl,
another derive, or a stdlib builtin. A missing impl is rejected at the
declaration with a diagnostic naming the field and its type:

```kaikai-neg
type Tag = { label: String }

#[derive(Ord)]
type Tagged = { id: Int, tag: Tag }

fn main() : Unit = ()
```

```text
error: cannot `#[derive(Ord)]` for `Tagged`: no `Ord` impl for field
`tag` of type `Tag` (declare `impl Ord for Tag` or `#[derive(Ord)]` on `Tag`)
```

## Composition with other features

### With units of measure

The stdlib's `impl[u: Measure] Show for Real<u>` renders a measured
value with its unit:

```kaikai
unit USD
unit EUR

fn main() : Unit / Stdout = {
  let usd = 100.5<USD>
  let eur = 89.75<EUR>
  Stdout.print("balances: #{usd}, #{eur}")   # balances: 100.5 USD, 89.75 EUR
}
```

The `u: Measure` parameter monomorphizes per unit at the call site;
`unit_name` provides the symbol.

### With refinements

```kaikai
type Email = String where self |> contains("@")

fn main() : Unit / Stdout = {
  let e : Email = "ana@example.com"
  Stdout.print(show(e))               # Show for String
}
```

Refined types `T where P` inherit `impl P` from their base type
unless the user provides a more specific impl.

### With effects

Protocols are **pure** — `impl P for T` ops cannot have effect rows.
If a use case needs effects (e.g. `print` to console), the operation
goes via an effect, not a protocol.

```kaikai-neg
protocol Audit {
  log(x: Self) : Unit / Stdout        # rejected: no effect row on a protocol op
}

fn main() : Unit = ()
```

The same holds for an impl: a method written `fn show(c: Counter) : String / Stdout`
is rejected, since dispatch would drop its row.

Use an effect, not a protocol, for behaviour with effects. This keeps
protocols compositional and predictable.

#### Why the rule (the four reasons)

The hard rule exists for four reasons, ordered by load-bearing:

1. **Dispatch resolves after inference.** A protocol call is
   rewritten to its impl after monomorphisation, but the call's effect
   row is needed during inference, before the impl is known. With
   per-impl rows, the row of a call would depend on a choice the typer
   has not made yet.
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

Proposed form; not implemented:

```text
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

Each impl declares its own row (proposed form; not implemented):

```text
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

Proposed form; not implemented:

```text
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
kaikai ships **A** (no effects in protocols). Reopening requires a
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

```kaikai
type IdentityError = AccountNotFound | KycExpired
type AuthError     = InsufficientBalance | OverDailyLimit
type QueryErr      = IdentityError | AuthError

impl Show for IdentityError {
  fn show(e: IdentityError) : String = match e {
    AccountNotFound -> "account not found"
    KycExpired      -> "kyc expired"
  }
}

impl Show for AuthError {
  fn show(e: AuthError) : String = match e {
    InsufficientBalance -> "insufficient balance"
    OverDailyLimit      -> "over daily limit"
  }
}

fn render(err: QueryErr) : String = match err {
  ie : IdentityError -> show(ie)     # dispatches on IdentityError
  ae : AuthError     -> show(ae)     # dispatches on AuthError
}

fn main() : Unit / Stdout = Stdout.print(render(KycExpired))
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

#### B. Single-dispatch parametrized — `protocol P[a]` (chosen)

```kaikai
protocol From[a] {
  from(x: a) : Self
}

type Usd = { cents: Int }
type Eur = { cents: Int }

impl From[Usd] for Eur {
  fn from(m: Usd) : Eur = Eur { cents: m.cents * 92 / 100 }
}

impl From[Eur] for Usd {
  fn from(m: Eur) : Usd = Usd { cents: m.cents * 109 / 100 }
}

fn main() : Unit / Stdout = {
  let my_usd = Usd { cents: 1000 }
  let eur : Eur = from(my_usd)        # Self resolved by annotation
  Stdout.print("#{eur.cents}")
}
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
- **When right**: most fintech use cases. The chosen path; see
  §*Single-dispatch parametrized protocols*.

#### C. Multi-method dispatch — `protocol P { op(a: A, b: B) }` real

Rejected form; not implemented:

```text
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

Rejected form; `TypeOf` does not exist:

```text
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

Until one of these manifests, C remains rejected. B is the path
forward.

Units of measure cover the cases where the multi-type dispatch is at
the *value-arithmetic* level (`Real<USD> + Real<USD>` vs
`Real<USD> + Real<EUR>`).

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

## Single-dispatch parametrized protocols

`protocol P[a]` declares one or more type parameters (alongside the
implicit `Self`) bound at impl-site — `From[a]` above is one.

Surface contract:

- **Tparam names**: lowercase (`a`, `other`, `ctx`, …). Uppercase
  forms (`A`, `Other`) are valid syntax but are rejected later by
  the implicit-tparam classifier — keep to lowercase.
- **Impl-site arguments are concrete**. Polymorphism in `a`
  (`impl[T] From[T] for X`) is not supported.
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
**expected type** — a let-binding annotation or a function's declared
return type:

```kaikai
protocol From[a] {
  from(x: a) : Self
}

type Usd = { cents: Int }
type Eur = { cents: Int }

impl From[Usd] for Eur {
  fn from(m: Usd) : Eur = Eur { cents: m.cents * 92 / 100 }
}

fn to_eur(u: Usd) : Eur = from(u)          # Self pinned by the return type

fn main() : Unit / Stdout = {
  let eur : Eur = from(Usd { cents: 500 }) # Self pinned by the annotation
  Stdout.print("#{eur.cents} #{to_eur(Usd { cents: 1000 }).cents}")
}
```

An unannotated `let eur = from(my_usd)` has no Self informant.

The unification step pins the call's return type from the expected
type; the post-inference rewrite (`try_rewrite_proto_call_with_ret`)
then reads that pinned type and dispatches accordingly.

### Heterogeneous arithmetic

`Add[a] for X` lets `synth_binop` route across types:

`stdlib/math/complex.kai` declares `impl Add[Real] for Complex` and
`impl Add[Complex] for Real`, so both operand orders type-check:

```kaikai
import math.complex

fn main() : Unit / Stdout = {
  let z = 2.0 + 3.0i                  # Real + Complex
  let w = Complex(0.0, 3.0) + 2.0     # Complex + Real
  Stdout.print("#{z.re} #{z.im}")
  Stdout.print("#{w.re} #{w.im}")
}
```

`synth_binop` consults a snapshot of the post-`lower_protocols` impl
table (carried in `InferState.protos`) and routes to
`__proto_<op>` whenever a (lhs, rhs) pair has a registered impl
(homogeneous, prim/nonprim, or two distinct nonprim heads). When no
impl exists the binop falls through to the standard mismatch
diagnostic.

The expression has the type the selected impl returns, in either
operand order: `2.0 + z` is a `Complex`, like `z + 2.0`.

### Out of scope

- Polymorphic impl in `a` (`impl[T] From[T] for X`).
- Multi-method dispatch (`(P, T1, T2, ...) -> fn`) — Tier 1 #3 commit.
- `#[derive(...)]` synthesis for parametrised protocols.

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
| unary `-` | `Neg` | `neg` | `neg(self: Self) : Self`; no protocol argument |

Primitive `Int <op> Int` and `Real <op> Real` arithmetic for `+`,
`-`, `*`, `/` stays on the unify-and-emit fast path — the typer
rewrites to the protocol method only when both operands resolve to
a concrete non-primitive type (or when the heterogeneous routing
matches a registered impl). kaikai has no separate integer-division
operator: `Int / Int` truncates.

Unary `-` follows the same split: `Int`, `Real`, the fixed-width
integers and a dimensioned primitive (`Real<m>`) negate on the fast
path, and any other operand type calls `Neg.neg`. A type with no `impl
Neg` is rejected at the operator (`no impl of `Neg` for type `T``).
Inside a generic body the operand is still a type variable, so the
negation resolves once monomorphisation makes it concrete; `[T: Neg]`
is the bound that checks it at the call.

`%` is the exception: `Real % Real` always routes through `Rem.rem`
because stage0's primitive `%` is Int-only. The `Real` impl
delegates to the `real_rem` libm binding (`fmod`), so `r1 % r2`
returns the IEEE-754 remainder — `5.5 % 2.0 == 1.5`. NaN propagates
on a zero divisor (`a % 0.0` is NaN, matching the rest of the libm
surface from issue #343); inspect with `real_is_nan` at
boundaries.

### Pipe operators are convention-based, not protocol-based (#594)

`|` (map pipe), `||` (flat-map pipe), `|?` (filter pipe), and `|!` (each pipe) do
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
exported by a module whose `pub fn map / flat_map / filter / each`
follow the canonical signatures participates in `|`, `||`, `|?`, `|!`
automatically — no annotation, no compiler change. ahu's future
`Source[T, e]`, henua's `EventBus[E]`, manutara stream/sink types
opt in by declaring `pub type` + the canonical fns.

The canonical signatures (LHS receiver first, function/predicate
second, return wraps the same head type):

```text
pub fn map[A, B, e](xs: T[A], f: (A) -> B / e) : T[B] / e
pub fn flat_map[A, B, e](xs: T[A], f: (A) -> T[B] / e) : T[B] / e
pub fn filter[A, e](xs: T[A], p: (A) -> Bool / e) : T[A] / e
pub fn each[A, e](xs: T[A], f: (A) -> Unit / e) : Unit / e
```

`|!` checks its stage's return against `Unit` at the pipe and types the
pipe as `Unit`, whatever the export declares.

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

Implementation: `build_head_owner_map`
(`stage2/compiler/infer_head_dispatch.kai`) +
`find_head_owner` (linear lookup on the small cache list) +
`synth_pipe_dispatch` (the rewrite path). The seeded `List → list`
entry preserves the dispatch surface for `[T] | f` —
`stdlib/core/list.kai` ships the canonical fns and the parser-stamped
`TyListT` flows through the same code path as a user `pub type`.
