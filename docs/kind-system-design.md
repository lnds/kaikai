# The kind system — kinds, theories, habitants (consolidated design proposal)

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
| **kind** | a name plus a classification algebra (`kind Metric : AbelianGroup`) | keyword `kind`, contextual |
| **theory** | a *classification algebra*: an assembled, decidable unification theory (`theory AbelianGroup = { assoc, commut, inverse, identity }`) | keyword `theory`, catalog-only |
| **property** | one algebraic law a theory may include (`assoc`, `commut`, `idempotent`, `inverse`, `identity`, …) | contextual, inside `{ }` |
| **habitant** | a member of a kind (`meter` in `Metric`, `USD` in `Money`) | per-kind introducer word |
| **introducer** | the keyword that declares a kind's habitants (`unit`, `metric`, `currency`, `perm`, `layout`) | keyword, one per kind |

So "`kind Metric : AbelianGroup`" reads "Metric is a kind under the abelian-group
theory". `kind` and `theory` are the natural type-system words; both were briefly
avoided over identifier-collision fears, but the collision is illusory: `kind` is a
*contextual* keyword (keyword only in item-declaration position, ordinary identifier
everywhere else — the same shape-recognised trick `region` already uses), so the
~1055 internal `.kind` field uses survive by construction; `theory` collides once in
the whole tree and is a catalog-only hard keyword. See *Surface naming* below.

## The three levels

```
value  : type      42 : Int                              (value    : type)
type   : kind      km : Measure   /   type Person = {…}   (habitant : kind)
kind   : theory    kind Measure : AbelianGroup           (kind     : theory)
theory   { props } AbelianGroup = { assoc, commut, inverse, identity }
```

- A **value** has a **type** (`42 : Int`).
- A **type/habitant** has a **kind** (`Int : Type`, `km : Measure`).
- A **kind** is a name **classified by a theory** (`Measure : AbelianGroup`).
- A **theory** is a decidable unification algebra, assembled from a **closed menu
  of properties**.

## What a theory is (and is NOT)

A theory decides **type equality by unification, at compile time**. It is NOT a way
to compute values.

- `AbelianGroup` decides that `m·s` unifies with `s·m` — it exists so the *type
  checker* treats them as the same type, not so you can multiply units.
- A theory is **phantom**: it guides unification and is erased at runtime (except
  where it parametrizes codegen, e.g. `Composition`'s byte-swap — still no runtime
  value cost).

**The hard rule (why properties are a closed menu).** A theory is a decidable
E-unification algorithm. E-unification over *arbitrary* equational theories is
**indecidable**, and it is itself indecidable to tell whether a given theory's
unification terminates — so a user cannot write the equations of a theory directly.
Instead a theory is **assembled from a closed menu of properties** the compiler
knows how to decide (see next section). Decidability is verified *by construction*:
the compiler owns a terminating unification engine for each recognised
property-combination, and an unrecognised or provably-indecidable combination is a
**compile error**, not a hung type checker. This is the same discipline that forbids
type-class resolution and the borrow checker: bounded, decidable, checkable.

**Two interacting operations → out.** Every viable theory combines laws over
**one** operation. Two interacting operations (a ring's `+` and `×` with
distributivity) make unification indecidable. `AbelianGroup` (one op) is in; a ring
is out. This is the cheap admission filter: one op → candidate; two interacting → no.
`distributes_over` is deliberately absent from the property menu for exactly this
reason.

**One theory, one engine — no facades.** A theory in the catalog names a
unification engine that exists. It is never a *label* over a formation-guard that
reuses another theory's engine under a different name. Two theories may not share an
engine while claiming to be distinct: either they are genuinely the same mechanism
(then they are the *same theory*, one engine serving several kinds — as
`AbelianGroup` serves `Measure`), or they need different engines (then they are
different theories, each with its own). Reusing engine X for kind Y while presenting
Y as a separate theory is the facade pattern, and it is forbidden. A kind whose theory
has no engine of its own is not shippable — the engine is *built*, not faked. The one
legitimate reuse is the **HM core itself**: the core is not a catalog theory, it is
the substrate every theory extends (AbelianGroup adds AC; the shape engine adds
witness-binding; EffectRow adds row unification). A kind whose engine *is* the core —
first-order syntactic equality, nothing added — reuses the core the way `Type` does,
and that is honest, not a facade.

### Real engines vs. catalog claims (the honesty map)

An audit of the compiler (`stage2/compiler/`) against the catalog found the catalog
claimed more engines than exist. The truth, and the target:

