# Units of Measure for kaikai

## Status: Landed (m12.5, 2026-04-26)

Implemented end-to-end in stage 2: lexer, parser, typer (abelian-group
unification of unit expressions, generalisation over unit-vars), and
codegen erasure (units evaporate before any C/LLVM IR is emitted).
Fixtures live in `examples/units/m12_5_*.kai`. Lane experience report
at `docs/lane-experience-m12.5.md`.

Research notes, snapshot 2026-04-25. Discussion document.
Question: does F#'s units-of-measure feature (Andrew Kennedy
1997, 2010) fit kaikai, and is it worth implementing?

**Short answer: yes, it fits cleanly, and there is a strong case
for prioritising it early because of its impact on the fintech
pitch.**

## Summary of the F# feature

F# is the only mainstream language with first-class units of
measure. The feature combines:

- **Declaration**: `[<Measure>] type cm` introduces a unit
  symbol. No payload, no runtime.
- **Literals**: `1.0<cm>` annotates a literal with its unit.
- **Types**: `float<cm>` is a dimensioned float.
- **Abelian algebra**: `kg m / s^2` is product/quotient/power.
  Canonicalised by the compiler (`m /s s * kg` ≡ `kg m/s^2`).
- **Dimensionless**: `<1>` or absence. Compatible with no-unit.
- **Aliasing**: `[<Measure>] type N = kg m / s^2`.
- **Generic units**: type parameter `'u` with `[<Measure>]`
  annotation. `let add (x: float<'u>) (y: float<'u>) = x + y`.
- **Records with generic units**: `type Vec<[<Measure>] 'u> = { x:
  float<'u>; y: float<'u> }`.
- **Conversion**: multiply by a factor with units (`x * 1.0<cm/inch>`)
  or `float x` to erase units.
- **Zero runtime cost**: units disappear after the checker. Zero
  overhead.
- **Only applies to primitive numerics** (float, int, decimal,
  all signed/unsigned variants).
- **Limitation**: units cannot be inspected at runtime
  (no `ToString`, no reflection over units).

Classic F# use cases:
- Physics: `1.0<m/s>`, `9.81<m/s^2>`.
- Finance: `1.50<USD>`, `1.20<EUR/USD>` (exchange rate).
- Engineering: prevent feet-vs-meters bugs (the Mars Climate
  Orbiter lost USD 327M to a missing unit conversion).

## Why it fits kaikai

### Tier 1 — Safe at compile time

Tier 1 #1 lists it explicitly: "effects visible in types, no
null, memory safe". Units of measure is **the same principle
applied to arithmetic**: what the unit is must live in the
signature, checked at compile time. Full conceptual fit.

### Tier 1 — Runtime efficient

Zero runtime cost. Units are erased after the typer; the
generated code operates on plain `Real` or `Int`. Fits perfectly
into the "primitives unboxed" principle.

### Tier 1 — Fast compilation

Unit inference is decidable and efficient. Kennedy 1996/2010
shows the algorithm is polynomial. The extra cost in the typer
is proportional to the subset of functions that use units —
pure code (the majority) pays nothing. Same "transform only what
is affected" model already used for effects (Doc C §*The CPS
transform* §*What gets transformed*).

### Tier 2 — Approachable core

Units do NOT affect code that does not use them. A "hello world"
or a fizzbuzz does not need to know about them. When the dev
writes `1.50<USD>`, they are opting in voluntarily. Compatible
with the "few visible concepts, layered" principle.

### Tier 3 — LLM authorability

Here UoM gives a **substantial gain**: the typed hole
`?: Money<USD>` does not just say the type — it also says the
expected unit. An LLM that sees `?: Money<USD>` has much more
information than `?: Money` to fill the hole correctly. This is
exactly the kind of signal the Tier 3 bet exploits.

## Why it fits the adoption strategy

### For fintech (`adoption-enterprise-fintech.md`)

