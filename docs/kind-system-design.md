# The kind system — theories, kinds, habitants (consolidated design proposal)

**Status:** proposal, not accepted. Consolidates a long design conversation into
one model. Supersedes the scattered notes in `docs/layout-kind-design.md` and the
kind sections of `docs/unsafe-systems-design.md`; those remain the per-feature
detail, this is the whole system.

Today kaikai has two kinds (`Type`, `Measure`) hardcoded in the compiler
(`docs/kinds.md`). This proposal generalises to a **catalog of unification
theories** with a uniform declaration surface, while keeping the catalog **closed**
and the inference **decidable** (Tier 1 #3).

## The three levels

```
value    : type      let x : Int
type     : kind      unit km        (habitant : kind)    /  type Person = {...}
kind     = theory    kind Measure = $abelian_group       (kind : theory)
theory               $abelian_group                      (compiler primitive)
```

- A **value** has a **type** (`42 : Int`).
- A **type/habitant** has a **kind** (`Int : Type`, `km : Measure`).
- A **kind** is a name **plus a unification theory** (`Measure = $abelian_group`).
- A **theory** is a decidable unification algorithm, a **compiler primitive** —
  the closed catalog.

## What a theory is (and is NOT)

A theory decides **type equality by unification, at compile time**. It is NOT a way
to compute values, and NOT user-definable.

- `$abelian_group` decides that `m·s` unifies with `s·m` — it exists so the *type
  checker* treats them as the same type, not so you can multiply units.
- The theory is **phantom**: it guides unification and is erased at runtime (except
  where it parametrizes codegen, e.g. `$composition`'s byte-swap — still no runtime
  value cost).

**The hard rule (why the catalog is closed):** a theory is a decidable
E-unification algorithm. E-unification over arbitrary equational theories is
**indecidable**, and it is *itself indecidable* to tell whether a given theory's
unification terminates. So a user cannot define one — it would let them hang the
type checker, and the compiler could not even reject the bad ones automatically
(same family as the forbidden "no type-class resolution"). Each catalog theory is
an individual mathematical result (someone proved its unification terminates).

**Two operations → out.** Every viable theory combines with **one** operation. Two
interacting operations (a ring's `+` and `×` with distributivity) make unification
indecidable. `$abelian_group` (one op) is in; a ring is out. This is the cheap
admission filter: one op → candidate (verify decidability); two interacting → no.

## The catalog (closed)

| Theory token | Vis | Equations | Decidable by | Kinds using it |
|---|---|---|---|---|
| `$structural` | **priv** | none (syntactic) | Robinson | `Type` only (isolation → use phantom types) |
| `$abelian_group` | pub | assoc + commut + inverse + id | Kennedy/Stuckey | `Measure`, `Metric`/`Imperial`, coord frames |
| `$semilattice` | pub | assoc + commut + **idempotent**, no inverse | ACI-unification | `Perm`, `Taint`, `Clearance` |
| `$composition` | pub | assoc, **non-commutative**, with measure | trivial (sequence) | `Layout`, `Waterfall`, `Timeline` |
| `$rows` | **priv** | semilattice + **row variables** | row unification (Rémy/Leijen) | `Effect` only |

Notation: **`$snake_case`**. The `$` sigil marks a compiler primitive (as in
kaikai's `$extern_handler` intrinsics); snake_case marks "not a type" (types are
PascalCase). Together they teach, in the syntax, that a theory is *chosen from a
fixed set, never defined* — `$my_theory` is visibly a non-existent intrinsic
(error), not a declarable name.

**Not in the catalog** (indecidable or not a unification problem): rings/fields
(two ops), arbitrary graphs / DAGs (relational, not algebraic — runtime data),
dependent-types-with-effects (the F* unified theory — powerful but indecidable,
rejected for the same reason as the borrow checker and overloading). DAG, Oban/BPM
workflows, crypto/number-theory, JSON binding: all **runtime or structural**, not
theories — a theory unifies types, it does not compute or relate graphs.

## Where the catalog lives (and why it cannot grow from user code)

Two layers, and the split is what makes the catalog closed:

1. **The unification engines** — compiler code (`stage2/compiler/`), like today's
   `unify_unit` (the abelian-group engine for `Measure`) in `infer.kai`. NOT
   declarable in kaikai. The engine catalog *is* which unification functions the
   typer implements — closed by construction, only what was programmed and proven.

2. **The token↔engine registry** — one stdlib file, `stdlib/core/theories.kai`,
   binds each `$token` to its compiler engine and gives it visibility:

   ```kaikai
   priv theory $structural    = intrinsic "structural"    # Type's engine — private
   pub  theory $abelian_group = intrinsic "abelian_group"
   pub  theory $semilattice   = intrinsic "semilattice"
   pub  theory $composition   = intrinsic "composition"
   priv theory $rows          = intrinsic "rows"          # Effect's engine — private
   ```

   **Two theories are private** (`$structural`, `$rows`), for the same reason:
   their only useful kind is already built-in and must not be user-replicated.
   `Type` is the sole structural kind — a user-declared isolated type universe
   (`kind Validated = $structural`) would add only *isolation*, which phantom
   types / newtypes already approximate, so `$structural` gives no user-facing
   algebra worth exposing. `Effect` is the sole rows kind (its machinery is
   built-in). The **public** theories (`$abelian_group`, `$semilattice`,
   `$composition`) are exactly those where a new user kind adds an algebra nothing
   else gives — Money's abelian group, Perm's idempotent join, Waterfall's summed
   measure. The admission criterion, now explicit:

   > **A theory is public if declaring new kinds over it yields an algebra nothing
   > else provides; private if its only useful kind is already built-in.**

   `intrinsic "..."` *names* a compiler engine; it does not define one. A `$token`
   with no matching engine is a compile error — you cannot invent `$foo`.

**Adding a theory = a compiler + stdlib change, never user code:** (a) implement the
engine and prove its unification decidable (design doc + typer code — the expensive
part), then (b) expose its token in `theories.kai` with the right visibility (one
line). A user program can do neither.

### The `theory` declaration is module-restricted (special rule)

`theory $x = intrinsic "..."` is a declaration form that is **legal only inside the
`stdlib/core/theories` module**. This is a deliberate special-case in the parser/
resolver — like a few other context-restricted forms — not a general capability.
A `theory` declaration in any other file (stdlib or user) is a compile error:
"`theory` declarations are only allowed in the theory catalog". This is the second
lock (after "no engine → error") ensuring the catalog cannot grow from outside its
one home. The catalog is closed by *implementation* (engines in the compiler) and
by *surface* (the `theory` form is inert everywhere but its home module).

## Kinds — declaration and visibility

A kind is declared over a **public** theory. Kinds obey ordinary `pub`/`priv`
visibility — the **same** mechanism (and the same pub-leak validator) that governs
every other symbol. No new `intrinsic`/`builtin` marker: visibility is the barrier.

```kaikai
# stdlib/core/kinds.kai
pub  kind Type    = $structural                       # public: usable
priv kind Effect  = $rows                             # DECLARED, but private → unusable
pub  kind Measure = $abelian_group with unit
pub  kind Money   = $abelian_group with currency
pub  kind Perm    = $semilattice  with perm
pub  kind Layout  = $composition  with layout { be le }
```

**Declared-but-unusable is just `priv`.** `Effect` *is* a first-class kind — real,
declared, in the registry, listed by `kai info kinds` — but `priv`, so a user who
writes `kind X = $rows` or references `Effect` gets a visibility error, the same one
any `priv` symbol produces. `$rows` is likewise `priv` in `theories.kai`. The
compiler knows `rows`/`Effect` exist (to give a clear error), but the pub-leak
validator forbids their use outside `stdlib/core`. This answers "how is a kind
declared yet unusable" without a bespoke concept: it is the visibility kaikai
already has.

A user may declare kinds over **public** theories only:

```kaikai
kind Money = $abelian_group with currency     # ✓ $abelian_group is public
kind Perm  = $semilattice with perm           # ✓
# kind Access = $structural                   # ✗ pub-leak: $structural is private
# kind Bad   = $rows                          # ✗ pub-leak: $rows is private
```

Isolated type universes (a `Validated` vs `Unvalidated` kind) are **not** available
— `$structural` is private, `Type` is its only kind. Use a phantom type / newtype
for that isolation; the kind would add nothing a wrapper does not.

## Habitants — the keyword and its policy

`with <kw>` gives the keyword that introduces the kind's habitants. Two properties
of each habitant set fall out of the **kind** (not always the theory):

**Structure** — a habitant may be atomic or structured:

| Kind | keyword | habitant | body? |
|---|---|---|---|
| Type | `type` | a type | yes (fields/variants) |
| Effect | `effect` | an effect | yes (operations) |
| Measure / Money / Perm | `unit` / `currency` / `perm` | a unit/currency/permission | no (atom) |
| Layout | `layout` | endianness | no (atom) |

`type` and `effect` are just the habitant keywords of their (built-in) kinds —
structured habitants — exactly as `unit` is Measure's — an atomic habitant. This
de-magics `type`/`effect`: they are `with`-keywords of built-in kinds.

**Open vs closed** — who declares the habitants:

- **Open** (`with kw`, no block): the **user** declares atoms — `unit km`,
  `currency USD`, `perm read`. For theories whose atoms are **opaque symbols** the
  algebra manipulates without knowing them (`km·s` works without the engine knowing
  what a metre is). Unlimited.
- **Closed** (`with kw { a b }`): the block **selects** from atoms the engine
  already knows, each carrying built-in semantics (`be` = byte-swap). The user adds
  none and defines no semantics (that would extend the engine — forbidden). A closed
  block naming an atom the engine does not know (`{ be le xyz }`) is an error.

Policy is per-**kind**: `Layout` is closed (`be`/`le` fixed), a hypothetical `Row`
over `$composition` (UI columns) would be open (arbitrary widths). Same theory,
different habitant policy, set by the kind.

**Value — opaque vs measure-carrying (a property of the THEORY):** whether a
habitant carries a numeric value depends on whether its theory *sums* one.

- **Opaque** (`$abelian_group`, `$semilattice`): the habitant is a bare symbol —
  `unit km`, NOT `unit km = 1000`. The algebra manipulates symbols (`km/h`, `km²`,
  `read ∪ write`) but **never sums magnitudes**. The km↔metre relation lives in a
  separate conversion function, not the kind.
- **Measure-carrying** (`$composition`): the habitant **declares a measure**,
  because the theory *sums* it (`size(a·b) = size(a) + size(b)` is its essence).
  To verify "tranches sum to 100%" the compiler must know `pct70 = 70`. So:

  ```kaikai
  kind Waterfall = $composition with pct
  pct pct70 = 70        # the habitant DECLARES its measure; Composition sums it
  pct pct20 = 20
  pct pct10 = 10
  ```

  `Layout`'s `be`/`le` carry the measure implicitly via the field type
  (`U32` → 4 bytes). A bare `pct pct70` (no `= 70`) would be an opaque symbol
  meaning nothing — the name "pct70" is not self-describing.

So a theory has **four** derived properties: unification rule, habitant policy
(open/closed), habitant structure (atomic/structured), and habitant value
(opaque/measure-carrying). `$composition` is the one whose habitants carry a
measure, precisely because summing that measure is what it does.

### How an intrinsic theory operates on user-declared habitants

A subtle point: `Layout` feels coherent because `$composition` **and** `be`/`le`
are all intrinsic — the compiler knows the theory, the habitants, and their codegen
semantics. But `kind Waterfall = $composition with pct` + `pct pct70 = 70` mixes an
**intrinsic theory** with **user-declared habitants**. How does a built-in engine
operate on `pct70`, which it never saw?

**A theory defines an interface its habitants must satisfy; it does not know the
habitants, it receives from them what it needs.** `$composition` says: "each
habitant gives me a number — its measure — and I sum them." Two ways to satisfy it:

- **Closed habitants** (`be`) satisfy the interface **from the compiler** —
  built-in, intrinsic to the engine (their measure/semantics ship with
  `$composition`).
- **Open habitants** (`pct70 = 70`) satisfy it **via the declaration** — the
  `= 70` *is* the user handing the theory the measure it asks for.

Declaring your own kind over an intrinsic theory does **not** extend the theory
(that would be defining a unification/codegen algorithm — forbidden, indecidable).
It **feeds** the theory's habitants the data the theory's interface requires. The
engine stays 100% compiler; the habitant is the user's; the `= N` is the visible
contract between them. That contract is why Waterfall is not magic: nothing about
`pct70` is inferred — the user states its measure, the theory sums it.

### Measure arithmetic must be exact (decidability)

`$composition` *verifies* its sum at compile time ("tranches sum to 100%"). With
**floating-point** measures this is unsound — `0.1 + 0.2 ≠ 0.3` in IEEE-754, so a
legitimate waterfall could fail to compile on a rounding artefact. So a
`$composition` measure must be **integer or exact rational** (`pct70 = 70`, or
`1/3`), never a float. A tolerance-based check (`|sum − 100| < ε`) would inject an
arbitrary ε into the type checker — the same unpredictable "accept/reject by a
magic number" that Tier 1 #3 rejects. Exact arithmetic keeps the sum
verification decidable and sound.

## Usage — uniform surface

```kaikai
type P = { x: Int }              # habitant of Type
effect E { op() : Unit }         # habitant of Effect
unit km                          # habitant of Measure
perm read                        # habitant of Perm

let d : Real<km>  = 5.0<km>      # Measure inhabitant on a numeric
type H = { magic: U32<be> }      # Layout inhabitant on a field

fn area[u: Measure](w: Real<u>, h: Real<u>) : Real<u^2> = w * h   # polymorphism over a kind
```

## `unit` compatibility

`unit` stays as a convenience: `unit x` ≡ `kind x : Measure` (its default kind).
It is Measure's habitant keyword, spelled with the existing `TkUnitKw` for
backward compatibility and F#-familiarity. With multiple abelian kinds, `unit USD`
would be ambiguous, so cross-kind habitants use the explicit form
(`currency USD`) — `unit` is reserved to Measure.

## The three hard locks (summary)

1. **Catalog cannot grow from user code.** Engines live in the compiler (closed by
   implementation); the `theory` form is legal *only* in `stdlib/core/theories`
   (closed by surface); a `$token` with no engine is an error.
2. **Some declared kinds are unusable.** `priv kind Effect = $rows` — real,
   registered, `kai info`-visible, but the pub-leak validator forbids use outside
   `stdlib/core`. Visibility is the barrier, not a new marker.
3. **`theory` is module-restricted.** The declaration form is inert everywhere but
   its one home module — a deliberate special rule in the resolver.

## When to separate two abelian kinds (Measure/Money was a trap)

Two kinds may share `$abelian_group` (`Metric` and `Imperial`) but stay isolated
(`meter + foot` = error). When is that separation worth it? The criterion, learned
by a counter-example:

**Separate two abelian kinds only when crossing the domains is *always* an error.**

- **Money is NOT such a case — separating it is harmful.** `USD/kWh` (price per
  energy), `USD/km` (freight), `USD/hour` (wage), `USD/kg` (commodities) are
  *legitimate* combinations of money with physics. A separate `kind Money` would
  make `USD/kWh` a type error — forbidding exactly what real fintech/energy/logistics
  code needs. And `USD + EUR` is *already* rejected inside a single `Measure` (they
  are distinct units). So money belongs in `Measure` as ordinary units: it gets
  `USD+EUR`-rejection **and** `USD/kWh`-composition, the best of both. `kind Money`
  is worse, not better. (The kWh example, `USD/(W*h) * (W*h) = USD`, is the proof:
  money divides by *compound* physical units, which demands one shared abelian
  algebra.)
- **Metric vs Imperial IS such a case.** `meter + foot` is always a bug — you must
  convert first. The Mars Climate Orbiter (1999, $327M lost) mixed newton-seconds
  with pound-force-seconds. `kind Metric = $abelian_group` and
  `kind Imperial = $abelian_group`, isolated, turn that class of bug into a compile
  error.
- **Coordinate frames / reference spaces** are the other real case: screen-space vs
  world-space, camera vs global — summing coordinates across frames without an
  explicit transform is a classic graphics/GIS bug. Separate frames → separate kinds.

Rule: **cross-domain combination legitimate → one kind (Measure); cross-domain
combination always an error → separate kinds (Metric/Imperial, coord frames).** The
user-declared-`$abelian_group` case is real — it just is not money.

## Implementation cost (measured by spike, 2026-07-05)

A read-only spike over `stage2/compiler/` measured what implanting this costs. Two
findings:

**The mechanism (`kind K = $theory`) is MEDIUM, contained in the frontend.** The
`Kind` type is a hardcoded 2-variant enum (`pub type Kind = KType | KUnit`,
`ast.kai:329`), and a tparam's kind is encoded by *suffixing its name with `#Unit`*
(a string hack, not a field). Real blast radius (excluding the `KUnitV` false
positive — the KIR's Unit value, unrelated): **~29 sites in 3 files**
(`ast.kai`, `parse.kai`, `infer.kai`) — **no KIR, no codegen**. Generalising the
enum + replacing the `#Unit` string hack with real data is mechanical. The one
genuinely delicate piece is the **contextual habitant keyword** (`currency`, `perm`
at top level) — it puts state in the LL(1) parser (a table of introducers populated
by `kind ... with kw`), the same mechanism `opaque`/`region` use but stateful.

**A unification engine is CHEAP if it does not solve equations.** The abelian engine
(`$abelian_group`) is **~350–450 LOC** in `infer.kai`: normal form (`UTable` +
`unit_to_table` + `utable_to_tree`, ~177 LOC), unification (`unify_unit` +
`unify_unit_diff`, ~78 LOC), and a **2-line hook** in `unify_env` (the `TyDimT`
case). The expensive part is `unify_unit_diff` — it *solves linear integer
equations* to unify unit variables (`m·? = m²·s`). **`$composition` needs none of
that**: its unification is "do the field sequences match, do the sizes sum to the
expected total" — comparison + addition, no solver, no `UTable`. Estimate:
**~150–200 LOC**, roughly ⅓–½ the abelian engine.

**Consequences for a plan:**
- **`$abelian_group` already exists** — it *is* the `unit`/Measure engine
  (`unify_unit`, `TyDimT`). `unit` is preserved as-is (sugar for `kind x : Measure`);
  the system wraps the existing engine, does not rewrite it.
- **Start with `$composition` + `Layout`**: cheapest engine (no solver), strongest
  cases (binaries + waterfall fintech + aerospace telemetry), and it follows the
  `unit` mold step for step (`TyDimT` → `TyComp`, same hook point). Low risk.
- The system's total cost is **per engine**, not per kind — and Composition, the one
  with ★★★ cases, is the cheapest engine to add.

## Open questions

- `theory $x = intrinsic "..."` surface — a new declaration form, module-locked.
  Confirm the resolver can restrict a form to one module (precedent: contextual
  keywords `opaque`/`region`).
- Habitant keywords (`currency`, `perm`) at top level need the contextual-keyword
  mechanism, populated by the `kind ... with kw` declarations read so far.
- Whether `Effect`'s engine is even exposed as a `$rows` token, or is purely
  internal with `priv kind Effect` referencing it by a stdlib-core-only path.
- Cost: `$composition` is the cheapest new engine (trivial unification);
  `$abelian_group` already exists (Measure). `$semilattice` is the next real add.

## References

- `docs/kinds.md` — the current two-kind system this generalises.
- `docs/units-of-measure.md` — the `$abelian_group` engine (`unify_unit`, Kennedy).
- `docs/layout-kind-design.md` — `$composition` / `Layout` detail (incl. TLV).
- `docs/effects.md` — the `$rows` machinery (why `Effect` is private, not a public
  theory: row variables + runtime handlers exceed a catalog theory).
- CLAUDE.md Tier 1 #3 — decidable HM, the constraint that closes the catalog.
