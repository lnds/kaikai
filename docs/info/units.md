# units

Units of measure on `Real` — phantom-type discipline, zero runtime
cost.

## Description

Units of measure give you dimensional checking at compile time at
zero runtime cost. `unit` is its own top-level declaration form. Units
inhabit the built-in `Measure` kind — an *abelian kind*, i.e. one whose
habitants multiply and divide (`m·s`, `m/s²`). `unit` is `Measure`'s
habitant introducer. Most code never names a kind — the annotation only
appears when a tparam ranges over units (`[u: Measure]`).

## Declaring and using

```kaikai
unit m
unit s
unit kg

fn distance(speed: Real<m/s>, time: Real<s>) : Real<m> = speed * time
fn area(w: Real<m>, h: Real<m>) : Real<m^2> = w * h

fn main() : Unit / Stdout = {
  let g = 9.81<m/s^2>                           # literal with unit
  let t = 2.0<s>
  let v = g * t                                 # inferred Real<m/s>
  let d = distance(v, t)
  let a = area(3.0<m>, 4.0<m>)
  Stdout.print("ok")
}
```

## Polymorphism over units

```kaikai
unit m
unit s

fn area_of[u: Measure](w: Real<u>, h: Real<u>) : Real<u^2> = w * h

fn main() : Unit / Stdout = {
  let a1 = area_of(3.0<m>, 4.0<m>)              # Real<m^2>
  let a2 = area_of(3.0<s>, 4.0<s>)              # Real<s^2>
  Stdout.print("ok")
}
```

`[u: Measure]` is the kind annotation — required because `u` ranges
over units, not types.

## User-declared kinds

`Measure` is not special — declare your own abelian kind and its
introducer word:

```kaikai
kind Metric   : AbelianGroup with metric
kind Imperial : AbelianGroup with imperial

metric m
metric s
imperial ft

fn area_of[u: Metric](w: Real<u>, h: Real<u>) : Real<u^2> = w * h

fn main() : Unit / Stdout = {
  let ok = 3.0<m> + 2.0<m>           # same kind → Real<m>
  Stdout.print("ok")
}
```

Two habitants in **different** kinds never unify — `metric m + imperial ft`
is a compile-time error, the Mars-Orbiter class of bug:

```kaikai-neg
kind Metric   : AbelianGroup with metric
kind Imperial : AbelianGroup with imperial

metric m
imperial ft

fn bad(a: Real<m>, b: Real<ft>) : Real<m> = a + b   # Metric vs Imperial

fn main() : Unit / Stdout = { Stdout.print("unreachable") }
```

A symbol may live in more than one kind (`unit pt` *and* `fiat pt` from
`kind Fiat : AbelianGroup with fiat`); they are distinct habitants
that share the spelling `pt`.

### Resolving a shared symbol

A bare `<pt>` that lives in several kinds resolves by a fixed
precedence — never order-of-declaration:

1. **Qualification** — `<Fiat.pt>` (or `<fiat.pt>`, the introducer
   form) names the kind explicitly.
2. **`use kind Fiat`** — a file-level default for bare habitants,
   the same `use` that opens an effect's ops.
3. **Unique** — a symbol in exactly one kind needs no qualification.

If none apply, a bare ambiguous `<pt>` is a compile error demanding
disambiguation.

```kaikai
kind Fiat : AbelianGroup with fiat
unit pt                                     # pt in Measure
fiat pt                                     # pt also in Fiat
use kind Fiat

fn main() : Unit / Stdout = {
  let a : Real<Fiat.pt> = 10.0<Fiat.pt>     # qualified → Fiat
  let b : Real<pt>      = 10.0<pt>          # bare → Fiat (used)
  Stdout.print("ok")
}
```

Because money and physics can share one abelian kind, `USD/kWh` (price
per energy) type-checks when both inhabit `Measure` (`unit USD`):
`(USD/kWh)·kWh = USD`.

## Module kinds — `Currency` and money

`Module` is the additive-only theory: habitants unify by nominal
atom equality, add and subtract within one kind, and scale by a bare
number, but have **no products or powers** — `USD^2`, `USD*EUR`, and
`1/USD` are compile errors at the operator or annotation that would
form them. The catalog declares
`kind Currency : Module over T with currency`; the habitants (`USD`,
`EUR`, ...) ship in `stdlib/money.kai`, where `Money[t]<c>` is a
carrier `t` (`Decimal`, `BigInt`, `Int`, ...) tagged with currency
`c`:

```kaikai
import money
import decimal as dec
import decimal_proto

fn main() : Unit / Stdout = {
  let a: Money[dec.Decimal]<USD> = 10.50<USD>
  let b: Money[dec.Decimal]<USD> = 4.50<USD>
  let total = a + b                  # same currency → Money[Decimal]<USD>
  let k: dec.Decimal = 3
  let scaled = total * k             # scalar action → Money[Decimal]<USD>
  Stdout.print(money.to_string(scaled))
}
```

A money literal takes its carrier from the annotation (`10.50<USD>`
alone is `Real<USD>`), so bind constants with a `Money[...]<...>`
annotation before operating on them. The carrier takes no protocol
bound: `Money[String]<USD>` constructs, and arithmetic on it is
rejected where the concrete instance meets a numeric operator.

`Money[t]<USD> + Money[t]<EUR>` is a type mismatch;
`Money[t]<USD> * Money[t]<USD>` cannot be formed. Cross-currency
conversion is an explicit door: `money.convert(m, rate)` with the
target currency pinned by annotation. Declaring your own additive
kind works the same way (`kind Points : Module with points`).

## Caveats

- `^` in unit expressions (`Real<m^2>`, `<m/s^2>`) denotes a unit-level
  power and the exponent is a literal integer. The same `^` also works
  as an expression-level power operator (`x ^ n` with `n: Int`) — see
  `kai info syntax`. The unit rule and the expression rule line up:
  `r : Real<m>` and `r ^ 3 : Real<m^3>` only when `3` is a literal.
- Adding two values requires matching units: `1.0<m> + 1.0<s>` is
  a type error. Multiplying multiplies units: `1.0<m> * 1.0<s> :
  Real<m*s>`.
- Unit annotations apply only to numeric heads: `Real<m>`, `Int<m>`,
  and `Decimal<m>` parse and typecheck. `String<m>` or `Bool<m>`
  are parse errors.

## NOT IN KAIKAI

- Unit conversions as language built-ins. Write a conversion fn:
  `fn ft_to_m(x: Real<ft>) : Real<m> = x * 0.3048<m/ft>`.
- F#-style unit suffixes without angle brackets — `9.81<...>` is
  required; `9.81m_s_2` is not a thing.

## See also

`kai info syntax`, `docs/kinds.md`