`Money<USD>` ≠ `Money<EUR>` resolves **the most typical fintech
bug** in one line. The compiler rejects `usd_balance + eur_balance`
without an explicit conversion. No Java, Python, Go, or Rust does
this without custom libraries and boilerplate.

The C2 Fintech toolkit on the roadmap mentions Money/Decimal as
an **immediate differentiator**. UoM is the mechanism that lets
that differentiator be robust without being library-defined
(with all its boilerplate and escapes).

Literal pitch: "kaikai is the only mainstream language where
adding USD to EUR is a compile error, not a production bug".
That sells.

### For web/startup (`adoption-web-startup.md`)

Branded types — the "tag a string for safety" pattern:

- `String<UserId>` vs `String<OrderId>` — the compiler rejects
  `delete_user(order_id)`. Without UoM, devs do this with
  single-field records and get tired; with UoM it is zero
  overhead and zero friction.
- `String<RawInput>` vs `String<Sanitized>` — a classic. A web
  app that forgets to sanitise before injecting HTML is caught
  by the compiler.
- `Int<Seconds>` vs `Int<Milliseconds>` — timeout bugs.

F# does not support this in branded form outside numerics; there
is a community package (`FSharp.UMX`) that extends to arbitrary
types. **kaikai can ship it natively**, which is differential.

## Proposed design

kaikai-specific decisions. Each one pinned for review.

### 1. Declaration

**Decision**: a dedicated `unit` keyword, not reuse of `type`.

```kai
unit USD
unit EUR
unit cm
unit sec
```

Reasons:
- Reusing `type` introduces confusion: `type USD` already means
  "alias or sum type". Overloading it is unnecessary ambiguity.
- `unit` makes it clear that this is a dimensional symbol.
- F#'s `[<Measure>]` attribute is ugly and .NET-specific. kaikai
  has no attributes; do not invent them just for this.

Aliases with algebra:

```kai
unit Newton  = kg * m / sec^2
unit Pascal  = Newton / m^2
```

**Capitalisation**: unit symbols are ordinary identifiers, so any
casing is accepted (`m`, `M`, `kg`, `USD`, `Newton`). Names are
case-sensitive — `m` and `M` are distinct units. Conventional usage
(not enforced by the compiler):

- SI base units in lowercase: `m`, `s`, `kg`, `mol`, `cd`. The two
  exceptions `K` (kelvin) and `A` (ampere) are uppercase because
  `k` and `a` collide with common metric prefixes.
- Units named after a person in titlecase: `Newton`, `Pascal`,
  `Joule`, `Hertz`, `Watt`, `Celsius`.
- Currencies in ISO 4217 uppercase: `USD`, `EUR`, `CLP`.

### 2. Literals and types

**Decision**: angle brackets like F#, syntax `<unit-expr>`.

```kai
let price : Real<USD>      = 1.50<USD>
let speed : Real<m / sec>  = 9.81<m / sec>
let rate  : Real<USD/EUR>  = 1.20<USD/EUR>
let area  : Real<m^2>      = 4.0<m^2>
```

Reasons:
- F# syntax is proven, understandable.
- Distinguishes from generics `[T]` (kaikai uses `[]` for type
  args).
- Angle brackets are already free lexical space in kaikai (not
  used for generics).

### 3. Eligible primitive types

**Decision**: only `Real` and `Int` in m-pre-1.0. Extension to
"branded types" over arbitrary types (FSharp.UMX-style) in
m-post-1.0.

```kai
let id : Int<UserId> = 42<UserId>              # OK in v1
let s  : String<Sanitized> = "..."<Sanitized>  # m-post-1.0
```

Reasons:
- F# starts from numerics and extends later. Same path is
  prudent.
- Branded types over String/records require extra care with
  drop semantics (Perceus) — defer to a separate milestone.

### 4. Unit algebra

**Decision**: same as F# — multiplication, division, integer
powers (positive and negative), canonicalisation in the typer.

```
m * sec, m / sec, m^2, sec^-1, m * sec / kg
```