| Catalog entry | Engine status | Target |
|---|---|---|
| `HindleyMilner` (`Type`) | real — the core structural unifier | keep |
| `EffectRow` (`Effect`) | real — row unification (`unify_row`) | keep |
| `AbelianGroup` (`Measure`) | real — Kennedy 1996 (`unify_abelian`) | keep |
| shape engine (`Shape`) | real — witness-binding of arity-1 constructors (`shape_bind`/`shape_witness_of`, a dedicated `TyShapeApp` node) | **shipped** as its own theory `ConstructorApp`; the `Structural` label is dissolved |
| `Module` (`Currency`) | **real** — `unify_dim` dispatches by theory; Module kinds unify by nominal atom equality (`unify_nominal`) with structural formation (no products/powers), and `scale` lives in operation signatures, not the kind algebra | keep |
| `Composition` (`Layout`) | **real** — `unify_dim` dispatches Composition kinds to the nominal slot verdict (`unify_nominal`, shared with Module) with structural formation (no products/powers); load-bearing order and the summed measure are formation invariants of the composed type, not part of the slot's unification identity | keep |
| `Region` (theory `Nominal`) | **no engine yet** — no `TyRegion`/`TyBranded` node; region identity reuses `TyDim` and falls into `unify_abelian` | catalog is honest (`Nominal`, its own theory, no longer sharing `Shape`'s label); the engine itself is still to **build** |
| `Dim` (engine = the HM core) | **real** — `unify_dim` dispatches first-order kinds (Module, `Dim`) to identity-plus-binding; positional `<m, n>` lists (`UDims`) unify point to point and never touch the abelian table | **shipped** — the typer half; the fixed-width `Vec[t]<n>` representation is the codegen follow-up |
| `Functorial` (protocol laws) | **not a unification engine at all** — `law_checks.kai` synthesizes `check` blocks, reparses, runs them as property tests | **shipped** — no longer a theory; a *protocol-law set* in its own namespace, verified by generated property checks |

The principle is **build, not collapse**: a facade is paid down by writing the engine
its theory always implied, not by deleting the theory from the design. The one entry
that leaves the theory catalog is `Functorial` — not for lack of an engine, but
because it is categorically not a unification theory. `Dim` (below) is the second kind
whose engine *is* the HM core, honestly.

### `Dim` — the shape-index kind, engine = the HM core

`Dim` classifies static shape indices: its habitants are `Int` values (`<3>`), `with`
form (b). `<3>` unifies with `<3>`, never with `<4>` — **first-order value
equality**. `Vec`/`Matrix`/`Tensor` are *types over* `Dim`, not kinds
(`type Matrix[T]<m: Dim, n: Dim>`), carrying one or more `Dim` habitants in `<>`,
exactly as `Money` is a type over `Currency`.

`Dim`'s engine is **not** the shape engine and **not** the abelian one:

- Not shape: `shape_bind` binds a constructor *metavariable* by decomposing a type
  application `D[A]` into head + argument (`shape_witness_of` yields an unapplied
  constructor, `shape_reapply` re-applies it). `<3>` has no head, no argument, no
  variable to bind — it is a closed value. Forcing a fake witness (`witness_of(<3>) =
  TyCon("3",[])`) would be the forbidden makeup.
- Not abelian: the `Real<m^2>` exponent is compared *inside* the abelian solver
  (exponent subtraction, pivot division — Kennedy), not by a plain `Int ==`. Routing
  `<3>` through it would model a dimension as a free abelian group on one generator
  with exponent 3 — the `Module≡AbelianGroup` facade again.

`Dim`'s engine is **first-order syntactic equality with variable binding** (Robinson,
the thinnest fragment: constant ~ constant ⟹ equality; variable ~ constant ⟹ bind),
which the HM core already provides — the same `==` the core uses on type-var ids,
applied to the index *value*. So `Dim` reuses the core (legitimate, like `Type`), not
a catalog theory (forbidden). The `<m,n>·<n,p> → <m,p>` shape rule lives in the
operation's signature, not in a theory. The seam is `unify_dim`, which now dispatches
by the kind's theory (Module kinds go to nominal atom equality, measure symbols to
abelian); a value-index habitant slots into the same dispatch with first-order
equality.

## Theories are assembled from a closed property menu

A theory is **not** an atomic primitive picked from a fixed list — it is a named
**assembly of properties** from a closed menu the compiler can decide:

```kaikai
# stdlib/core: the standard theories are named assemblies
theory AbelianGroup = { assoc, commut, inverse, identity }
theory Semilattice   = { assoc, commut, idempotent }          # no inverse
theory Composition   = { assoc, measure }                     # non-commutative, summed measure
```

The **properties** are the closed catalog now — `assoc`, `commut`, `idempotent`,
`inverse`, `identity`, `measure`, `rows`, … — each a law the compiler knows how to
unify under. The compiler recognises which *combinations* have a terminating engine
(abelian group = assoc+commut+inverse+identity; semilattice = assoc+commut+idempotent;
etc.). A combination with no known-decidable engine is rejected at the declaration.

**The `builtin` body.** Two theories have no property set at all:
`theory HindleyMilner = builtin` and `theory EffectRow = builtin`. A `builtin` body
means the engine is the compiler core itself — HM unification for `Type`, row
unification for `Effect` — rather than a hand-written `unify_<theory>`. This is the
same hardcoded-engine discipline taken to its root: the compiler *is* the hardcode.
`builtin` theories and the kinds over them (`kind Type : HindleyMilner with type`,
`kind Effect : EffectRow with effect`) are provided by the language, closed, and not
user-declarable; declaring `theory X = builtin` or redeclaring `kind Type` /
`kind Effect` in user code is a compile error. The catalog entries are declarative
surface only — type and effect unification do not route through a kind dispatcher.

**A future user can create a theory — by assembling, never by writing algebra.**
`theory Mine = { assoc, commut, idempotent }` is legal (the compiler recognises it
as a semilattice engine); `theory Bad = { assoc, distributes_over(...) }` is a
compile error (no decidable engine, two interacting ops). The user composes from the
menu; they never write unification equations. This keeps the door open without
crossing the Tier 1 #3 red line.

The standard names (`AbelianGroup`, `Semilattice`, `Composition`) are thus
**aliases for common property assemblies**, provided by `stdlib/core` for convenience
and readability — not opaque atoms.

Notation: **PascalCase, no sigil**. A theory name is written like a type
(`AbelianGroup`), introduced by the `theory` keyword — the keyword marks what it is,
so no `$` sigil is needed (and `$name` is reserved for the `$extern_handler`-style
compiler intrinsics; a theory is not one). A theory is still *assembled from the
property menu, never hand-written*: `theory MyThing = { assoc, distributes_over(…) }`
is a compile error (no decidable engine), but the guard is the closed property menu,
not a naming sigil.

**Not expressible** (indecidable or not a unification problem): rings/fields (two
interacting ops), arbitrary graphs / DAGs (relational, not algebraic — runtime data),
dependent-types-with-effects (the F* unified theory — powerful but indecidable,
rejected for the same reason as the borrow checker and overloading). DAG, workflow
engines, crypto/number-theory, JSON binding: all **runtime or structural**, not
theories — a theory unifies types, it does not compute or relate graphs.

## Where the catalog lives (and why it stays decidable)

Two layers:

1. **The property engines** — compiler code (`stage2/compiler/`), like today's
   `unify_unit` (the abelian-group engine for `Measure`) in `infer.kai`. NOT
   declarable in kaikai. The engine catalog *is* which property-combinations the typer
   can unify — closed by construction, only what was programmed and proven decidable.

