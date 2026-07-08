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

A symbol may live in more than one kind (`unit USD` *and* `cur USD` from
`kind Currency : AbelianGroup with cur`); they are distinct habitants
that share the spelling `USD`.

### Resolving a shared symbol

A bare `<USD>` that lives in several kinds resolves by a fixed
precedence — never order-of-declaration:

1. **Qualification** — `<Currency.USD>` (or `<cur.USD>`, the introducer
   form) names the kind explicitly.
2. **`use kind Currency`** — a file-level default for bare habitants,
   the same `use` that opens an effect's ops.
3. **Unique** — a symbol in exactly one kind needs no qualification.

If none apply, a bare ambiguous `<USD>` is a compile error demanding
disambiguation.

```kaikai
kind Currency : AbelianGroup with cur
unit USD                                    # USD in Measure
cur  USD                                    # USD also in Currency
use kind Currency

fn main() : Unit / Stdout = {
  let a : Real<Currency.USD> = 10.0<Currency.USD>   # qualified → Currency
  let b : Real<USD>          = 10.0<USD>            # bare → Currency (used)
  Stdout.print("ok")
}
```

Because money and physics can share one abelian kind, `USD/kWh` (price
per energy) type-checks when both inhabit `Measure`: `(USD/kWh)·kWh = USD`.

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