Canonicalisation: alphabetically order numerator and denominator,
group exponents (`m * m` → `m^2`), eliminate factor `1`.
`kg m / sec^2 ≡ m kg / sec^2 ≡ m * kg * sec^-2`.

**Pretty-printing** (used by `unit_name`, `impl Show for Real<u>`,
and typer diagnostics): scientific style. Multiplication renders
as a single space (`kg m`), division as `/` with no surrounding
spaces (`m/s`, `kg m/s^2`). Powers use `^` with the integer
literal (`m^2`, `s^-1`). Trivial `1` factors collapse out.

#### 4.1. Value-level `^` (v0.9.0)

The `^` operator also works at the expression level: `r ^ n`
raises a numeric value to an integer power. When `r` has a non-
trivial unit, the exponent must be a literal Int so the typer can
lift `Real<u> ^ N` to `Real<u^N>` statically (no dependent types).
A non-literal exponent on a dimensioned base is rejected.

```kai
let r : Real<m>     = 3.0<m>
let area : Real<m^2> = r ^ 2     # OK — literal exponent, unit lifted
let vol  : Real<m^3> = r ^ 3
let inv  : Real<m^-1> = r ^ -1   # negative literal accepted

let n = 2
let bad = r ^ n                  # error: literal Int exponent required
```

`^` is right-associative (`2 ^ 3 ^ 2` parses as `2 ^ (3 ^ 2) = 512`)
and binds tighter than `*`/`/`. Bare numeric bases (`Int`, `Real`,
`Real<1>`) accept any `Int` exponent — the runtime helper
`kai_pow_int` handles negatives on `Real` as `1.0 / base^|n|` and
clamps negatives to 0 on `Int`.

### 5. Generic units

**Decision**: introduce a `Unit` kind for type parameters; do not
reuse the `Type` kind.

```kai
fn double[u: Unit](x: Real<u>) : Real<u> = x * 2.0
fn area[u: Unit](w: Real<u>, h: Real<u>) : Real<u^2> = w * h
fn divide[u: Unit, v: Unit](a: Real<u>, b: Real<v>) : Real<u/v> = a / b
```

Reasons:
- Separating kinds prevents errors like `Real<Int>` (where Int
  is a type, not a unit).
- F# uses `[<Measure>] 'u`, which is essentially this but with
  attribute syntax.

### 6. Dimensionless

**Decision**: the absence of `<...>` is dimensionless. `<1>` is
also dimensionless, as an explicit synonym.

```kai
let unitless : Real     = 3.14
let unitless2 : Real<1> = 3.14
let mixed = unitless + 1.0       # OK, both dimensionless
let bad = price + unitless        # error: USD ≠ 1
```

Conversion to/from dimensionless requires multiplying by an
explicit factor with a unit.

### 7. Conversions

**Decision**: multiplicative factor + a `unitless` function for
the escape.

```kai
let cm_per_m : Real<cm/m> = 100.0<cm/m>

let h_m : Real<m>   = 5.0<m>
let h_cm : Real<cm> = h_m * cm_per_m   # OK: m * cm/m = cm

let raw : Real = unitless(h_m)         # explicit drop
```

`unitless` is a prelude function (`unitless[u: Unit](x: Real<u>) : Real`).
There is NO implicit unit→no-unit conversion.

### 8. Pretty-printing

**Decision**: units are preserved in diagnostics and dump-typed.
At runtime they do not exist.

```
error: type mismatch in operator `+`
  --> src/foo.kai:42:5
   |
42 |   usd_balance + eur_balance
   |   ^^^^^^^^^^^^^^^^^^^^^^^^^^
   = note: expected: Real<USD>
   = note: found:    Real<EUR>
   = help: convert with an explicit rate, e.g. `eur_balance * usd_per_eur`
```

### 9. Interaction with effects

**Decision**: orthogonal. A function can be unit-polymorphic and
have an effect row at the same time.