2. **The theory registry** — one stdlib file, `stdlib/core/theories.kai`, names
   the standard property assemblies and gives each visibility:

   ```kaikai
   theory Nominal            = builtin                              # Region's engine — closed
   theory ConstructorApp     = builtin                              # Shape's engine — closed
   pub  theory AbelianGroup = { assoc, commut, inverse, identity }
   pub  theory Semilattice   = { assoc, commut, idempotent }
   pub  theory Composition   = { assoc, measure }
   theory EffectRow          = builtin                              # Effect's engine — closed
   ```

   **Four theories are `builtin`** (`HindleyMilner`, `EffectRow`, `Nominal`,
   `ConstructorApp`), for the same reason: their engine is the compiler core and admits
   no user-declared kind. `Type` is the sole kind over the HM core — a user-declared
   isolated type universe (`kind Validated : HindleyMilner`) would add only
   *isolation*, which phantom types / newtypes already approximate. `Effect` is the sole rows kind (its machinery is
   built-in). The **public** theories are exactly those where a new user kind adds an
   algebra nothing else gives — Money's abelian group, Perm's idempotent join,
   Waterfall's summed measure. The admission criterion:

   > **A theory is assemblable if declaring new kinds over it yields an algebra nothing
   > else provides; `builtin` if its only useful kind is already the compiler core.**

### The `theory` declaration is module-restricted

`theory X = { … }` is a declaration form **legal only inside the
`stdlib/core/theories` module** — a deliberate special-case in the parser/resolver,
like other context-restricted forms, not a general capability. A `theory` in any
other file is a compile error: "`theory` declarations are only allowed in the
theory catalog". This keeps the *standard* assemblies in one home. (Future
user-assembled theories, if enabled, would relax this to "any file, but only from
the property menu" — the property menu stays the closed part, not the location.)

## Kinds — declaration and visibility

A kind is declared over a **public** theory. Kinds obey ordinary `pub`/`priv`
visibility — the **same** mechanism (and the same pub-leak validator) that governs
every other symbol. No new `intrinsic`/`builtin` marker: visibility is the barrier.

```kaikai
# stdlib/core/kinds.kai
kind Type    : HindleyMilner with type               # builtin engine, closed
kind Effect  : EffectRow with effect                 # builtin engine, closed → user cannot classify a kind by it
pub  kind Measure   : AbelianGroup with unit
pub  kind Currency  : Module over T with currency        # NOT AbelianGroup; scalar is the carrier T — see below
pub  kind Perm      : Semilattice  with perm
pub  kind Layout    : Composition over Int      with layout { be le }
pub  kind Waterfall : Composition over Rational with pct
```

> **Money is `Module`, not `AbelianGroup` — `USD²` is the bite.** An earlier draft wrote
> `Money = AbelianGroup with currency`. That is WRONG: `AbelianGroup` is a *multiplicative*
> group over its habitants — it gives `unit × unit` (that is why `m/s²` and `m·s` work for
> physics), which for money means `USD × USD = USD²`, nonsense. Money admits `USD + USD`,
> scalar-mult by a dimensionless number (`3 · USD`), but NOT `USD × USD`. That is a **module /
> vector space**: an additive group of elements plus scalar multiplication from an external
> ring, no element×element product. See *The surface: `[carrier]`, `<habitant>`, `over`, `with`*
> and *When to separate two abelian kinds* below.

**Declared-but-unusable because the engine is `builtin`.** `Effect` *is* a first-class
kind — real, declared, in the registry, listed by `kai info kinds` — but its theory
`EffectRow` is `builtin`: the engine is the compiler core (row unification), closed by
construction. A user who writes `kind X : EffectRow` gets `EffectRow is a builtin theory
and cannot classify a user-declared kind`. The same holds for `Type` over `HindleyMilner`.
The compiler knows `Effect`/`EffectRow` exist (to give a clear error), but a `builtin`
theory admits no new kinds over it. This answers "how is a kind declared yet unusable":
the barrier is the closed builtin engine, not visibility.

A user may declare kinds over **non-builtin** theories only:

```kaikai
kind Metric : AbelianGroup with metric      # ✓ AbelianGroup is assemblable
kind Perm   : Semilattice with perm          # ✓
# kind Access : Nominal                       # ✗ Nominal is builtin (closed)
# kind Bad   : EffectRow                      # ✗ EffectRow is builtin (closed)
```

Isolated type universes (a `Validated` vs `Unvalidated` kind) are **not** available —
every builtin theory is closed, so a user cannot declare a kind over one. Use a phantom type /
newtype for that isolation; the kind would add nothing a wrapper does not.

## Habitants — the introducer and its policy

`with <introducer>` gives the keyword that declares the kind's habitants. **One
introducer word declares habitants of exactly one kind** — the introducer *is* the
kind at the declaration site. `unit m` declares `m` in `Measure`; `metric meter`
(from `kind Metric : AbelianGroup with metric`) declares `meter` in `Metric`:

```kaikai
kind Measure  : AbelianGroup with unit
kind Metric   : AbelianGroup with metric

unit m                                          # m declared in Measure
metric meter                                    # meter declared in Metric
```

**A habitant *symbol* may live in more than one kind.** The introducer disambiguates
at the *declaration* site, but the same word can be declared through different
introducers into different kinds — `unit USD` and `cur USD` (from
`kind Currency : AbelianGroup with cur`) are two distinct habitants that happen to
share the symbol `USD`. This is deliberate: a currency is a legitimate `Measure`
habitant *and* can be a `Currency` habitant, depending on which algebra the user
wants at a given site. Resolution at the *use* site is the next section.

`unit` is no longer special — it is `Measure`'s registered introducer, exactly as
`metric`/`cur` are their kinds'. (Today `unit` is still a hard keyword; the v2+
direction is to make it an introducer registered by the kind declaration, like every
other introducer, so it stops being a keyword. See *Open questions*.)

## Habitant resolution at the use site

A habitant symbol in `<>` (`Real<USD>`) must resolve to exactly one kind. When the
symbol lives in only one kind, that is the answer. When it lives in several
(`unit USD` *and* `cur USD`), resolution follows a fixed precedence — no magic, no
order-dependence:

```kaikai
kind Currency : AbelianGroup with cur
kind Measure  : AbelianGroup with unit

unit USD                                   # USD in Measure
cur  USD                                   # USD ALSO in Currency

# 1. Explicit qualification wins — Kind.habitant (or introducer.habitant):
let a : Real<Currency.USD> = 10.0<Currency.USD>    # unambiguous → Currency
let b : Real<cur.USD>      = 10.0<cur.USD>         # introducer form, same effect

# 2. `use kind` sets the module default for a bare habitant:
use kind Currency
let c : Real<USD> = 10.0<USD>              # bare USD ⇒ Currency (the used kind)

# 3. No qualification, no `use kind`, and the symbol is AMBIGUOUS:
let d : Real<USD> = 10.0<USD>              # ✗ ERROR: `USD` is declared in
                                           #   Currency and Measure — qualify
                                           #   (Currency.USD) or `use kind`.
```

**The precedence: qualification `>` `use kind` `>` unique-symbol.** If none of the
first three resolve a multi-kind symbol, it is a **compile error demanding
disambiguation** — never a silent "last declared wins". Order-of-declaration must
NOT change a program's meaning; a bare `<USD>` that is ambiguous is rejected, not
guessed. This is the same discipline kaikai already applies to ambiguous module
names, and it keeps the surface decidable and surprise-free (Tier 1 #3, and the
"few forms, clear intent" rule).

**Why symbols may collide across kinds — the `USD/kWh` case.** Money and physical
units must be able to compose: `USD/kWh` (energy price), `USD/km` (freight),
`USD/hour` (wage) are legitimate — `(USD/kWh) * kWh = USD` must type-check. That only
works if `USD` and `kWh` inhabit the **same** abelian kind, so their algebra
cancels. So a user who needs money-with-physics declares both in `Measure`
(`unit USD`, `unit kWh` → they compose). A user who wants money *isolated* from
physics declares `USD` in a separate `Currency` kind — then `USD` (Currency) does NOT
cancel against `kWh` (Measure), which is correct if that isolation is the intent. The
kind a symbol lives in is a user choice about *which algebra it participates in*; the
resolution rules above just make that choice legible at each use site.

> **Contrast with strict isolation (Metric/Imperial).** `meter + foot` is always a
> bug, so `meter` (Metric) and `foot` (Imperial) live in separate kinds and never
> unify — no `use kind` makes them compose. The difference from `USD`/`kWh` is
> intent: cross-kind composition is *legitimate* for money+physics (same kind) and
> *always wrong* for metric+imperial (separate kinds). See *When to separate two
> abelian kinds* below.

Two properties of each habitant set fall out of the **kind** (not always the
theory):

**Structure** — a habitant may be atomic or structured:

| Kind | introducer | habitant | body? |
|---|---|---|---|
| Type | `type` | a type | yes (fields/variants) |
| Effect | `effect` | an effect | yes (operations) |
| Measure / Money / Perm | `unit` / `currency` / `perm` | a unit/currency/permission | no (atom) |
| Layout | `layout` | endianness | no (atom) |

`type` and `effect` are just the habitant introducers of their (built-in) kinds —
structured habitants — exactly as `unit` is Measure's — an atomic habitant. This
de-magics `type`/`effect`: they are the `with`-introducers of built-in kinds.

**Open vs closed** — who declares the habitants:

- **Open** (`with intro`, no block): the **user** declares atoms — `unit km`,
  `currency USD`, `perm read`. For theories whose atoms are **opaque symbols** the
  algebra manipulates without knowing them (`km·s` works without the engine knowing
  what a metre is). Unlimited.
- **Closed** (`with intro { a b }`): the block **selects** from atoms the engine
  already knows, each carrying built-in semantics (`be` = byte-swap). The user adds
  none and defines no semantics (that would extend the engine — forbidden). A closed
  block naming an atom the engine does not know (`{ be le xyz }`) is an error.

Policy is per-**kind**: `Layout` is closed (`be`/`le` fixed); a hypothetical `Row`
over `Composition` (UI columns) would be open (arbitrary widths). Same theory,
different habitant policy, set by the kind.

**Value — opaque vs measure-carrying (a property of the THEORY):** whether a
habitant carries a numeric value depends on whether its theory *sums* one.

- **Opaque** (`AbelianGroup`, `Semilattice`): the habitant is a bare symbol —
  `unit km`, NOT `unit km = 1000`. The algebra manipulates symbols (`km/h`, `km²`,
  `read ∪ write`) but **never sums magnitudes**. The km↔metre relation lives in a
  separate conversion function, not the kind.
- **Measure-carrying** (`Composition`): the habitant **declares a measure**, because
  the theory *sums* it (`size(a·b) = size(a) + size(b)` is its essence — the
  `measure` property). To verify "tranches sum to 100%" the compiler must know
  `pct70 = 70`. So:

  ```kaikai
  kind Waterfall : Composition with pct
  pct pct70 = 70        # the habitant DECLARES its measure; Composition sums it
  pct pct20 = 20
  pct pct10 = 10
  ```

  `Layout`'s `be`/`le` carry the measure implicitly via the field type
  (`U32` → 4 bytes). A bare `pct pct70` (no `= 70`) would be an opaque symbol meaning
  nothing — the name "pct70" is not self-describing.

So a theory has **four** derived properties: unification rule, habitant policy
(open/closed), habitant structure (atomic/structured), and habitant value
(opaque/measure-carrying). `Composition` is the one whose habitants carry a measure,
precisely because summing that measure is what it does.

### How an intrinsic theory operates on user-declared habitants

A subtle point: `Layout` feels coherent because `Composition` **and** `be`/`le` are
all intrinsic — the compiler knows the theory, the habitants, and their codegen
semantics. But `kind Waterfall : Composition with pct` + `pct pct70 = 70` mixes an
**intrinsic theory** with **user-declared habitants**. How does a built-in engine
operate on `pct70`, which it never saw?

**A theory defines an interface its habitants must satisfy; it does not know the
habitants, it receives from them what it needs.** `Composition` says: "each habitant
gives me a number — its measure — and I sum them." Two ways to satisfy it:

- **Closed habitants** (`be`) satisfy the interface **from the compiler** — built-in,
  intrinsic to the engine (their measure/semantics ship with `Composition`).
- **Open habitants** (`pct70 = 70`) satisfy it **via the declaration** — the `= 70`
  *is* the user handing the theory the measure it asks for.

