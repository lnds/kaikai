# kinds

Kinds classify types the way types classify values — one closed
catalog of unification theories, each with a decidable engine.

## Description

A *value* `42` has a *type* `Int`; a *type* `Int` has a *kind*
`Type`. Most languages expose only `Type`, so the concept stays
invisible. kaikai exposes more kinds so the type system can keep
units, currencies, memory regions, and binary layouts separate from
ordinary types — each with its own compile-time algebra, all erased at
runtime.

Three words carry the whole system:

- A **theory** is a *decidable unification algebra* — one hardcoded
  engine per theory, drawn from a closed catalog. It decides type
  equality at compile time and is erased at runtime.
- A **kind** is a name *classified by a theory* (`Measure` is
  classified by `AbelianGroup`). A kind's members are its
  **habitants**.
- The **habitant→theory contract**: each theory says what it needs
  from a habitant. `AbelianGroup` treats habitants as opaque symbols
  it multiplies and divides; `Composition` asks each habitant for a
  measure it can sum. A habitant fulfils the contract by its
  declaration.

Theories are a *closed* catalog because E-unification over arbitrary
equational theories is undecidable. The compiler owns a terminating
engine for each recognised theory; there is no way to write the
equations of a new one. This is the same discipline that forbids
Haskell-style type-class resolution (CLAUDE.md Tier 1 #3).

## The catalog

Every kind is declared in `stdlib/core/kinds.kai` over a theory:

| Kind | Theory | Habitants |
|---|---|---|
| `Type` | `HindleyMilner` | ordinary types (`Int`, `[T]`, records, sums) |
| `Effect` | `EffectRow` | effect labels (`Stdout`, `State`, `Spawn`) |
| `Measure` | `AbelianGroup` | units of measure (`m`, `kg`, `m/s`) |
| `Currency` | `Module` | currencies (`USD`, `EUR`) |
| `Region` | `Structural` | memory arenas (each `region { r -> }` mints one) |
| `Layout` | `Composition` | byte-order modifiers (`be`, `le`) |
| `Shape` | `Structural` | arity-1 constructors (`List`, `Vec`, `Option`, user `Tree[a]`) |

`Type` and `Effect` are `builtin`: their engine is the compiler core
itself (HM unification, row unification), and they are closed — user
code can neither redeclare them nor declare a new `builtin` theory.

## The theories

- `AbelianGroup = { assoc, commut, inverse, identity }` — habitants
  multiply and divide (`m·s`, `m/s²`). The engine solves linear
  integer equations to unify unit variables.
- `Module = { assoc, commut, inverse, identity }` used additively:
  habitants add within one kind but have no product, so `USD²` and
  `USD·EUR` are rejected at formation. Scalar multiplication
  (`Money · Decimal`) is external, a protocol impl.
- `Structural = builtin` — identity alone: two habitants unify iff
  they are the same symbol. No product, no sum. The engine for
  regions, where each arena is distinct.
- `Composition = { assoc, measure }` — composes elements in
  declaration order (associative, **not** commutative — order is
  load-bearing) and sums a per-element measure. A habitant is a
  single identity symbol, so `be·le` and `be²` are rejected at
  formation, exactly the `Module`/`Structural` shape.

## Units — the `Measure` kind

```kaikai
unit m
unit s

fn speed(d: Real<m>, t: Real<s>) : Real<m/s> = d / t

fn main() : Unit / Stdout = {
  let v = speed(100.0<m>, 9.5<s>)      # Real<m/s>
  Stdout.print("ok")
}
```

## Layout — the `Composition` kind

`Layout` classifies the binary representation of a fixed-width field.
The width comes from the base type (`U32` is 4 bytes); the `<be>` /
`<le>` habitant is the byte-order modifier. `U32<be>` and `U32<le>`
are the same base type but distinct representations, so they never
unify:

```kaikai
type Header = {
  magic: U32<be>,        # big-endian, network order
  port:  U16<be>,
  flags: U16<le>,        # little-endian
}

fn main() : Unit / Stdout = {
  let h = Header { magic: 0<be>, port: 0<be>, flags: 0<le> }
  Stdout.print("ok")
}
```

A big-endian value cannot be passed where little-endian is expected —
the habitant rides the type and the typer keeps them apart:

```kaikai-neg
fn take_be(x: U32<be>) : U32<be> = x

fn main() : Unit / Stdout = {
  let le_val : U32<le> = 5<le>
  let bad = take_be(le_val)            # U32<le> ≠ U32<be>
  Stdout.print("no")
}
```

A `Composition` habitant stands alone: it has no product or power, so
a composed annotation is rejected at the spot that writes it:

```kaikai-neg
fn bad(x: U32<be^2>) : Int = 0        # be^2 does not exist
fn main() : Unit / Stdout = Stdout.print("no")
```

## Shape — the kind of arity-1 constructors

`Shape` classifies **arity-1 type constructors** — `List`, `Vec`,
`Option`, or a user `Tree[a]`. Its habitants are *derived*: every
`type T[a] = ...` of exactly one type parameter is automatically a
`Shape` habitant (its bare constructor `T`), so `Shape` takes no
`with` introducer. `Structural` gives identity — two shapes unify iff
they are the same constructor symbol.

A `[s: Shape]` type parameter is applied as `s[A]`, letting a protocol
quantify over the container itself — the expressiveness a functor
gives, without HKT's cost. `s[A] ~ List[Int]` unifies first-order
(`s := List, A := Int`); after monomorphisation the dispatch is an
`O(1)` static call, no runtime indirection:

```kaikai
protocol Container[s: Shape] {
  first(xs: s[Int]) : Int
}

type Box[a] = Box(a)

impl Container for Box {
  fn first(xs: Box[Int]) : Int = match xs {
    Box(v) -> v
  }
}

impl Container for List {
  fn first(xs: [Int]) : Int = match xs {
    []          -> 0
    [h, ..._t]  -> h
  }
}

fn main() : Unit / Stdout = {
  Stdout.print(int_to_string(first(Box(7))))
  Stdout.print(int_to_string(first([3, 4, 5])))
}
```

A shape is atomic: it applies to exactly one type. Arity-2 (`Map`) is
not a `Shape`, and composition `s[t[..]]` (two stacked shape variables)
is rejected at formation — the cases that would reintroduce
higher-order unification:

```kaikai-neg
fn bad[s: Shape, t: Shape](xs: s[t[Int]]) : Int = 0   # composition
fn main() : Unit / Stdout = Stdout.print("no")
```

A Shape protocol may declare a **theory** in its header — reusing the
`kind K : Theory` spelling — to state which laws its ops obey. A
`Sequence[s: Shape] : Functorial` names the closed `Functorial` theory
(`identity`, `fusion`); `kai check` / `kai test` then autogenerate
property checks per impl, and a `map`-into-`foldl` pipeline over any
lawful container fuses to a single traversal. An impl asserts
`axiom Functorial` after its body to opt out of the checks:

```kaikai
protocol Sequence[s: Shape] : Functorial {
  map(xs: s[Int], f: (Int) -> Int) : s[Int]
  foldl(xs: s[Int], init: Int, f: (Int, Int) -> Int) : Int
}

type Chain[a] = Empty | Link(a, Chain[a])

impl Sequence for Chain {
  fn map(xs: Chain[Int], f: (Int) -> Int) : Chain[Int] = match xs {
    Empty      -> Empty
    Link(h, t) -> Link(f(h), map(t, f))
  }
  fn foldl(xs: Chain[Int], init: Int, f: (Int, Int) -> Int) : Int = match xs {
    Empty      -> init
    Link(h, t) -> foldl(t, f(init, h), f)
  }
}

fn main() : Unit / Stdout = Stdout.print("ok")
```

## User-declared kinds

`Measure`, `Currency`, and `Layout` are not special — declare your own
kind over any public theory and its habitant introducer word. An
abelian kind (isolated from `Measure`, so `metric` and `imperial`
never mix):

```kaikai
kind Metric   : AbelianGroup with metric
kind Imperial : AbelianGroup with imperial

metric m
imperial ft

fn area[u: Metric](w: Real<u>, h: Real<u>) : Real<u^2> = w * h

fn main() : Unit / Stdout = {
  let a = area(3.0<m>, 4.0<m>)         # Real<m^2>, Metric kind
  Stdout.print("ok")
}
```

A kind over `Composition` — the public theory `Layout` is built on —
gets the summed-measure algebra and the atomic-habitant guard:

```kaikai
kind Frame : Composition with frame

frame hdr
frame body

fn main() : Unit / Stdout = Stdout.print("ok")
```

## Worked example — a `Waterfall` kind (design sketch)

`Composition` being public means a user can build the
*measure-in-the-habitant* shape too — a fintech waterfall where a
CDO's tranches must sum to 100%, verified in the type. The habitant
carries its measure and `Composition` sums it. This form (habitant
`= N`) is a design sketch, not shipped surface:

```text
kind Waterfall : Composition with pct
pct senior   = 70          # each habitant declares its measure
pct mezz     = 20
pct equity   = 10          # Composition checks the sum closes to 100
```

Because the measure must verify a total exactly, `Composition`
measures are integer or rational, never float: `0.1 + 0.2 ≠ 0.3` in
IEEE-754 would fail a legitimate waterfall on a rounding artefact.

## See also

- `kai info units` — the `Measure` kind in full.
- `docs/kinds.md` — the kind-system primer.
- `docs/kind-system-design.md` — theories, habitant resolution,
  the closed property menu.
- `docs/layout-kind-design.md` — the `Layout` spec (encode/decode,
  TLV, nesting).