```kai
fn log_price[u: Unit](p: Real<u>) : Unit / Console = {
  Console.print("price: #{real_to_string(unitless(p))}")
}
```

No special interaction. The two mechanisms live in their own
"spaces" of the type system.

## Key use case: Money

`Money` is not directly UoM — it is a record with a currency tag
and an internal representation (typically Decimal). But UoM makes
it much cleaner:

```kai
# Without UoM (manual boilerplate)
type Money = { amount: Decimal, currency: String }
fn add(a: Money, b: Money) : Result[Money, String] = {
  if a.currency == b.currency { Ok(Money { amount: a.amount + b.amount, currency: a.currency }) }
  else { Err("cannot add different currencies") }
}

# With UoM (compiler does the check)
unit USD
unit EUR
type Money[u: Unit] = { amount: Decimal<u> }
fn add[u: Unit](a: Money<u>, b: Money<u>) : Money<u> = {
  Money { amount: a.amount + b.amount }
}
# Use:
let total = add(usd_a, usd_b)   # OK
let bad   = add(usd_a, eur_b)   # compile-time error
```

`Decimal<u>` requires extending the primitive type list to
include `Decimal` (when it lands in stdlib). Already pinned.

Standard currencies (ISO 4217) pre-declared in stdlib:

```kai
# stdlib/money/currency.kai
unit USD; unit EUR; unit GBP; unit JPY
unit BRL; unit CLP; unit ARS; unit MXN
# ... 180 ISO 4217 codes
```

Exchange rates:

```kai
let usd_per_eur : Real<USD/EUR> = fetch_rate("EUR", "USD")
let eur_amount  : Money<EUR>    = Money { amount: 100.0<EUR> }
let usd_amount  : Money<USD>    = Money { amount: eur_amount.amount * usd_per_eur }
```

## Implementation in stage 2

### Typer changes

1. **New type kind**:
   ```
   Ty = TyInt | TyReal | ... | TyDim(Ty, UnitExpr)
   UnitExpr = UnitVar(Int) | UnitSym(String) | UnitMul(UnitExpr, UnitExpr)
            | UnitInv(UnitExpr) | UnitOne
   ```

2. **Extended unifier**: an additional case for `TyDim`. When two
   `TyDim(t1, u1)` and `TyDim(t2, u2)` unify, first unify t1 with
   t2 (as today), then unify u1 with u2 via abelian-group
   unification (Stuckey 1990, Kennedy 1996).

3. **Abelian group unification**:
   - Canonical form: ordered product of integer powers of symbols
     and unit-vars, e.g. `m^2 * sec^-1 * 'u`.
   - Solving: represent as a vector of exponents; find a
     substitution that equates two vectors. Algorithm: Smith
     normal form over the exponent matrix.
   - Decidable and polynomial.

4. **Generalisation**: add unit-vars to the set of generalisable
   free variables. A binding `let x = ...` with type
   `Real<m * 'u>` generalises `'u` the same way as a type var.

### Parser changes

1. **New token**: `unit` as a keyword.
2. **Literal syntax**: lex `1.0<USD>` as a literal with unit
   annotation. No space between the number and `<`.
3. **Type syntax**: `Real<unit-expr>` parses as a dimensioned
   type.
4. **Declaration syntax**: `unit Foo` and `unit Foo = expr`.
5. **Generics syntax**: `[u: Unit]` as a kind annotation on type
   parameters.

### Codegen changes

**None**. The typer erases units before codegen. `Real<USD>`
emits the same as `Real`. No runtime tax.

### Proposed roadmap

A new milestone **m12.5 — Units of Measure**, after m12 (self-host
checkpoint), before m5 (Perceus). Reasons:

- Independent of effects/fibers (m7/m8). Does not block anything.
- Substantially benefits the C2 Fintech toolkit. If C2 starts
  before UoM, the toolkit re-implements Money with boilerplate;
  afterwards it has to migrate.