Declaring your own kind over an intrinsic theory does **not** extend the theory
(that would be defining a unification/codegen algorithm — forbidden, indecidable). It
**feeds** the theory's habitants the data the interface requires. The engine stays
100% compiler; the habitant is the user's; the `= N` is the visible contract between
them. That contract is why Waterfall is not magic: nothing about `pct70` is inferred —
the user states its measure, the theory sums it.

### Measure arithmetic must be exact (decidability)

`Composition` *verifies* its sum at compile time ("tranches sum to 100%"). With
**floating-point** measures this is unsound — `0.1 + 0.2 ≠ 0.3` in IEEE-754, so a
legitimate waterfall could fail to compile on a rounding artefact. So a `Composition`
measure must be **integer or exact rational** (`pct70 = 70`, or `1/3`), never a float.
A tolerance-based check (`|sum − 100| < ε`) would inject an arbitrary ε into the type
checker — the same unpredictable "accept/reject by a magic number" that Tier 1 #3
rejects. Exact arithmetic keeps the sum verification decidable and sound.

## The surface: `[carrier]`, `<habitant>`, `over`, `with`

The full declaration form:

```
kind Name : Theory [over SecondDomain] with <habitant-form>
type  M[T]<Kind> = { ... }
# use:  M[Carrier]<habitant>      e.g.  Money[Real]<USD>,  Matrix[Real]<3,4>
```

Four pieces, each with one job.

### `[T]` vs `<habitant>` — two orthogonal axes

A user type over a kind has **two** parameters of different natures, in different brackets:

- **`[Carrier]`** — an ordinary **type** parameter (`[]`, exactly like `Option[T]`). The `[]`
  list holds ONLY `Type` params (the carrier), with **no protocol bound** (see next). Its
  *name* is a plain convention — but a meaningful convention: name it for what the theory
  expects, not a bare `T`. Money's carrier is a number, so name it `Num`
  (`type Money[Num]<Currency>`); a bare `T` reads as "anything" and hides the intent. The name
  is documentation, not a bound — the compiler enforces the numeric requirement through the
  theory (below), the name just makes it legible. It is the type the value is built from:
  `Money[Decimal]`, `Money[BigInt]`, `Matrix[Real]`.
- **`<habitant>`** — the kind **habitant** (`<>`, exactly like `Real<m>`). A currency or a
  dimension is **not a type** — it is a classifying habitant, so it goes in `<>`. Kinds go in
  `<>`, never in `[]`; only `Type` goes in `[]`.

`Money[Decimal]<USD>` reads "money with carrier `Decimal`, currency `USD`". The two axes are
independent: the carrier is a plain generic in `[]`, the habitant is the kind marker in `<>`.
This is what lets a kind habitant live on a **library/user type** (`Money`) without the unit
machinery being wired to native heads — the number enters as an ordinary carrier generic, and
only the wrapper type (`Money`, `Matrix`) carries the `<>`.

```kaikai
type Money[Num]<Currency> = { amount: Num }
fn sum(a, v : Money[Real]<USD>) = a + v          # a inferred Money[Real]<USD>; a+v needs same currency+carrier
```

> **v1 status (2026-07-22):** the use-site surface (`Money[Decimal]<USD>`, carrier in
> `[]`, habitant in `<>`) is shipped; the shipped declaration is the transparent
> carrier alias `type Money[t] = t` with the habitant applied at the use site, so the
> carrier's own arithmetic rides through unchanged. The declaration-side habitant slot
> (`type M[T]<Kind> = { ... }` on a record body) has no surface yet.

### No protocol bound on the carrier — the theory restricts it

The carrier takes **no** protocol bound in the surface. `type M[Num: Add]` is a parse error
("type-parameter kind must be `Type` or `Measure`") — a bound on a `type` declaration is the
Haskell constraint-propagation the language vetoes (a `Num: Add` would travel with every
signature that touches `M[Num]`). So the carrier is named for intent (`Num`), not bounded.

But `Money[String]<USD>` must not be a valid type — a string is not money. The requirement
"the carrier is a number" is real; it just does not live as a bound on the declaration. **It
lives in the theory.** `Module` is "additive group + scalar-from-a-ring", so its carrier/scalar
must support the ring operations by the theory's own definition. A kind over `Module`
therefore validates its carrier **at construction, per concrete instantiation**: `Money[Int]<USD>`
checks that `Int` provides what `Module` requires (it does → ✓); `Money[String]<USD>` checks
`String` (it does not → **error at construction**, not a deferred failure-on-use).

