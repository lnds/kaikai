# The kind system — taxa, taxologies, habitants (consolidated design proposal)

**Status:** proposal, not accepted. Consolidates a long design conversation into
one model. Supersedes the scattered notes in `docs/layout-kind-design.md` and the
kind sections of `docs/unsafe-systems-design.md`; those remain the per-feature
detail, this is the whole system.

Today kaikai has two kinds (`Type`, `Measure`) hardcoded in the compiler
(`docs/kinds.md`). This proposal generalises to a **catalog of decidable
unification properties** with a uniform declaration surface, while keeping the
catalog **closed** and the inference **decidable** (Tier 1 #3).

## Terminology (read this first)

The system has a small, precise vocabulary. Two of these are surface keywords;
the rest are concepts.

| Term | What it is | Surface |
|---|---|---|
| **taxon** | a *kind*: a name plus a classification algebra (`taxon Metric = $abelian_group`) | keyword `taxon` |
| **taxology** | a *classification algebra*: an assembled, decidable unification theory (`taxology $abelian_group = { assoc, commut, inverse, identity }`) | keyword `taxology`, catalog-only |
| **property** | one algebraic law a taxology may include (`assoc`, `commut`, `idempotent`, `inverse`, `identity`, …) | contextual, inside `{ }` |
| **habitant** | a member of a taxon (`meter` in `Metric`, `USD` in `Money`) | per-taxon introducer word |
| **introducer** | the keyword that declares a taxon's habitants (`unit`, `metric`, `currency`, `perm`, `layout`) | keyword, one per taxon |

A *taxonomy* is the classification system; a *taxon* is one class within it. So
"`taxon Metric = $abelian_group`" reads "Metric is a taxon under the abelian-group
taxology". `taxon` and `taxology` were chosen because `kind`/`theory` collide with
popular identifiers (`kind` alone appears ~1350 times in the compiler as a field
name); `taxon`/`taxology` are collision-free and taxonomically apt.

## The three levels

```
value    : type       let x : Int
type     : taxon       unit km        (habitant : taxon)   /  type Person = {...}
taxon    = taxology    taxon Measure = $abelian_group      (taxon : taxology)
taxology              $abelian_group  = { assoc, commut, inverse, identity }
```

- A **value** has a **type** (`42 : Int`).
- A **type/habitant** has a **taxon** (`Int : Type`, `km : Measure`).
- A **taxon** is a name **plus a taxology** (`Measure = $abelian_group`).
- A **taxology** is a decidable unification algebra, assembled from a **closed menu
  of properties**.

## What a taxology is (and is NOT)

A taxology decides **type equality by unification, at compile time**. It is NOT a way
to compute values.

- `$abelian_group` decides that `m·s` unifies with `s·m` — it exists so the *type
  checker* treats them as the same type, not so you can multiply units.
- A taxology is **phantom**: it guides unification and is erased at runtime (except
  where it parametrizes codegen, e.g. `$composition`'s byte-swap — still no runtime
  value cost).

**The hard rule (why properties are a closed menu).** A taxology is a decidable
E-unification algorithm. E-unification over *arbitrary* equational theories is
**indecidable**, and it is itself indecidable to tell whether a given theory's
unification terminates — so a user cannot write the equations of a taxology directly.
Instead a taxology is **assembled from a closed menu of properties** the compiler
knows how to decide (see next section). Decidability is verified *by construction*:
the compiler owns a terminating unification engine for each recognised
property-combination, and an unrecognised or provably-indecidable combination is a
**compile error**, not a hung type checker. This is the same discipline that forbids
type-class resolution and the borrow checker: bounded, decidable, checkable.

**Two interacting operations → out.** Every viable taxology combines laws over
**one** operation. Two interacting operations (a ring's `+` and `×` with
distributivity) make unification indecidable. `$abelian_group` (one op) is in; a ring
is out. This is the cheap admission filter: one op → candidate; two interacting → no.
`distributes_over` is deliberately absent from the property menu for exactly this
reason.

## Taxologies are assembled from a closed property menu

A taxology is **not** an atomic primitive picked from a fixed list — it is a named
**assembly of properties** from a closed menu the compiler can decide:

```kaikai
# stdlib/core: the standard taxologies are named assemblies
taxology $abelian_group = { assoc, commut, inverse, identity }
taxology $semilattice   = { assoc, commut, idempotent }          # no inverse
taxology $composition   = { assoc, measure }                     # non-commutative, summed measure
```

The **properties** are the closed catalog now — `assoc`, `commut`, `idempotent`,
`inverse`, `identity`, `measure`, `rows`, … — each a law the compiler knows how to
unify under. The compiler recognises which *combinations* have a terminating engine
(abelian group = assoc+commut+inverse+identity; semilattice = assoc+commut+idempotent;
etc.). A combination with no known-decidable engine is rejected at the declaration.

**A future user can create a taxology — by assembling, never by writing algebra.**
`taxology $mine = { assoc, commut, idempotent }` is legal (the compiler recognises it
as a semilattice engine); `taxology $bad = { assoc, distributes_over(...) }` is a
compile error (no decidable engine, two interacting ops). The user composes from the
menu; they never write unification equations. This keeps the door open without
crossing the Tier 1 #3 red line.

The standard `$…` names (`$abelian_group`, `$semilattice`, `$composition`) are thus
**aliases for common property assemblies**, provided by `stdlib/core` for convenience
and readability — not opaque atoms.

Notation: **`$snake_case`**. The `$` sigil marks a compiler primitive (as in kaikai's
`$extern_handler` intrinsics); snake_case marks "not a type" (types are PascalCase).
Together they signal that a taxology is *assembled from the property menu, never
hand-written* — `$my_thing` is a compile error unless it is a declared assembly.

**Not expressible** (indecidable or not a unification problem): rings/fields (two
interacting ops), arbitrary graphs / DAGs (relational, not algebraic — runtime data),
dependent-types-with-effects (the F* unified theory — powerful but indecidable,
rejected for the same reason as the borrow checker and overloading). DAG, workflow
engines, crypto/number-theory, JSON binding: all **runtime or structural**, not
taxologies — a taxology unifies types, it does not compute or relate graphs.

## Where the catalog lives (and why it stays decidable)

Two layers:

1. **The property engines** — compiler code (`stage2/compiler/`), like today's
   `unify_unit` (the abelian-group engine for `Measure`) in `infer.kai`. NOT
   declarable in kaikai. The engine catalog *is* which property-combinations the typer
   can unify — closed by construction, only what was programmed and proven decidable.

2. **The taxology registry** — one stdlib file, `stdlib/core/taxologies.kai`, names
   the standard property assemblies and gives each visibility:

   ```kaikai
   priv taxology $structural    = { }                                  # Type's engine — private
   pub  taxology $abelian_group = { assoc, commut, inverse, identity }
   pub  taxology $semilattice   = { assoc, commut, idempotent }
   pub  taxology $composition   = { assoc, measure }
   priv taxology $rows          = { assoc, commut, idempotent, rows }  # Effect's engine — private
   ```

   **Two taxologies are private** (`$structural`, `$rows`), for the same reason: their
   only useful taxon is already built-in and must not be user-replicated. `Type` is the
   sole structural taxon — a user-declared isolated type universe
   (`taxon Validated = $structural`) would add only *isolation*, which phantom types /
   newtypes already approximate. `Effect` is the sole rows taxon (its machinery is
   built-in). The **public** taxologies are exactly those where a new user taxon adds an
   algebra nothing else gives — Money's abelian group, Perm's idempotent join,
   Waterfall's summed measure. The admission criterion:

   > **A taxology is public if declaring new taxa over it yields an algebra nothing else
   > provides; private if its only useful taxon is already built-in.**

### The `taxology` declaration is module-restricted

`taxology $x = { … }` is a declaration form **legal only inside the
`stdlib/core/taxologies` module** — a deliberate special-case in the parser/resolver,
like other context-restricted forms, not a general capability. A `taxology` in any
other file is a compile error: "`taxology` declarations are only allowed in the
taxology catalog". This keeps the *standard* assemblies in one home. (Future
user-assembled taxologies, if enabled, would relax this to "any file, but only from
the property menu" — the property menu stays the closed part, not the location.)

## Taxa — declaration and visibility

A taxon is declared over a **public** taxology. Taxa obey ordinary `pub`/`priv`
visibility — the **same** mechanism (and the same pub-leak validator) that governs
every other symbol. No new `intrinsic`/`builtin` marker: visibility is the barrier.

```kaikai
# stdlib/core/taxa.kai
pub  taxon Type    = $structural                       # public: usable
priv taxon Effect  = $rows                              # DECLARED, but private → unusable
pub  taxon Measure   = $abelian_group with unit
pub  taxon Currency  = $module over T with currency        # NOT $abelian_group; scalar is the carrier T — see below
pub  taxon Perm      = $semilattice  with perm
pub  taxon Layout    = $composition over Int      with layout { be le }
pub  taxon Waterfall = $composition over Rational with pct
```

> **Money is `$module`, not `$abelian_group` — `USD²` is the bite.** An earlier draft wrote
> `Money = $abelian_group with currency`. That is WRONG: `$abelian_group` is a *multiplicative*
> group over its habitants — it gives `unit × unit` (that is why `m/s²` and `m·s` work for
> physics), which for money means `USD × USD = USD²`, nonsense. Money admits `USD + USD`,
> scalar-mult by a dimensionless number (`3 · USD`), but NOT `USD × USD`. That is a **module /
> vector space**: an additive group of elements plus scalar multiplication from an external
> ring, no element×element product. See *The surface: `[carrier]`, `<habitant>`, `over`, `with`*
> and *When to separate two abelian taxa* below.

**Declared-but-unusable is just `priv`.** `Effect` *is* a first-class taxon — real,
declared, in the registry, listed by `kai info taxa` — but `priv`, so a user who writes
`taxon X = $rows` or references `Effect` gets a visibility error, the same one any
`priv` symbol produces. `$rows` is likewise `priv`. The compiler knows `rows`/`Effect`
exist (to give a clear error), but the pub-leak validator forbids their use outside
`stdlib/core`. This answers "how is a taxon declared yet unusable" without a bespoke
concept: it is the visibility kaikai already has.

A user may declare taxa over **public** taxologies only:

```kaikai
taxon Metric = $abelian_group with metric      # ✓ $abelian_group is public
taxon Perm   = $semilattice with perm          # ✓
# taxon Access = $structural                   # ✗ pub-leak: $structural is private
# taxon Bad   = $rows                          # ✗ pub-leak: $rows is private
```

Isolated type universes (a `Validated` vs `Unvalidated` taxon) are **not** available —
`$structural` is private, `Type` is its only taxon. Use a phantom type / newtype for
that isolation; the taxon would add nothing a wrapper does not.

## Habitants — the introducer and its policy

`with <introducer>` gives the keyword that declares the taxon's habitants. **One
introducer word declares exactly one taxon** — there is no default taxon and no
disambiguation rule. `unit` always means `Measure`; `metric` (from
`taxon Metric = $abelian_group with metric`) always means `Metric`. The word *is* the
taxon:

```kaikai
unit m                                          # unit ⇒ Measure, always
taxon Metric = $abelian_group with metric
metric meter                                    # metric ⇒ Metric, always
```

This is why `unit` never had to disambiguate before and never will: it is Measure's
exclusive introducer. A new taxon brings its own introducer word (you name it in the
`with`), so `metric meter` and `unit m` are never ambiguous. The cost — one new word
per taxon — buys zero ambiguity and zero magic default; the word carries domain
meaning (`metric`, `imperial`, `currency`, `perm`).

Two properties of each habitant set fall out of the **taxon** (not always the
taxology):

**Structure** — a habitant may be atomic or structured:

| Taxon | introducer | habitant | body? |
|---|---|---|---|
| Type | `type` | a type | yes (fields/variants) |
| Effect | `effect` | an effect | yes (operations) |
| Measure / Money / Perm | `unit` / `currency` / `perm` | a unit/currency/permission | no (atom) |
| Layout | `layout` | endianness | no (atom) |

`type` and `effect` are just the habitant introducers of their (built-in) taxa —
structured habitants — exactly as `unit` is Measure's — an atomic habitant. This
de-magics `type`/`effect`: they are the `with`-introducers of built-in taxa.

**Open vs closed** — who declares the habitants:

- **Open** (`with intro`, no block): the **user** declares atoms — `unit km`,
  `currency USD`, `perm read`. For taxologies whose atoms are **opaque symbols** the
  algebra manipulates without knowing them (`km·s` works without the engine knowing
  what a metre is). Unlimited.
- **Closed** (`with intro { a b }`): the block **selects** from atoms the engine
  already knows, each carrying built-in semantics (`be` = byte-swap). The user adds
  none and defines no semantics (that would extend the engine — forbidden). A closed
  block naming an atom the engine does not know (`{ be le xyz }`) is an error.

Policy is per-**taxon**: `Layout` is closed (`be`/`le` fixed); a hypothetical `Row`
over `$composition` (UI columns) would be open (arbitrary widths). Same taxology,
different habitant policy, set by the taxon.

**Value — opaque vs measure-carrying (a property of the TAXOLOGY):** whether a
habitant carries a numeric value depends on whether its taxology *sums* one.

- **Opaque** (`$abelian_group`, `$semilattice`): the habitant is a bare symbol —
  `unit km`, NOT `unit km = 1000`. The algebra manipulates symbols (`km/h`, `km²`,
  `read ∪ write`) but **never sums magnitudes**. The km↔metre relation lives in a
  separate conversion function, not the taxon.
- **Measure-carrying** (`$composition`): the habitant **declares a measure**, because
  the taxology *sums* it (`size(a·b) = size(a) + size(b)` is its essence — the
  `measure` property). To verify "tranches sum to 100%" the compiler must know
  `pct70 = 70`. So:

  ```kaikai
  taxon Waterfall = $composition with pct
  pct pct70 = 70        # the habitant DECLARES its measure; Composition sums it
  pct pct20 = 20
  pct pct10 = 10
  ```

  `Layout`'s `be`/`le` carry the measure implicitly via the field type
  (`U32` → 4 bytes). A bare `pct pct70` (no `= 70`) would be an opaque symbol meaning
  nothing — the name "pct70" is not self-describing.

So a taxology has **four** derived properties: unification rule, habitant policy
(open/closed), habitant structure (atomic/structured), and habitant value
(opaque/measure-carrying). `$composition` is the one whose habitants carry a measure,
precisely because summing that measure is what it does.

### How an intrinsic taxology operates on user-declared habitants

A subtle point: `Layout` feels coherent because `$composition` **and** `be`/`le` are
all intrinsic — the compiler knows the taxology, the habitants, and their codegen
semantics. But `taxon Waterfall = $composition with pct` + `pct pct70 = 70` mixes an
**intrinsic taxology** with **user-declared habitants**. How does a built-in engine
operate on `pct70`, which it never saw?

**A taxology defines an interface its habitants must satisfy; it does not know the
habitants, it receives from them what it needs.** `$composition` says: "each habitant
gives me a number — its measure — and I sum them." Two ways to satisfy it:

- **Closed habitants** (`be`) satisfy the interface **from the compiler** — built-in,
  intrinsic to the engine (their measure/semantics ship with `$composition`).
- **Open habitants** (`pct70 = 70`) satisfy it **via the declaration** — the `= 70`
  *is* the user handing the taxology the measure it asks for.

Declaring your own taxon over an intrinsic taxology does **not** extend the taxology
(that would be defining a unification/codegen algorithm — forbidden, indecidable). It
**feeds** the taxology's habitants the data the interface requires. The engine stays
100% compiler; the habitant is the user's; the `= N` is the visible contract between
them. That contract is why Waterfall is not magic: nothing about `pct70` is inferred —
the user states its measure, the taxology sums it.

### Measure arithmetic must be exact (decidability)

`$composition` *verifies* its sum at compile time ("tranches sum to 100%"). With
**floating-point** measures this is unsound — `0.1 + 0.2 ≠ 0.3` in IEEE-754, so a
legitimate waterfall could fail to compile on a rounding artefact. So a `$composition`
measure must be **integer or exact rational** (`pct70 = 70`, or `1/3`), never a float.
A tolerance-based check (`|sum − 100| < ε`) would inject an arbitrary ε into the type
checker — the same unpredictable "accept/reject by a magic number" that Tier 1 #3
rejects. Exact arithmetic keeps the sum verification decidable and sound.

## The surface: `[carrier]`, `<habitant>`, `over`, `with`

The full declaration form:

```
taxon Name = $taxology [over SecondDomain] with <habitant-form>
type  M[T]<Taxon> = { ... }
# use:  M[Carrier]<habitant>      e.g.  Money[Real]<USD>,  Matrix[Real]<3,4>
```

Four pieces, each with one job.

### `[T]` vs `<habitant>` — two orthogonal axes

A user type over a taxon has **two** parameters of different natures, in different brackets:

- **`[T]`** — an ordinary **type** parameter (`[]`, exactly like `Option[T]`). The `[]` list
  holds ONLY `Type` params (the carrier), with **no protocol bound** (see next). By convention
  name it `T` uppercase, as the stdlib does. It is the type the value is built from:
  `Money[Real]`, `Money[Decimal]`, `Matrix[BigInt]`.
- **`<habitant>`** — the taxon **habitant** (`<>`, exactly like `Real<m>`). A currency or a
  dimension is **not a type** — it is a classifying habitant, so it goes in `<>`. Taxa go in
  `<>`, never in `[]`; only `Type` goes in `[]`.

`Money[Decimal]<USD>` reads "money with carrier `Decimal`, currency `USD`". The two axes are
independent: the carrier is a plain generic in `[]`, the habitant is the taxon marker in `<>`.
This is what lets a taxon habitant live on a **library/user type** (`Money`) without the unit
machinery being wired to native heads — the number enters as an ordinary `[T]` generic, and
only the wrapper type (`Money`, `Matrix`) carries the `<>`.

```kaikai
type Money[T]<Currency> = { amount: T }
fn sum(a, v : Money[Real]<USD>) = a + v          # a inferred Money[Real]<USD>; a+v needs same currency+carrier
```

### No protocol bound on the carrier — monomorphisation is the gate

The carrier `[T]` carries **no** protocol bound. Protocol bounds on a `type`/record declaration
are NOT valid — `type M[T: Add]` is a parse error ("type-parameter kind must be `Type` or
`Measure`"). (Free *functions* DO accept bounds since #877 — `fn sum[T: Numeric](...)` — but a
`type` decl does not; and even on functions the bound is not yet enforced at the call site, see
issue tracking that gap. Either way, the taxon carrier `[T]` takes no bound.) The old
`[u: Measure]` form is different (`Measure` was a *kind*, not a protocol) and is superseded here:
taxa go in `<>`, only `Type` in `[]`.

So how is "the carrier must be summable" enforced without a bound? **By monomorphisation, in
the USE, not the signature — it does not propagate.** Verified:

- `Money[String]<USD>` *constructs* (kaikai does not check the carrier at type declaration). But
  it is **inert**: any monetary op fails at compile time — `money + money` on a `String` carrier
  gives `error: no impl of Add for type String`, because monomorphisation specialises the op per
  concrete carrier and checks the impl-table there. A `Money[String]` is a dead type that never
  reaches runtime — exactly as `List[T]` accepts any `T` and fails when you use an unsupported
  op. NOT special to Money.
- A generic `fn combine[T](a: Money[T], b: Money[T]) = a.amount.add(b.amount)` compiles even
  though `T` may lack `Add` — kaikai does NOT verify the abstract `T`; it waits for the concrete
  instance (`combine[Int]` checks `Int.add`, `combine[String]` errors on `.add`). The sharp line
  vs Haskell: NO constraint travels with the signature; the check is local per monomorphised
  instance. **`concrete ⇒ check locally / variable ⇒ wait for the instance`** — the same
  discipline as literal minting.

So `Money[String]` is constructible-but-inert (the accepted price of "no typeclasses"): it never
does harm, and forbidding it *in construction* would need the taxon to validate its carrier — a
local `impl`-table check per concrete instantiation (no constraint propagation), DEFERRED because
monomorphisation already rejects any real use. And there is **no algebraic-protocol hierarchy**
(`Numeric extends Ring…` = Haskell superclasses, vetoed): the fat `Numeric` protocol (only
Int/Real/Decimal implement it) is never the gate; overlapping small protocols (`Add` on every
numeric carrier) plus per-instance monomorphisation cover it.

### `over R` — the second domain (present iff the structure crosses two type domains)

`over R` comes from "a module **over** a ring R": it names a **second, distinct type domain**
the operation crosses. Present exactly when the taxology has two domains, absent otherwise:

- `$module` (Money): elements (amounts, the carrier) **and** scalars (a ring) — the two-domain
  structure. `over T` names the scalar; for Money the scalar IS the carrier `T` (you scale a
  `Money[Decimal]` by a `Decimal`, `3` minted to the carrier). A *general* module allows scalar
  ≠ element (a vector space of matrices over reals) — the surface can express that when needed
  by naming a scalar distinct from `[T]`; Money simply reuses `T`. Either way `over` is what
  marks the second domain, and (like the carrier) carries no protocol bound — monomorphisation
  checks the scalar's ops per concrete instance.
- `$composition` (Layout, Waterfall): elements (fields/tranches) **and** a summed **measure**
  (a number). `over Int` (Layout, byte sizes) / `over Rational` (Waterfall, exact %s). Here
  `over` names the measure type, an additive second domain, not a multiplicative scalar.
- Single-domain taxologies take **no** `over`: `$abelian_group`/Measure (`unit × unit → unit`,
  one domain — the rich `m/s²` algebra is still one domain), `$semilattice`, `$dimensional`.

The sharp line: **`over` is not "more than one operation" — it is "does the operation cross two
distinct type domains?"** UoM has `·`, `/`, `^` but all within one domain (units) → no `over`.
A module crosses element↔scalar → `over`.

### `with <form>` — what a habitant is (one keyword, four forms)

`with` always declares what a habitant *is*; the taxology behind it decides how the habitant
behaves (opaque vs measure-carrying, open vs closed — the per-taxology properties above):

- **(a) introducer → symbol** — `with currency` ⇒ `currency USD`. An opaque, user-declared
  symbol, open set. Money, Perm, Measure (`unit`).
- **(b) type → value** — `with Int`, `with String`. Habitants **are values** of that type,
  written directly (`<3>`, `<"tag">`), not declared; the type restricts validity
  (`Matrix[Real]<3,4>` ok, `<USD,4>` error). `Dim`, `Matrix`.
- **(c) introducer → symbol-with-measure** — `with pct` ⇒ `pct pct70 = 70`. A declared symbol
  that **carries a number** the taxology sums. Waterfall. The `= N` is the habitant declaring
  its measure (a measure-carrying taxology).
- **(d) closed set** — `with layout { be le }`. Habitants are a **fixed** set the engine knows
  (`be`/`le`), not user-extendable. Layout endianness.

One keyword, four forms; which applies follows from the taxology's properties. Integer literals
in `<>` already work today (`Real<m^2>`, exponent = literal integer), so `<3>` is not a new
capability.

### Practical mode — decidable checks compile-time, the rest at runtime

The catalog's "two interacting operations = indecidable = out" rule is not the last word for a
user's declared structure. The language lets you **declare** the mathematical structure of a
type and enforces it in a split: **compile-time for what is decidable, runtime for what is
not.** `$module`'s scalar-distributivity is never *proven* statically (that would be F*); what
the compiler checks statically is which operations exist and whether habitants/carriers are
compatible, and it **inserts runtime checks** for the rest.

For Money: `Money<USD> + Money<USD>` ✓ and `+ Money<EUR>` ✗ and `Money × Money` ✗ (no internal
product → no `USD²`) are **compile-time**; a currency read from a file is dynamic, so the
value→type boundary lives in `parse(carrier, code) : Option[Money<...>]` (like `int.parse`, one
type, no parallel "dynamic Money"). For Matrix: static shapes `Matrix<3,4> * Matrix<4,5>` check
at compile time (inner `4` cancels); a matrix of runtime-unknown shape checks at runtime (a trap
like an out-of-bounds index). Same split both cases — this is the "practicidad" that makes
indecidable structures usable without weakening Tier 1 #3's static core.

## Usage — uniform surface

```kaikai
type P = { x: Int }              # habitant of Type
effect E { op() : Unit }         # habitant of Effect
unit km                          # habitant of Measure
perm read                        # habitant of Perm

let d : Real<km>  = 5.0<km>      # Measure habitant on a numeric (bare unit in <>)
type H = { magic: U32<be> }      # Layout habitant on a field
let m : Money[Real]<USD> = ...   # user type: carrier T in [], currency habitant in <>

fn area(w: Real<u>, h: Real<u>) : Real<u^2> = w * h   # unit-polymorphic; u is a Measure habitant in <>
```

The taxon and its habitants appear in `<>` (`Real<u>`, `Real<km>`); `[]` holds only `Type`
carriers (`Money[Real]`). The keyword `taxon` never appears in *use* positions — only in the
one-line declaration. (The pre-taxon surface spelled unit-polymorphism `[u: Measure]`, a
*kind*-bound in `[]`; the taxon design moves habitants to `<>` uniformly, so `[]` is `Type`-only.)

## `unit` compatibility

`unit` stays exactly as-is: `unit x` ≡ a `Measure` habitant (its exclusive
introducer), spelled with the existing `TkUnitKw`. All current code (`unit m`,
`unit USD`, `unit kg`) is unchanged — no `: Measure` annotation is ever required,
because `unit` *is* Measure's introducer. A currency uses its own taxon's introducer
(`currency USD` after `taxon Currency = $module over T with currency`); `unit`
remains Measure-only.

## The hard locks (summary)

1. **The property menu is closed.** Taxologies are assembled only from the compiler's
   known-decidable properties; an unrecognised combination is a compile error, never a
   hung type checker. Engines live in the compiler.
2. **Standard taxologies live in one module.** The `taxology` form is legal only in
   `stdlib/core/taxologies`; a `$token` naming no decidable assembly is an error.
3. **Some declared taxa are unusable.** `priv taxon Effect = $rows` — real, registered,
   `kai info`-visible, but the pub-leak validator forbids use outside `stdlib/core`.
   Visibility is the barrier, not a new marker.

## When to separate two abelian taxa (Measure/Money was a trap)

Two taxa may share `$abelian_group` (`Metric` and `Imperial`) but stay isolated
(`meter + foot` = error). When is that separation worth it? The criterion, learned by a
counter-example:

**Separate two abelian taxa only when crossing the domains is *always* an error.**

- **Money is NOT such a case — separating it is harmful.** `USD/kWh` (price per
  energy), `USD/km` (freight), `USD/hour` (wage), `USD/kg` (commodities) are
  *legitimate* combinations of money with physics. A separate `taxon Money` would make
  `USD/kWh` a type error — forbidding exactly what real fintech/energy/logistics code
  needs. And `USD + EUR` is *already* rejected inside a single `Measure` (they are
  distinct units). So money belongs in `Measure` as ordinary units: it gets
  `USD+EUR`-rejection **and** `USD/kWh`-composition, the best of both. `taxon Money` is
  worse, not better. (The kWh example, `USD/(W*h) * (W*h) = USD`, is the proof: money
  divides by *compound* physical units, which demands one shared abelian algebra.)
- **Metric vs Imperial IS such a case.** `meter + foot` is always a bug — you must
  convert first. The Mars Climate Orbiter (1999, $327M lost) mixed newton-seconds with
  pound-force-seconds. `taxon Metric = $abelian_group` and
  `taxon Imperial = $abelian_group`, isolated, turn that class of bug into a compile
  error.
- **Coordinate frames / reference spaces** are the other real case: screen-space vs
  world-space, camera vs global — summing coordinates across frames without an explicit
  transform is a classic graphics/GIS bug. Separate frames → separate taxa.

Rule: **cross-domain combination legitimate → one taxon (Measure); cross-domain
combination always an error → separate taxa (Metric/Imperial, coord frames).** The
user-declared-`$abelian_group` case is real — it just is not money.

### Worked example — Metric/Imperial

```kaikai
taxon Metric   = $abelian_group with metric
taxon Imperial = $abelian_group with imperial

metric meter
metric second
imperial foot
imperial pound

fn area[u: Metric](w: Real<u>, h: Real<u>) : Real<u^2> = w * h

fn main() : Unit / Stdout = {
  let ok  = 3.0<meter> + 2.0<meter>        # ✓ same taxon
  let bad = 3.0<meter> + 2.0<foot>         # ✗ ERROR: distinct measure systems
  Stdout.print("ok")                       #   (Metric vs Imperial never unify)
}
```

`meter` and `foot` are bare units in `<>` — there is no `Metric.meter` qualification;
the taxon is carried by the unit's declaration, not by the use site. `taxon` appears
only in the two declaration lines.

## Implementation cost (measured by spike, 2026-07-05)

A read-only spike over `stage2/compiler/` measured what implanting this costs. Two
findings:

**The mechanism (`taxon K = $taxology`) is MEDIUM, contained in the frontend.** The
`Kind` type is a hardcoded 2-variant enum (`pub type Kind = KType | KUnit`,
`ast.kai:329`), and a tparam's kind is encoded by *suffixing its name with `#Unit`* (a
string hack, not a field). Real blast radius (excluding the `KUnitV` false positive —
the KIR's Unit value, unrelated): **~29 sites in 3 files** (`ast.kai`, `parse.kai`,
`infer.kai`) — **no KIR, no codegen**. Generalising the enum + replacing the `#Unit`
string hack with real data is mechanical. The one genuinely delicate piece is the
**per-taxon introducer keyword** (`metric`, `currency`, `perm` at top level) — it puts
state in the LL(1) parser (a table of introducers populated by `taxon … with intro`),
the same mechanism `opaque`/`region` use but stateful.

