# The kind system

> Status: the catalog (`stdlib/core/kinds.kai`) declares four
> kinds — `Type` and `Effect` (builtin, closed), `Measure`
> (`AbelianGroup`), and `Currency` (`Module`) — with `Structural`
> as a candidate theory under design. The `kind` / `theory`
> declaration surface and the generalized engine (user kinds over
> `AbelianGroup` or `Module`, per-kind isolation, use-site
> resolution, cross-kind bind rejection) are shipped. The
> authoritative design is `docs/kind-system-design.md`; this page
> is the primer.

## What is a kind?

Kinds classify types the same way types classify values.

- A *value* `42` has the *type* `Int`.
- A *type* `Int` has the *kind* `Type`.

In most languages you only ever meet one kind (`Type`), so the
concept stays invisible. kaikai exposes a second kind so the
type system can keep units of measure separate from ordinary
types.

## The catalog kinds

Every kind is declared in the catalog over a **theory** — the
unification discipline that decides how its habitants unify:

| Kind      | Theory | Engine | Introducer | Inhabits |
|-----------|--------|--------|------------|----------|
| `Type`    | `HindleyMilner` | builtin (the compiler's HM unifier) | `type` | ordinary types: `Int`, `String`, `[T]`, `Option[T]`, user records and variants |
| `Effect`  | `EffectRow` | builtin (the compiler's row-unification) | `effect` | effect labels: `Stdout`, `State`, `Spawn` |
| `Measure` | `AbelianGroup` | hand-written abelian unifier | `unit` | units of measure: `m`, `kg`, `m/s`, `m^2` |
| `Currency` | `Module` | abelian unifier + formation guard | `currency` | currencies: `USD`, `EUR` (habitants ship in `stdlib/money.kai`) |

A `builtin` theory (`theory HindleyMilner = builtin`) names an
engine that is the compiler core itself rather than a property
set with a hand-written `unify_<theory>`. `Type` and `Effect` are
**closed**: the language provides them, and user code can neither
redeclare them nor declare a new `builtin` theory. The catalog is
declarative surface — HM and row-unification run exactly where
they always did, not through a kind dispatcher.

The introducer column is the symmetry that makes the catalog the
whole truth: `type Foo = ...` mints a habitant of `Type` the same
way `effect Stdout { ... }` mints one of `Effect` and `unit m`
mints one of `Measure`.

Effect *rows* (the `/ Console + State` part of a signature) stay
in their own machinery (see `docs/effects.md`); `Effect` classifies
the labels, and only unit-shaped kinds appear in tparam
annotations (`[u: Measure]`, `[c: Currency]`). Candidate theory
still under design: `Structural` (identity — regions).

### The `Module` theory

`theory Module = { assoc, commut, inverse, identity }` — an
additive abelian group with **no multiplicative closure**. A
well-formed quantity is either a scalar or carries exactly one
habitant with exponent 1, so `USD^2`, `USD*EUR`, and `1/USD` are
rejected where they would be *formed* (`*`, `/`, `^`, or a written
annotation) rather than by a separate unifier: on well-formed
inputs the abelian engine already behaves as symbol equality plus
variable binding. Scalar multiplication (`Money * Decimal`) is the
module's external action and lives in protocols, not in the kind
engine. Same-habitant addition unifies; a cross-habitant sum is an
ordinary unit mismatch.

A habitant binds a unit tparam only within its own kind: passing
`Real<USD>` to `fn sq[u: Measure]` is a type error — without that
check the generic body would mint `USD^2` behind the guard's back.

## Kind annotation syntax

Type parameters carry an optional `: Kind` annotation inside the
brackets. The default is `Type`, so most code never names a kind:

```kai
fn map[a, b](xs: [a], f: (a) -> b) : [b]
# expands to: map[a: Type, b: Type]
```

When a parameter ranges over units, the annotation is mandatory:

```kai
fn area[u: Measure](w: Real<u>, h: Real<u>) : Real<u^2>
fn divide[u: Measure, v: Measure](a: Real<u>, b: Real<v>) : Real<u/v>
```

`impl` blocks may carry the same annotation:

```kai
impl[u: Measure] Show for Real<u> {
  fn show(x: Real<u>) : String = ...
}
```

The grammar is mechanical: after a tparam name, an optional
`: <KindName>` follows. Mixing kinds in a single tparam list is
allowed:

```kai
fn convert[t: Type, u: Measure](x: t, factor: Real<u>) : Real<u> = ...
```

The compiler keeps type-vars and measure-vars in separate ID
namespaces internally; users never have to think about it.

## The three things called `Unit`

This is the disambiguation note that motivated #253. Three
different things share or share-derive the spelling `Unit`:

| Spelling   | What it is                              | Example |
|------------|------------------------------------------|---------|
| `Unit`     | the void return type (like `()` in ML) | `fn log(msg: String) : Unit / Console = ...` |
| `unit`     | a top-level keyword that declares a unit of measure | `unit USD`, `unit kg` |
| `Measure`  | the kind for unit-of-measure type parameters | `[u: Measure]` |

Before #253 the kind was also called `Unit`, which made
signatures like `fn log_price[u: Unit](p: Real<u>) : Unit` have
two unrelated `Unit` tokens with two unrelated meanings. The
kind was renamed to `Measure` (echoing F#'s `[<Measure>]`
attribute) so each role gets a distinct spelling.

The lowercase/uppercase distinction separates `unit` (the
keyword) from `Unit` (the type) — kaikai keywords are lowercase,
type names are uppercase. The remaining ambiguity lived between
`Unit` (type) and `Unit` (kind), which was always disambiguated
only by syntactic position (return slot vs `[]` annotation).
After #253 the kind has its own name and the ambiguity is gone.

## Declaring a new kind

A kind is declared over a **theory** — a unification discipline
drawn from a closed catalog (`stdlib/core/kinds.kai`; only the
catalog may declare theories):

```kai
kind Metric : AbelianGroup with metric   # user kind, own habitants
metric m
metric s

kind Points : Module with points         # additive-only user kind
points pt
```

The theory decides how the kind's habitants unify (`AbelianGroup`
gives Measure-style exponent algebra; `Module` is additive-only —
no habitant products or powers); the optional `with` introducer
mints habitants. Habitants in different kinds never unify, and the
same symbol may inhabit several kinds, resolved at the use site by
qualification > `use kind` > uniqueness — see
`docs/kind-system-design.md` § *Habitant resolution at the use
site*.

`Type` and `Effect` are **not** user-declarable. Only kinds over
the open catalog theories (`AbelianGroup`, `Module`) can be
declared; a `kind Type : ...` redeclaration, a user
`theory X = builtin`, and a user kind over a builtin theory
(`kind Foo : EffectRow`) are all hard errors.

## Cross-references

- `docs/kind-system-design.md` — the authoritative design of the
  kind/theory system: catalog, introducers, habitant resolution,
  `over`, the practical mode.
- `docs/units-of-measure.md` — full UoM design; defines the
  `Measure` kind formally and gives the abelian-group unification
  algorithm.
- `docs/protocols.md` § "With UoM" — how `impl` blocks use the
  `Measure` kind.
- `docs/effects-stdlib.md` — uses `Unit` (the type) extensively
  as the return slot; no `Measure` references because effects
  are kind-orthogonal.
- `stage2/compiler.kai` § `parse_optional_kind_annotation` — the
  parser site that recognises `Measure` and produces the
  targeted diagnostic when a user writes `Unit` in kind position.