The sharp line vs Haskell: this validation is **local and non-propagating**. It is an
`impl`-table check at the concrete construction site — the same `concrete ⇒ check locally /
variable ⇒ wait for the instance` discipline as literal minting. No constraint travels with a
signature; a generic `fn combine[C](a: Money[C], b: Money[C])` still waits for the concrete `C`
(and the theory's carrier check fires when `C` is pinned), rather than propagating a `C: Numeric`
up the call graph. So the theory gives what a bound would — `Money[String]` is impossible — without
the constraint resolution Tier 1 #3 rejects.

And there is **no algebraic-protocol hierarchy** (`Numeric extends Ring…` = Haskell superclasses,
vetoed): the theory names the operations its carrier must support (via the closed property menu),
not a subclass chain. Overlapping small protocols (`Add`, `Mul` on each numeric carrier) are what
the theory's carrier check consults, per concrete instance.

### `over R` — the second domain (present iff the structure crosses two type domains)

`over R` comes from "a module **over** a ring R": it names a **second, distinct type domain**
the operation crosses. Present exactly when the theory has two domains, absent otherwise:

- `Module` (Money): elements (amounts, the carrier) **and** scalars (a ring) — the two-domain
  structure. `over T` names the scalar; for Money the scalar IS the carrier `T` (you scale a
  `Money[Decimal]` by a `Decimal`, `3` minted to the carrier). A *general* module allows scalar
  ≠ element (a vector space of matrices over reals) — the surface can express that when needed
  by naming a scalar distinct from `[T]`; Money simply reuses `T`. Either way `over` is what
  marks the second domain, and (like the carrier) carries no protocol bound — monomorphisation
  checks the scalar's ops per concrete instance.

  > **Why not `kind Currency[T: Numeric] : Module over T`?** A module mathematically
  > *requires* its scalar to be a ring, so it is tempting to spell that as a bound on the
  > kind. Don't: that predicate belongs to the **theory**, not the kind declaration. `Module`
  > *is* "additive group + scalar-from-a-ring" by construction — the `scale` property has no
  > meaning without a ring to draw scalars from, so the ring requirement is baked into the
  > theory. A `[T: Numeric]` on the kind would (a) be redundant with what `Module` already
  > guarantees and (b) reintroduce exactly the constraint-propagation the carrier rule forbids
  > — a bound that travels with the signature, the Haskell line Tier 1 #3 rejects. The scalar's
  > ring-ness is enforced the same way the carrier's summability is: monomorphisation at the
  > use, not a bound on the declaration. `Money[String]<USD>` constructs but cannot scale.
- `Composition` (Layout, Waterfall): elements (fields/tranches) **and** a summed **measure**
  (a number). `over Int` (Layout, byte sizes) / `over Rational` (Waterfall, exact %s). Here
  `over` names the measure type, an additive second domain, not a multiplicative scalar.
- Single-domain theories take **no** `over`: `AbelianGroup`/Measure (`unit × unit → unit`,
  one domain — the rich `m/s²` algebra is still one domain), `Semilattice`.

The sharp line: **`over` is not "more than one operation" — it is "does the operation cross two
distinct type domains?"** UoM has `·`, `/`, `^` but all within one domain (units) → no `over`.
A module crosses element↔scalar → `over`.

### `with <form>` — what a habitant is (one keyword, four forms)

`with` always declares what a habitant *is*; the theory behind it decides how the habitant
behaves (opaque vs measure-carrying, open vs closed — the per-theory properties above):

- **(a) introducer → symbol** — `with currency` ⇒ `currency USD`. An opaque, user-declared
  symbol, open set. Money, Perm, Measure (`unit`).
- **(b) type → value** — `with Int`, `with String`. Habitants **are values** of that type,
  written directly (`<3>`, `<"tag">`), not declared; the type restricts validity
  (`Matrix[Real]<3,4>` ok, `<USD,4>` error). The one kind here is `kind Dim with Int`
  — a habitant is a single `Int`. `Vec`, `Matrix`, `Tensor` are **types over `Dim`**, not kinds:
  `type Vec[T]<n: Dim>`, `type Matrix[T]<m: Dim, n: Dim>` — they carry one or more `Dim`
  habitants in `<>`, exactly as `Money` is a type over `Currency` and `Real` a type over
  `Measure`. `Dim` alone owns habitants (`Int` values); `Matrix` owns none, it consumes `Dim`'s.
  `Dim`'s engine is the **HM core** (first-order value equality), not a catalog theory — see
  *`Dim` — the shape-index kind* above. The inner-index cancellation of matmul (`<m,n>·<n,p>`
  needs `n ≡ n`) is that equality, and the `<m,n>·<n,p> → <m,p>` shape rule lives in the
  operation's **signature**, like `u^2` in `area`'s — not in a theory. So no `Dimensional`
  theory is introduced: value-equality over `Int` yields no algebra the core does not already
  give, which the admission criterion below rejects.
- **(c) introducer → symbol-with-measure** — `with pct` ⇒ `pct pct70 = 70`. A declared symbol
  that **carries a number** the theory sums. Waterfall. The `= N` is the habitant declaring
  its measure (a measure-carrying theory).
- **(d) closed set** — `with layout { be le }`. Habitants are a **fixed** set the engine knows
  (`be`/`le`), not user-extendable. Layout endianness.

One keyword, four forms; which applies follows from the theory's properties. Integer literals
in `<>` already work today (`Real<m^2>`, exponent = literal integer), so `<3>` is not a new
capability.

### Practical mode — decidable checks compile-time, the rest at runtime

The catalog's "two interacting operations = indecidable = out" rule is not the last word for a
user's declared structure. The language lets you **declare** the mathematical structure of a
type and enforces it in a split: **compile-time for what is decidable, runtime for what is
not.** `Module`'s scalar-distributivity is never *proven* statically (that would be F*); what
the compiler checks statically is which operations exist and whether habitants/carriers are
compatible, and it **inserts runtime checks** for the rest.

For Money: `Money<USD> + Money<USD>` ✓ and `+ Money<EUR>` ✗ and `Money × Money` ✗ (no internal
product → no `USD²`) are **compile-time**; a currency read from a file is dynamic, so the
value→type boundary lives in `parse(carrier, code) : Option[Money<...>]` (like `int.parse`, one
type, no parallel "dynamic Money"). For Matrix: static shapes `Matrix<3,4> * Matrix<4,5>` check
at compile time (inner `4` cancels); a matrix of runtime-unknown shape checks at runtime (a trap
like an out-of-bounds index). Same split both cases — this is the "practicidad" that makes
indecidable structures usable without weakening Tier 1 #3's static core.

The Matrix split falls out of `Dim`'s engine being the HM core: the core decides only
equality, so the decidable half is exactly the equalities — shape identity, matmul's inner-index
cancellation, transpose. Everything arithmetic — `reshape` (`m·n = p·q`), `concat` (`n+k`),
broadcast — is *not* an equality, so no engine covers it and it lands on the runtime side.
That is the tell that `Matrix` needs no theory at all: there is no type-level
arithmetic engine to justify one, and inventing a `Dimensional` alias byte-equal to
"the core over values" would be the empty-alias trap the closed menu exists to prevent.

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

The kind and its habitants appear in `<>` (`Real<u>`, `Real<km>`); `[]` holds only `Type`
carriers (`Money[Real]`). The keyword `kind` never appears in *use* positions — only in the
one-line declaration. (The pre-kind surface spelled unit-polymorphism `[u: Measure]`, a
*kind*-bound in `[]`; the kind design moves habitants to `<>` uniformly, so `[]` is `Type`-only.)

## `unit` compatibility

`unit` stays exactly as-is: `unit x` ≡ a `Measure` habitant (its exclusive
introducer), spelled with the existing `TkUnitKw`. All current code (`unit m`,
`unit USD`, `unit kg`) is unchanged — no `: Measure` annotation is ever required,
because `unit` *is* Measure's introducer. A currency uses its own kind's introducer
(`currency USD` after `kind Currency : Module over T with currency`); `unit`
remains Measure-only.

## The hard locks (summary)

1. **The property menu is closed.** Theories are assembled only from the compiler's
   known-decidable properties; an unrecognised combination is a compile error, never a
   hung type checker. Engines live in the compiler.
2. **Standard theories live in one module.** The `theory` form is legal only in
   `stdlib/core/theories`; a `Token` naming no decidable assembly is an error.
3. **Some declared kinds are unusable.** `kind Effect : EffectRow` — real, registered,
   `kai info`-visible, but `EffectRow` is builtin so no user kind may be declared over it.
   Visibility is the barrier, not a new marker.

## When to separate two abelian kinds (Measure/Money was a trap)

Two kinds may share `AbelianGroup` (`Metric` and `Imperial`) but stay isolated
(`meter + foot` = error). When is that separation worth it? The criterion, learned by a
counter-example:

**Separate two abelian kinds only when crossing the domains is *always* an error.**

- **Money is NOT such a case — separating it is harmful.** `USD/kWh` (price per
  energy), `USD/km` (freight), `USD/hour` (wage), `USD/kg` (commodities) are
  *legitimate* combinations of money with physics. A separate `kind Money` would make
  `USD/kWh` a type error — forbidding exactly what real fintech/energy/logistics code
  needs. And `USD + EUR` is *already* rejected inside a single `Measure` (they are
  distinct units). So money belongs in `Measure` as ordinary units: it gets
  `USD+EUR`-rejection **and** `USD/kWh`-composition, the best of both. `kind Money` is
  worse, not better. (The kWh example, `USD/(W*h) * (W*h) = USD`, is the proof: money
  divides by *compound* physical units, which demands one shared abelian algebra.)
- **Metric vs Imperial IS such a case.** `meter + foot` is always a bug — you must
  convert first. The Mars Climate Orbiter (1999, $327M lost) mixed newton-seconds with
  pound-force-seconds. `kind Metric : AbelianGroup` and
  `kind Imperial : AbelianGroup`, isolated, turn that class of bug into a compile
  error.
- **Coordinate frames / reference spaces** are the other real case: screen-space vs
  world-space, camera vs global — summing coordinates across frames without an explicit
  transform is a classic graphics/GIS bug. Separate frames → separate kinds.

Rule: **cross-domain combination legitimate → one kind (Measure); cross-domain
combination always an error → separate kinds (Metric/Imperial, coord frames).** The
user-declared-`AbelianGroup` case is real — it just is not money.

### Worked example — Metric/Imperial

```kaikai
kind Metric   : AbelianGroup with metric
kind Imperial : AbelianGroup with imperial

metric meter
metric second
imperial foot
imperial pound

fn area[u: Metric](w: Real<u>, h: Real<u>) : Real<u^2> = w * h

fn main() : Unit / Stdout = {
  let ok  = 3.0<meter> + 2.0<meter>        # ✓ same kind
  let bad = 3.0<meter> + 2.0<foot>         # ✗ ERROR: distinct measure systems
  Stdout.print("ok")                       #   (Metric vs Imperial never unify)
}
```

Here `meter` and `foot` each live in exactly one kind, so a bare `<meter>` resolves
by the unique-symbol rule with no qualification needed. Qualification
(`<Metric.meter>`) and `use kind Metric` are still *available* — they are how a use
site disambiguates when a symbol is shared across kinds (see *Habitant resolution at
the use site*) — but for a symbol that is unique to one kind they are optional. `kind`
appears only in the two declaration lines.

## Implementation cost (measured by spike, 2026-07-05)

A read-only spike over `stage2/compiler/` measured what implanting this costs. Two
findings:

**The mechanism (`kind K : Theory`) is MEDIUM, contained in the frontend.** The
`Kind` type is a hardcoded 2-variant enum (`pub type Kind = KType | KUnit`,
`ast.kai:329`), and a tparam's kind is encoded by *suffixing its name with `#Unit`* (a
string hack, not a field). Real blast radius (excluding the `KUnitV` false positive —
the KIR's Unit value, unrelated): **~29 sites in 3 files** (`ast.kai`, `parse.kai`,
`infer.kai`) — **no KIR, no codegen**. Generalising the enum + replacing the `#Unit`
string hack with real data is mechanical. The one genuinely delicate piece is the
**per-kind introducer keyword** (`metric`, `currency`, `perm` at top level) — it puts
state in the LL(1) parser (a table of introducers populated by `kind … with intro`),
the same mechanism `opaque`/`region` use but stateful.