**A unification engine is CHEAP if it does not solve equations.** The abelian engine
(`$abelian_group`) is **~350–450 LOC** in `infer.kai`: normal form (`UTable` +
`unit_to_table` + `utable_to_tree`, ~177 LOC), unification (`unify_unit` +
`unify_unit_diff`, ~78 LOC), and a **2-line hook** in `unify_env` (the `TyDimT` case).
The expensive part is `unify_unit_diff` — it *solves linear integer equations* to unify
unit variables (`m·? = m²·s`). **`$composition` needs none of that**: its unification
is "do the field sequences match, do the sizes sum to the expected total" — comparison
+ addition, no solver, no `UTable`. Estimate: **~150–200 LOC**, roughly ⅓–½ the abelian
engine.

**Consequences for a plan:**
- **`$abelian_group` already exists** — it *is* the `unit`/Measure engine
  (`unify_unit`, `TyDimT`). `unit` is preserved as-is; the system wraps the existing
  engine, does not rewrite it.
- **Start with `$composition` + `Layout`**: cheapest engine (no solver), strongest
  cases (binaries + waterfall fintech + aerospace telemetry), and it follows the `unit`
  mold step for step (`TyDimT` → `TyComp`, same hook point). Low risk.
- The system's total cost is **per engine**, not per taxon — and Composition, the one
  with ★★★ cases, is the cheapest engine to add.

