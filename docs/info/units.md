UNITS(7)                        kaikai                        UNITS(7)

NAME
  units — units of measure on Real

SYNOPSIS
  unit m                                  # declare a base unit
  unit s
  Real<m>                                 # in a type
  Real<m/s>                               # quotient
  Real<m^2>                               # power
  Real<m*kg/s^2>                          # composite
  9.81<m/s^2>                             # in a literal

DESCRIPTION
  Units of measure are a phantom-type discipline on `Real`. They give
  you dimensional checking at compile time at zero runtime cost.

  `unit` is its own top-level declaration form. Units inhabit the
  `Measure` kind (kaikai has two kinds: `Type` and `Measure`). Most
  code never names a kind — the annotation appears only when a tparam
  ranges over units.

DECLARING

  unit m         # metre
  unit s         # second
  unit kg        # kilogram

USING IN TYPES

  fn distance(speed: Real<m/s>, time: Real<s>) : Real<m> =
    speed * time

  fn area(w: Real<m>, h: Real<m>) : Real<m^2> = w * h

USING IN LITERALS
  Unit-annotated numeric literals use angle brackets immediately after
  the number:

    let g = 9.81<m/s^2>
    let t = 2.0<s>
    let v = g * t                          # inferred Real<m/s>

POLYMORPHISM OVER UNITS

  fn area_of[u: Measure](w: Real<u>, h: Real<u>) : Real<u^2> = w * h

  area_of(3.0<m>, 4.0<m>)                  # → 12.0<m^2>
  area_of(3.0<s>, 4.0<s>)                  # → 12.0<s^2>

  `[u: Measure]` is the kind annotation — required because `u` ranges
  over units, not types.

CAVEATS
  - `^` ONLY appears inside `<...>` (units). `2^10` does not lex as
    power in expressions; use `int_pow(2, 10)`.
  - Adding two values requires matching units: `1.0<m> + 1.0<s>` is
    a type error. Multiplying multiplies units: `1.0<m> * 1.0<s> :
    Real<m*s>`.
  - Units cannot constrain integer types — `Int<m>` does not exist.
    Only `Real`.

NOT IN KAIKAI
  - Unit conversions as language built-ins. Write a conversion fn:
    `fn ft_to_m(x: Real<ft>) : Real<m> = x * 0.3048<m/ft>`.
  - Dimensional analysis on `Int`. Use `Real`.
  - F#-style unit suffixes without angle brackets (`9.81<...>` is
    required; `9.81m_s_2` is not a thing).

SEE ALSO
  kai info syntax, docs/kinds.md