**A unification engine is CHEAP if it does not solve equations.** The abelian engine
(`AbelianGroup`) is **~350–450 LOC** in `infer.kai`: normal form (`UTable` +
`unit_to_table` + `utable_to_tree`, ~177 LOC), unification (`unify_unit` +
`unify_unit_diff`, ~78 LOC), and a **2-line hook** in `unify_env` (the `TyDimT` case).
The expensive part is `unify_unit_diff` — it *solves linear integer equations* to unify
unit variables (`m·? = m²·s`). **`Composition` needs none of that**: its unification
is "do the field sequences match, do the sizes sum to the expected total" — comparison
+ addition, no solver, no `UTable`. Estimate: **~150–200 LOC**, roughly ⅓–½ the abelian
engine.

**Consequences for a plan:**
- **`AbelianGroup` already exists** — it *is* the `unit`/Measure engine
  (`unify_unit`, `TyDimT`). `unit` is preserved as-is; the system wraps the existing
  engine, does not rewrite it.
- **Start with `Composition` + `Layout`**: cheapest engine (no solver), strongest
  cases (binaries + waterfall fintech + aerospace telemetry), and it follows the `unit`
  mold step for step (`TyDimT` → `TyComp`, same hook point). Low risk.
- The system's total cost is **per engine**, not per kind — and Composition, the one
  with ★★★ cases, is the cheapest engine to add.

## Surface naming — why `kind`/`theory` (recovered from `taxon`/`taxology`)

`kind` and `theory` are the natural type-system words, and an earlier draft avoided
them for a collision fear that turned out to be illusory. The measured facts:

