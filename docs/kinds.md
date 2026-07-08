# The kind system

> Status: `Type` and `Measure` are the two *operational* kinds.
> The kind system is no longer closed: the `kind` / `theory`
> declaration surface is shipped (catalog in
> `stdlib/core/kinds.kai`), and the generalized engine — user
> kinds over `AbelianGroup`, per-kind isolation, use-site
> resolution — is in progress (#1108). The authoritative design
> is `docs/kind-system-design.md`; this page is the primer.

## What is a kind?

Kinds classify types the same way types classify values.

- A *value* `42` has the *type* `Int`.
- A *type* `Int` has the *kind* `Type`.

In most languages you only ever meet one kind (`Type`), so the
concept stays invisible. kaikai exposes a second kind so the
type system can keep units of measure separate from ordinary
types.

## The two operational kinds

| Kind      | Inhabits | Examples |
|-----------|----------|----------|
| `Type`    | ordinary types | `Int`, `String`, `Bool`, `[T]`, `Option[T]`, `Result[E, T]`, user records, user variants |
| `Measure` | units of measure | `USD`, `EUR`, `m`, `kg`, `m/s`, `m^2` |

There is no `Effect` kind, no `Row` kind, no higher-order kinds.
Effects and rows live in their own machinery (see
`docs/effects.md`); they do not show up at the kind level.

`Measure` is itself declared in the catalog —
`kind Measure : AbelianGroup with unit` in `stdlib/core/kinds.kai`
— rather than hardcoded; `unit m` mints its habitants. Further
kinds follow the same shape once the generalized engine lands.

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
```

The theory decides how the kind's habitants unify (`AbelianGroup`
gives Measure-style exponent algebra); the optional `with`
introducer mints habitants. Habitants in different kinds never
unify, and the same symbol may inhabit several kinds, resolved at
the use site by qualification > `use kind` > uniqueness — see
`docs/kind-system-design.md` § *Habitant resolution at the use
site*.

> **v1 status (2026-07-08):** the declaration surface is shipped;
> user kinds beyond the built-in `Measure` activate with the
> generalized engine (#1108, in progress). Candidate theories
> under design: `Structural` (identity — regions, #1123) and
> `Module` (currency/money — no internal product, so `USD^2`
> cannot arise).

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
