# `#[constructor]` — `Type(args)` sugar for the conventional constructor (design proposal)

**Status:** proposal, not accepted. Written for later evaluation.

## The problem

Constructing a type is more verbose than constructing a variant, with no design
reason — an asymmetry between kaikai's two product forms:

```kaikai
type BigInt = Small(Int) | Big(Int, Array[Int])    # sum: Small(5) works, positional, free
type Rational = { num: BigInt, den: BigInt }        # record: no positional ctor
type Complex  = { re: Real, im: Real }              # record: no positional ctor
```

`Small(5)` is positional because a variant *is* a named constructor-function
(`Some : (T) -> Option[T]`, generated). A record generates none — the only way in
is the literal `Rational { num: 1n, den: 4n }`. The stdlib already pays for this by
hand: `complex.mk(re, im)`, `rational.make(num, den)` are hand-written positional
constructors that exist *only* because the language does not generate them (with
inconsistent names, `mk` vs `make`).

## Rejected: positional literal sugar (`Type(args)` ≡ the record literal)

The first idea — `Rational(1n, 4n)` desugars to `Rational { num: 1n, den: 4n }` —
was **rejected**, because `BigInt` breaks it:

- **Sum types have no single positional form.** `BigInt(5)` is ambiguous — is it
  `Small(5)` or `Big(...)`? The type name maps to no representation; a `BigInt` from
  `5` is `from_int(5)`, which *chooses* `Small` vs `Big` by logic. The type is not
  its representation.
- **Records with invariants must not skip their logic.** `Rational { num: 2n, den:
  4n }` bypasses `make`'s canonicalisation (gcd reduction, sign normalisation),
  producing a non-canonical `2/4`. Literal sugar *is* the hole. It is harmful here,
  not convenient.
- **`priv` fields already block the literal** (`stdlib/time.kai`:
  `Instant = { priv secs, priv nanos }`) — so literal sugar can't even reach an
  encapsulated type.

Literal sugar only works for flat, invariant-free, public-field records (`Point`) —
exactly where the win is smallest and the reorder-fragility largest.

## Rejected: overloading + variadics

The urge to write `BigInt(5)` **and** `BigInt("123456")` under one name is
overloading (resolution by argument type/arity). **Rejected** — it reopens Tier 1
#3 ("no Haskell-style type-class resolution … decidable"): overloading is the
ad-hoc resolution that breaks HM inference (`f(x)` has no inferable type until `f`
is resolved, and multi-argument resolution is the indecidable case). kaikai bet on
decidable HM + single-dispatch `O(1)`; overloading unwinds that bet.

The two distinct constructions are better *named*: `from_int` lifts an `Int`,
`from_string` parses text and can fail (`Result`). Different names are information,
not verbosity — `BigInt("123")` overloaded hides that it can fail; `from_string`
declares it in the name and the return type. This is the ML/Elm/Elixir discipline:
name the conversion. And genuine ad-hoc polymorphism (`show`, `==`, `+`) is already
covered by single-dispatch protocols — one dispatch type, `O(1)` table, decidable,
*not* overloading.

## Accepted: `#[constructor]` marks the sugared function

The type author marks **one** function per type as the sugared constructor. The
sugar `Type(args)` desugars to a call to that marked function — not to the literal:

```kaikai
#[constructor]
pub fn from_int(n: Int) : BigInt =              # BigInt's convenience ctor is from_int,
  if n == 0 - small_mag_max() - 1 { ... }       #   NOT make(sign, mag)
  else { Small(n) }

#[constructor]
pub fn make(num: BigInt, den: BigInt) : Rational = { ...canonicalise... }

let b = BigInt(5)          # ≡ bigint.from_int(5) — passes the logic, picks Small/Big
let r = Rational(1n, 4n)   # ≡ rational.make(1n, 4n) — canonicalises
```

Why this is the right shape:

- **No overloading.** Exactly one `#[constructor]` per type, one signature. The
  sugar maps to *that* function, resolved with zero type-directed dispatch. HM
  decidability untouched.
- **Author chooses which function.** `BigInt`'s sugared ctor is `from_int` (the
  convenience form), not `make` (the raw sign+magnitude form). The compiler does not
  guess a magic `make` name; the author marks the right one.
- **Works for records AND sum types.** `#[constructor]` marks any function
  returning the type — a variant-returning `from_int` for `BigInt`, a
  record-returning `make` for `Rational`. Uniform.
- **Respects invariants and `priv`.** The marked function *is* the gate, with its
  logic; the sugar cannot skip it (unlike literal sugar). A `priv`-field type is
  constructed only through its `#[constructor]`.
- **Removes stdlib boilerplate.** `mk`/`make` stop being hand-written surface —
  they become the marked constructor, invoked as `Complex(3.0, 4.0)` /
  `Rational(1n, 4n)`.

The other constructions stay **named**: `bigint.from_string("…")`,
`rational.from_int(n)`, `complex.from_real(r)`, `zero()`, `one()`. Only the single
convenience constructor gets the `Type(args)` sugar; the rest are named functions,
which is clearer than overloading them under one call site.

## Design decisions (fixed)

- **One `#[constructor]` per type.** Multiple would require arity/type resolution =
  overloading = rejected. The convenience ctor gets the sugar; other constructions
  keep their names. `BigInt(5)` and `BigInt("123")` do **not** coexist — the second
  is `from_string("123")`, and its distinct name/`Result` type is a feature.
- **Desugars to a call, not a literal.** `Type(args)` → `module.marked_fn(args)`,
  passing through the function's logic. Never the raw record literal.
- **Attribute on a `pub fn` returning the type**, resolved via the type's home
  module. `#[constructor]` before `pub`, like other attributes.

## Open questions

- Is the sugar worth the attribute surface, or is `import bigint.{from_int}` +
  `from_int(5)` enough? (The verbosity was two things — the `module.` prefix, fixed
  by selective `import`/`use`, and the `make`/`mk` name, fixed by convention.
  `#[constructor]` addresses the second and the call-site form; weigh against just
  documenting a naming convention.)
- Does `Type(args)` collide with anything in expression position when `Type` is also
  a type name? (Variants keep type and ctor names distinct — `Option`/`Some`; here
  `BigInt` names both the type and the ctor call. Confirm the resolver separates
  type-position from expression-position, as it must for the sugar.)
- Interaction with generics: `Vec[Int](…)` — does the sugar carry type arguments?

## References

- `stdlib/math/bigint.kai` — `make(sign, mag)` vs `from_int(n)`: the evidence that
  the sugared ctor must be author-chosen, not a fixed `make` name.
- `stdlib/rational.kai`, `stdlib/math/complex.kai` — `make`/`mk`, the hand-written
  positional constructors this formalises.
- `stdlib/time.kai` — `priv` fields; the encapsulation half of controlled
  construction (kaikai already has abstract-data-type + smart-constructor).
- CLAUDE.md Tier 1 #3 — why overloading/variadics are out (decidable HM).
- `docs/protocols.md` — single-dispatch, the decidable ad-hoc polymorphism kaikai
  *does* have.