- **`kind`** appears ~1055 times in the compiler as the AST/token discriminant field
  (`.kind`, `kind:`) and **zero** times as a user-facing surface identifier the stdlib
  exposes. All of it is *field-position*, never *item-declaration position*.
- **`theory`** collides **once** in the entire tree (compiler + stdlib + examples), and
  never as a public field a user types.

So the recovery is cheap on both:

- **`kind` is a *contextual* keyword** — keyword only in item-declaration position
  (`kind Name = …` at module top level), an ordinary identifier everywhere else. The
  ~1055 `.kind` field uses survive by construction, because the parser only treats
  `kind` specially as the first token of a top-level declaration. This is LL(1) with
  one-token lookahead: kaikai's top-level dispatch has no "ident ⇒ try an expression"
  branch (a leading identifier at module scope is already an error), so `kind IDENT =`
  is unambiguous. The precedent is real and internal: **`region` is recognised the same
  way** — an ordinary identifier that becomes a block only in the shape `region { … }`,
  a plain identifier in every other position. `kind` follows `region`'s trick, not a
  new mechanism. (The earlier draft's claim that a soft keyword "would break the
  self-compiling compiler" conflated *soft* with *hard*: a hard `kind` keyword would
  force renaming 1055 fields; a contextual one does not touch them.)
- **`theory` is a hard keyword, catalog-only** — legal only in the theory registry
  module (`stdlib/core/theories`). Its single collision is renamed once; the cost is nil.

The user writes `kind` once per kind declaration; the word never appears in a use
position (`Real<meter>`, `Money[Real]<USD>`). `theory` is catalog-surface (and future
user-assemblable from the property menu), so it is first-class, not hidden plumbing.
`taxon`/`taxology` (reserved by an earlier lexical lane) are un-reserved and replaced.

## Shipped over AbelianGroup

User-declared abelian kinds, per-kind isolation, and use-site resolution
ship (issue #1108). Per-kind introducer keywords (`metric`, `cur`, …) are
recognised through a linear token pre-scan that builds the file's
introducer→kind map before any body is parsed — order-independent, and
without a stateful LL(1) parser (the spike's feared piece turned out
avoidable). Isolation rides the habitant symbol: the resolver rewrites a
bare `<sym>` to its kind-qualified form before unification, so the
existing abelian engine — which cancels by symbol-name equality —
isolates habitants across kinds with no new engine and no kind-tag on
`TyDimT`. `use kind Kind` extends the existing `use` keyword as designed.

## Open questions

- **`unit` should stop being a keyword.** It is `Measure`'s introducer,
  so it could become a registered introducer like `metric`/`cur` rather
  than a hard `TkUnitKw`. #1108 keeps `unit` as a keyword (the introducer
  table coexists with it); dropping the keyword is a follow-up. North
  star, not required for the engine.
- **Alias bodies across kinds.** Kind rewriting runs before alias
  expansion, so an alias whose body's symbols are shared across kinds is
  not re-qualified per use site (`metric.Newton` / `imperial.Newton`
  where `unit Newton = kg·m/s²`). #1108 leaves this to a follow-up; the
  fix is to move rewriting after expansion, not to add a `TyDimT` tag.
- Whether user-assembled theories (`theory Mine = { assoc, commut, idempotent }`)
  ship in the first cut or are deferred — the mechanism (closed property menu +
  decidable-combination check) is designed, but the standard aliases cover the known
  cases, so user assembly can land later without redesign.
- Whether `Effect`'s engine is even exposed as a `EffectRow` token, or is purely internal
  with `kind Effect` referencing it by a stdlib-core-only path.

**Resolved (see the honesty map above), now engineering work, not open design:**

- `Module`, `Composition`, `Region` each get a real engine of their own — the facade
  audit settled *that* they must; the remaining work is to *build* them, not decide
  whether to. ~~`Module`'s~~ **shipped**: nominal atom equality + structural
  formation, with `scale` in operation signatures (the `over T` on `Currency` names
  the scalar domain declaratively). ~~`Composition`'s~~ **shipped**: the same nominal
  slot verdict (`unify_nominal`) + structural formation, with load-bearing order and
  the summed measure as formation invariants of the composed type — not part of the
  slot's unification identity. `Region`'s is real identity/branding (`TyBranded`, which
  the runtime comment admits "has not landed").
- ~~`Semilattice` + `Perm`~~ **shipped**: the join engine (`kind_semilattice.kai`) —
  `+` joins in `<>`, idempotent collapse, subsumption-ordered unification (required ⊆
  provided, expected side first per `unify`'s contract), tree-level formation (no
  products/inverses/powers on Semilattice habitants, joins only among one Semilattice
  kind). `theory Semilattice = { assoc, commut, idempotent }` and
  `kind Perm : Semilattice with perm` are in the catalog; the file API is the first
  consumer (`FileHandle<read>` / `FileHandle<read + write>`).
- ~~`Functorial` leaves the theory catalog~~ **shipped.** A protocol header names a
  *law set*, not a theory, and the two namespaces are disjoint in the parser: a theory
  in a protocol header and `Functorial` on a `kind` are both errors. `fusion` left the
  theory property menu with it. The mechanism (`law_checks.kai`) is unchanged.
- ~~`Structural` dissolves~~ **shipped.** `Shape` is classified by `ConstructorApp`
  (witness-binding, the engine that already existed); `Region` by `Nominal`. They never
  shared an engine, so they no longer share a theory.
  `structural_kind_names`/`base_structural_kind_names` were dead and are removed.
  `Nominal`'s own engine is still to build — the catalog now names what Region needs
  rather than hiding it under a label shared with `Shape`.
- `Dim` stands on the HM core (first-order value equality), reached by dispatching
  `unify_dim` on the kind rather than delegating to the abelian solver.

## References

- `docs/kinds.md` — the current two-kind system this generalises.
- `docs/units-of-measure.md` — the `AbelianGroup` engine (`unify_unit`, Kennedy).
- `docs/layout-kind-design.md` — `Composition` / `Layout` detail (incl. TLV).
- `docs/effects.md` — the `EffectRow` machinery (why `Effect` is builtin, not a catalog
  theory: row variables + runtime handlers exceed a catalog theory).
- CLAUDE.md Tier 1 #3 — decidable HM, the constraint that closes the property menu.