- Isolated change to the typer. ~500-1000 new lines.
- Immediate traction: a concrete demonstrable pitch for fintech.

Estimate: **2-3 days at observed velocity**. 50% buffer →
**4 days**.

Alternative: prioritise before m12. Reason: C2's Money/Decimal
would be much cleaner from the start. Cost: m12 self-host has
to be re-validated with UoM active.

## Risks

1. **Added typer complexity**. Abelian-group unification is not
   trivial. Mitigation: exhaustive tests over canonicalisation
   + property-based testing.

2. **F# is the only mainstream with this**. Possible signal of
   niche demand. Mitigation: the fintech case is concrete and
   not niche. The adoption study
   (`adoption-enterprise-fintech.md`) holds it as a
   differentiator.

3. **Extra learning curve**. New devs may not understand unit
   polymorphism. Mitigation: units are opt-in. Whoever does not
   use them does not suffer them. Tier 2 #6 (few visible
   concepts, layered) allows this.

4. **Generics interaction**. `Real[T]` (generic) vs `Real<u>`
   (unit) could confuse. Mitigation: the syntax distinguishes
   (`[]` vs `<>`) and the kinds are separate (`Type` vs `Unit`).
   Explicit doc.

5. **Impact on error messages**. Unit errors can be cryptic
   without effort. Mitigation: invest in specific diagnostics
   for unit mismatches (show the canonical form of both sides,
   suggest a common conversion).

## Three open questions to decide

1. **Literal syntax**: `1.0<USD>` (F#) vs `1.0_USD` (Rust-style
   suffix) vs `(1.0 : Real<USD>)` (pure annotation). F# is
   proven. Rust suffix is lighter. Recommendation: F# for
   familiarity.

2. **Branded type support** (FSharp.UMX-style on `String`,
   records, etc.): include from v1 or defer. Recommendation:
   defer; the numeric case already gives 80% of the value.

3. **Declaration syntax**: `unit Foo` (new kw) vs `dim Foo` vs
   `tag Foo`. Recommendation: `unit` for maximum familiarity
   with F#/SI.

## Executive summary

- **Yes, it fits**. Natural fit with Tier 1 (compile-time safety
  + runtime efficiency) and Tier 3 (LLM authorability).
- **Yes, it is worth it**. Concrete differentiator for fintech,
  branded types for web/startup, no runtime cost.
- **Implementable**. ~500-1000 lines in typer/parser, codegen
  intact. 2-4 days at observed velocity.
- **When**: new milestone **m12.5** between m12 and m5. Possibly
  bring forward to before C2 Fintech to avoid re-implementation.
- **Risks manageable**. F# proved the path; copying with good
  ergonomics is work, not research.

Recommendation: schedule formally as m12.5 in
`docs/stage2-design.md` and consider bringing it forward if
C2-pre (Money/Decimal as kaikai core lib) becomes a priority.

## Sources

- [Types for Units-of-Measure: Theory and Practice — Kennedy 2010 (Springer)](https://link.springer.com/chapter/10.1007/978-3-642-17685-2_8)
- [Relational Parametricity and Units of Measure — Kennedy 1997 (POPL)](https://people.mpi-sws.org/~dreyer/tor/papers/kennedy.pdf)
- [Types for Units-of-Measure in F# — Kennedy 2008 slides](https://www.kb.ecei.tohoku.ac.jp/ml2008/slides/kennedy.pdf)
- [Units of Measure — F# Language Reference (Microsoft Learn)](https://learn.microsoft.com/en-us/dotnet/fsharp/language-reference/units-of-measure)
- [Units of Measure — F# for Fun and Profit](https://fsharpforfunandprofit.com/posts/units-of-measure/)
- [Type-Safe Currency Conversion with F# Units of Measure (HAMY)](https://hamy.xyz/blog/2024-10_fsharp-units-of-measure-currency)
- [FSharp.UMX — branded types via UoM](https://github.com/fsprojects/FSharp.UMX)