## Surface naming — why `taxon`/`taxology`, not `kind`/`theory`

`kind` and `theory` are the natural words, but both collide with popular identifiers.
`kind` is the AST/token discriminant field name — the compiler uses it ~1350 times
(`.kind`, `kind:`, `kind(`), and user code uses it as a record label. `theory` is
common in math/physics/ML domains. Making either a keyword (even a soft one) gives the
word a split personality and would break the self-compiling compiler. (`type`/`effect`/
`unit` are *hard* keywords reserved at birth; they never competed with identifiers, so
they are not a precedent `kind` could follow.)

`taxon` and `taxology` are verified collision-free across the compiler, stdlib, and
examples, and are taxonomically exact: a *taxonomy* is a classification system, a
*taxon* one class within it. The user writes `taxon` once per kind declaration; the
word never appears in `[u: Metric]` or `Real<meter>`. `taxology` is catalog-surface
(and future user-assemblable from the property menu), so it is first-class, not hidden
plumbing.

## Open questions

- Per-taxon introducer keywords (`metric`, `currency`, `perm`) at top level need the
  contextual-keyword mechanism, populated by the `taxon … with intro` declarations read
  so far — a stateful table in the LL(1) parser (precedent: `opaque`/`region`, but
  those are not stateful). This is the delicate piece the spike flagged.
- Whether user-assembled taxologies (`taxology $mine = { assoc, commut, idempotent }`)
  ship in the first cut or are deferred — the mechanism (closed property menu +
  decidable-combination check) is designed, but the standard aliases cover the known
  cases, so user assembly can land later without redesign.
- Whether `Effect`'s engine is even exposed as a `$rows` token, or is purely internal
  with `priv taxon Effect` referencing it by a stdlib-core-only path.
- Cost order: `$composition` is the cheapest new engine (trivial unification);
  `$abelian_group` already exists (Measure). `$semilattice` is the next real add.

## References

- `docs/kinds.md` — the current two-kind system this generalises.
- `docs/units-of-measure.md` — the `$abelian_group` engine (`unify_unit`, Kennedy).
- `docs/layout-kind-design.md` — `$composition` / `Layout` detail (incl. TLV).
- `docs/effects.md` — the `$rows` machinery (why `Effect` is private, not a public
  taxology: row variables + runtime handlers exceed a catalog taxology).
- CLAUDE.md Tier 1 #3 — decidable HM, the constraint that closes the property menu.
