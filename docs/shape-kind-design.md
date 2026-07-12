# `Shape` — a kind for arity-1 constructors: protocols quantified over type constructors, without paying HKT

**Status:** proposal with a taken position. Design-doc seed. Not accepted. The
Tier 1 §3 amendment (§7) is an edition-level decision.

**Packaging (resolved):** `Shape` is a **catalog kind** — `kind Shape : Structural`,
surfaced by `kai info kinds` — not a hidden typer sort. An earlier draft hid it as a
sort; §1 records why that was walked back. The one new mechanism (the `Shape-App`
formation rule, `s[A] :: Type`) and the atomicity discipline (arity-1, no composition,
no type-lambdas) are identical under either packaging; only the packaging changed.

**Thesis:** kaikai's kind/theory machinery already has everything needed to give
protocols power equivalent to `Functor`/`Foldable` **without** the two poisons of
HKT. A new kind `Shape : Structural` of atomic arity-1 constructors makes `s[A] ~
List[Int]` first-order applicative unification (decidable by construction — the same
way `Composition` already rejects `be*le` "at formation"); a new theory `Functorial`
lowers the functor laws to **terms** to drive fusion; and kaikai's total
monomorphisation collapses dispatch to `O(1)` with no instance search. With that,
fusion is decoupled from `stdlib/list`: **closed theory, open models.**

This proposal **revises the rejection** of option (B) `Foldable`/`Mappable` made in
the #1134 session. That rejection was correct *for the formulation then on the
table* (`map(self: Self[A]) : Self[B]` with `Self` HKT + GHC-RULES-style synthesis).
The `Shape` formulation is not that one: it never applies `Self` to two distinct type
arguments (no type-lambda), and the synthesis of the fused target is not a RULES
machine but inlining + collapse driven by a closed-catalog equational law — which is
exactly the option (C) that #1134 said it "converges on". The real novelty here:
**(B) and (C) were never rivals — `Shape` is the surface of (B) whose mechanism IS
(C).**

---

## 1. The `Shape` kind and formation

> **REVISED (challenge from lnds).** An earlier draft packaged `Shape` as a hidden
> *sort* of the typer, deliberately kept out of the kind catalog. That was a category
> error, walked back here. `Shape` is a **catalog kind** — `kind Shape : Structural`
> — surfaced by `kai info kinds` like every other. The application rule `s[A] :: Type`
> (§1.3) is the one genuinely new typer mechanism and is independent of the packaging;
> atomicity (§1.2) is identical either way. See the closing note of this section for
> the full rationale.

### 1.1 `Shape` is a kind over `Structural`

kaikai classifies habitants by kind (`Int : Type`, `km : Measure`, `r : Region`).
`Shape` joins the catalog as one more:

```kaikai
# stdlib/core/kinds.kai
pub kind Shape : Structural
```

**The theory is `Structural`, and it already exists — zero new engine.** `Structural`
is the theory of Region ("two habitants unify iff they are the same symbol"), and that
is exactly shape equality: `List ~ List` succeeds, `List ~ Vec` fails, by symbol
identity alone. No product, no inverse, no sum — a shape "stands only for itself",
precisely the `Structural` contract. The compiler core already gives this (symbol
equality), so nothing is added to the unifier for *habitant equality*. What is new is
the *application* rule (§1.3), which is formation, not theory — see below.

**Habitants are derived, not introduced — so `Shape` takes no `with` clause.** `with
type` already belongs to `kind Type` and mints structured habitants with a body;
`Shape` mints none of its own. Instead, **every arity-1 `type` already declared in
`Type` is automatically a `Shape` habitant** — `type Tree[a] = ...` is a `Type`
habitant (its saturated forms `Tree[Int]` are types) *and* a `Shape` habitant (the
bare `Tree` constructor). This is the catalog's **second derived-habitant kind**,
alongside `Region`, whose habitants "are never declared at item scope like `unit m` —
each is skolemized fresh by a `region { r -> ... }` block". `Region` derives its
habitants from region blocks; `Shape` derives its from arity-1 `type` declarations.
Neither has a per-kind introducer word, and inventing one for `Shape` (`with shape`)
would be an introducer nobody writes — surface noise. The catalog entry documents the
derivation explicitly (§1.1a).

> **The user cannot hang a theory on `Shape` — and this needs no hiding.** The earlier
> draft kept `Shape` out of the catalog fearing `kind MyShape : Shape`. That fear was
> already answered by the standing lock: **theories are closed and not
> user-declarable** (kind-system-design §The hard rule; `builtin` theories "provided
> by the language, closed"). `Type` and `Effect` live in the catalog in plain sight
> and nobody extends them, because `HindleyMilner`/`EffectRow`/`Structural` are
> closed. A user cannot declare *any* theory, so they cannot hang one over `Shape` —
> full stop. The lock is the catalog's closure (law since #1108), not the concept's
> concealment. Surfacing `Shape` costs nothing the closure does not already prevent,
> and buys the self-description a language owes `kai info kinds` (Tier 3: an LLM
> discovers the kind families from the catalog, not from reading the typer).

### 1.1a The catalog entry

Written in the `kinds.kai` house style (compare the `Region` and `Layout` entries):

```kaikai
# `Shape` classifies arity-1 type constructors. `Structural` gives identity:
# two shapes unify iff they are the same constructor symbol (`List` ~ `List`,
# never `List` ~ `Vec`). Habitants are DERIVED, not introduced by a `with`
# word — every arity-1 `type T[a] = ...` is automatically a `Shape` habitant
# (its bare constructor `T`), exactly as every `type` is a `Type` habitant. A
# shape stands only for itself: no composition (`s ∘ t`), no partial
# application, no type-lambda — all rejected at formation, the shape kaikai's
# arity-1 discipline admits. A shape applied to a type, `s[A]`, forms a Type;
# that application is the kind's one new formation rule (see docs).
pub kind Shape : Structural
```

No `layout be` / `unit m`-style habitant lines follow it: there is nothing to list,
because the habitants are the user's (and stdlib's) arity-1 `type` declarations,
minted where those are written.

### 1.2 Who inhabits `Shape` (habitant formation)

A `Shape` habitant is a **constant** or a **variable**, both of arity exactly 1.
Formation discipline inherited verbatim from the catalog (`Composition` rejects
`be*le`/`be^2` "at formation"; `Region` habitants "stand only for themselves"):

| Form | Inhabits `Shape`? | Reason |
|---|---|---|
| `List`, `Vec`, `Option` (builtin arity-1) | yes, constant | single-type-argument constructor |
| `type Tree[a] = Leaf \| Node(Tree[a], a, Tree[a])` (user) | yes, constant | `type` of exactly one `Type`-kind tparam |
| `s` in `protocol Sequence[s: Shape]` | yes, variable | `Shape` variable bound by the protocol |
| `Map` (arity 2) | **rejected at formation** | arity != 1 (§1.4) |
| `Int`, `String` (arity 0) | no | arity 0 — it is a `Type` habitant, not a `Shape` habitant |
| `List[Int]` (applied) | no as Shape (yes as Type) | already saturated — a `Type` habitant |
| `s . t`, `Compose[F,G]` | **rejected at formation** | shape composition — forbidden (§1.4) |
| `\a. List[a]` type-lambda | **does not parse** | type-lambda — HKT poison #1, never a `Shape` habitant |

The three prohibitions (arity!=1, composition, type-lambda) are the definition of
"atomic" and are **what buys decidability**. A `Shape` habitant "stands only for
itself" — exactly like a `Region` habitant.

### 1.3 Well-formedness of `s[A]` — the one new formation rule

Habitant *equality* comes free from `Structural` (§1.1). What `Structural` — and no
catalog theory — provides is **application**: the theory menu speaks only of
combination *within* one kind (abelian product of `Measure`, summed measure of
`Composition`), never of applying a habitant of one kind to a habitant of another. The
application `Shape × Type → Type` is therefore a genuinely new **formation rule in the
typer**, orthogonal to the theory. It is what makes `Shape` load-bearing rather than a
relabelled `Structural`.

The formation judgment classifies each type by which kind it belongs to; write `Γ ⊢ τ
: K` ("τ is a habitant of kind K"). The new rule:

```
Γ ⊢ s : Shape        Γ ⊢ A : Type
──────────────────────────────────────  (Shape-App)
        Γ ⊢ s[A] : Type
```

`s[A]` is a `Type` habitant formed by applying a `Shape` to a `Type`. **There is only
one shape-application rule**, and its argument kind is `Type`, not `Shape`: that is
what forbids `s[t]` with `t : Shape` (composition). Saturation is total and in one
step: a `Shape` applied to its single `Type` produces a finished `Type`, never a
partially applied constructor.

*(Implementation note: internally the typer distinguishes the positions where a
`Shape` habitant may stand from where a `Type` habitant may stand — a syntactic
category the machinery may call a "sort". That is an implementation detail of the
formation checker, not a surface concept: the user sees kinds in the catalog, never a
parallel "sort" level.)*

**The three cases to resolve:**

**(a) `s[A] ~ Int` → FAILS.** `s[A]` has sort `Type` and form "shape application";
`Int` has sort `Type` and form "nullary constant". First-order unification fails
because the head constructors differ in arity: `s[A]` is `App(s, A)`, `Int` is
`App(Int, ∅)` with `Int` of arity 0. No unifier. **Clear, early error**, not a hang.
(This is exactly what `List[Int] ~ Int` does today.)

**(b) `s[A] ~ Map[K,V]` → FAILS, and this is the arity decision.**

> **Decision — strict arity 1. `Map[K,V]` does not inhabit `Shape`; `s[A] ~ Map[K,V]`
> is a unification error, not a coercion.** `Map` is arity 2. The `Shape-App` rule
> only produces `s[A]` (one argument). An `s := Map` would require `Map` to be
> arity 1, false. The error is "`Map` has arity 2, cannot unify with a `Shape`
> (arity-1 constructor)".

Rationale (against the temptation of arity-N from day 1): arity-N is *precisely*
where undecidable higher order reappears, because `s[A] ~ Map[K,V]` with `s` an
arity-N variable admits two unifiers (`s := Map` with two args, or `s := \x.Map[K,x]`
by currying) — and the second is a type-lambda, poison #1. Strict arity-1 leaves only
`s := Constructor` with no currying possible: **syntactic** first-order unification,
one unifier or none, trivial principality. The precedent is Rust: `Iterator` was not
HKT, and GATs (which give variable arity to associated types) arrived **years later**
with a much more expensive resolution engine. `Map[K,V]` participating in a
`Sequence` is post-Orongo, exactly as GATs were post-1.0 in Rust — and when it lands it
will be via explicit user currying (`impl Sequence for Map[K]` fixing `K`), never via
higher-order unification. Register this as a known limit, not a bug.

**(c) nested shapes `s[t[A]]` → FORBIDDEN at formation.**

> **Decision — `s[t[A]]` with `s, t` both `Shape` variables is NOT a well-formed
> type.** The `Shape-App` rule requires `s`'s argument to be sort `Type`. `t[A]` is
> sort `Type` **only if `t` is resolved to a concrete constructor** (then `t[A]` is
> saturated and is `Type`). With `t` an unresolved shape variable, `t[A]` is
> syntactically `Type`, but the interesting position — two distinct shape variables
> stacked — is exactly functor-compose (`Compose F G`), the case Haskell needs
> `newtype Compose` for because the kind system does not give it free. Forbid it at
> the surface: a protocol op **may not** mention `s[t[A]]` with two environment shape
> variables. `List[Option[A]]` (one shape variable `s[Option[A]]`, `Option` concrete)
> IS legal — it is `s` applied to a `Type` that happened to be `Option[A]`. The line:
> **at most one free `Shape` variable per type position.**

This is not an accidental expressiveness limit — it is what keeps unification
first-order. With two shape variables stacked, `s[t[A]] ~ List[Option[Int]]` has
multiple unifiers (`s := List, t := Option` vs `s := \x.List[Option[x]], t :=
identity`), reintroducing currying. Forbidding it eliminates the whole class.

---

## 2. Unification

### 2.1 The extended judgment

Multi-sorted unification adds two rules and **zero new engine** (same argument as
#1108: "zero new engine", the solver consumes opaque keys):

```
unify(s[A], D[B])   where s : Shape variable, D : Shape constant
──────────────────────────────────────────────────────────────────
  s := D,  then unify(A, B)          -- first-order decomposition

unify(s, D)  where s : Shape variable, D : Shape constant or variable
──────────────────────────────────────────────────────────────────
  s := D    (standard occurs-check over the Shape sort)
```

The `Shape`-sort variable `s` resolves to a constructor by direct binding, with
standard occurs-check. `A ~ B` recurses into the existing `Type` unifier. There is no
imitation/projection (Huet's higher-order rules) because there are no type-lambdas to
imitate — that is the whole difference. It is **first-order applicative unification**,
the one Haskell itself uses for `F a ~ List Int` (the undecidable part of Haskell is
not this; it is *instance search* with constraints, which kaikai does not have).

### 2.2 Decidability

**Reduction to multi-sorted first-order unification.** Every `Shape` variable has
fixed arity 1 (by formation) and can only bind to another `Shape` (constant or
variable). The term set is:

- `Shape` constants (`List`, `Vec`, `Tree`, ...) — arity 1, atomic
- `Shape` variables — arity 1
- `Type` constants/variables — under the `[·]`

This is a first-order unification problem over a finite multi-sorted signature
(Robinson, decidable, unique most-general unifier or failure). The absence of partial
application (strict arity) and of type-lambda (forbidden at formation) guarantees
**no higher-order terms in the signature** — the exact condition under which
unification is decidable. Termination is that of the standard occurs-check plus the
existing `Type` unifier. **The Tier 1 §3 decidability budget is untouched.**

**Principality.** Because first-order unification yields a unique mgu, HM inference
extended with `Shape` preserves principal types: if `xs |> map(f)` has a type, it has
a principal one, obtained by the standard composition of mgus. (Attack (a) in the
attacks section verifies rows and Measure-under-shape do not break this.)

### 2.3 Interaction with effect rows

Effect rows live in a distinct sort (`Effect`) and a distinct syntactic position
(after `/`), not under `[·]`. An op `map(xs: s[A], f: (A)->B / e) : s[B] / e` has:

- `s : Shape` — unifies by §2.1
- `A, B : Type` — unify by standard HM
- `e : Effect` (row var) — unifies by the rows engine (Rémy-style, already exists)

The three unifiers are **independent and composable** — no cross-coupling. The row `e`
that the function-argument carries propagates to the op's row exactly as today (this
is why the "pipes convention-based" §of the protocols doc says pipes *cannot* ride an
operator-style protocol: `+`-style operators have closed rows). Here it is different:
**the `Sequence` protocol declares its op with an explicit row-variable `e`**, not a
closed row. That is row-polymorphism in the op signature — which kaikai *already*
supports for free functions (`fn map[A,B,e](xs, f: (A)->B/e) : /e`). The protocol
inherits that capability because its op is, at bottom, a free function also
parametrised by `s`.

> **Decision — the "no effect rows in protocol ops" ban (protocols.md §With effects)
> is RELAXED to "no per-impl effect row variation, but YES a uniform row-variable
> quantified by the op".** The original ban had four reasons; reviewing them:
> (1) "combinatorial complexity in the vtable" — does not apply, because total
> monomorphisation eliminates the vtable in the `Shape` case (§4); (2) "cross-impl row
> inference" — does not occur, because the row-var `e` belongs to the **op**, the same
> across all impls, not per-impl; (3) "caller predictability" — preserved, the caller
> sees `/e` and knows the row is `f`'s; (4) "conceptual cleanliness" — preserved,
> `Sequence` describes *what a value is* (a traversable container), the row comes from
> the function the user passes, not the container. This is the protocols.md catalog's
> alternative (B) "uniform effect row declared by the protocol", applied to a
> row-**variable** instead of a row-constant. It is sound because `e` does not depend
> on the impl.

### 2.4 Measure under shape: `s[Real<m>]`

`s[Real<m>]` is legal and decidable. The `Shape-App` rule requires the argument to be
sort `Type`; `Real<m>` is sort `Type` (a `Real` carrying a `Measure` habitant in
`<>`). Unifying `s[Real<m>] ~ List[Real<km>]` decomposes to `s := List` (Shape) and
then `Real<m> ~ Real<km>` (Type), and the latter invokes the `Measure` abelian engine
(`unify_unit`), which decides `m ~ km` → fails (distinct units), or `m ~ m` →
success. **The two engines (first-order Shape, abelian Measure) operate at nested
levels without interfering** — the Shape engine decomposes down to `Type`, and there
the Measure engine takes over. Neither sees the other. This is standard multi-sorted
unifier composition. (Attack (a) confirms there is no pathological case.)

---

## 3. Surface

### 3.1 Syntax of the Shape-quantified protocol

```kai
protocol Sequence[s: Shape] : Functorial {
  map(xs: s[A], f: (A) -> B / e) : s[B] / e
  foldl(xs: s[A], init: Acc, combine: (Acc, A) -> Acc / e) : Acc / e
}
```

`[s: Shape]` is a `Shape`-kind tparam, bound by the protocol (a kind annotation in
`[]`, exactly as `[u: Measure]` annotates a unit-polymorphic tparam). `A`, `B`, `Acc`
are each op's implicit `Type` tparams (lowercase, kaikai convention). `e` is the
uniform row-var (§2.3).

**Decision — the law is declared with the theory in the header (`: Functorial`), NOT
as a `laws { }` clause.**

Rejecting the `laws { map(id) = id; ... }` form for three reasons, in weight order:

1. **Catalog coherence.** The catalog already associates laws with *closed theory
   names* (`AbelianGroup`, `Composition`), never with inline-written equations. An
   inline `laws { }` would be the user writing a theory's equations — exactly what the
   decidability lock forbids ("a user cannot write the equations of a theory
   directly", kind-system-design §The hard rule). `: Functorial` is "assemble from the
   closed menu"; `laws { }` is "write the algebra" — the red line.
2. **LL(1) and few forms.** `: Functorial` reuses the existing `kind K : Theory`
   syntax. Zero new token, zero parser change beyond accepting a `Shape` tparam.
   `laws { }` is a new block with an equation grammar — surface that must be parsed,
   typed, and validated.
3. **Intent.** `: Functorial` says "this protocol obeys the functor laws" —
   declarative, verifiable against a catalog. `laws { }` says "these particular
   equations" — opening the door to ad-hoc laws the optimizer cannot exploit (what
   would fusion do with a user-invented law?).

### 3.2 The new theory: `Functorial`

**Decision — `Functorial` enters the catalog, sibling of `Composition`, not a
reuse.**

```
# stdlib/core/kinds.kai (or theories.kai)
theory Functorial = { identity, fusion }
```

The two properties, with their exact equations (the functor laws lowered to terms):

- **`identity`**: `map(xs, id) ≡ xs` — mapping the identity observes nothing.
  (Functor law 1: `fmap id = id`.)
- **`fusion`**: `map(map(xs, f), g) ≡ map(xs, g ∘ f)` — two traversals collapse into
  one with the composed stages. (Functor law 2: `fmap g . fmap f = fmap (g . f)`.)

These are the **only** two equations, and they are what the optimizer exploits (§5).
The name `fusion` (not `composition`, to avoid colliding with Layout's `Composition`
theory, which is a distinct concept — measure summation, not a functor law).

> **Why a sibling of `Composition`, not a reuse.** `Composition = { assoc, measure }`
> governs unification of **types** (field order in a binary record, byte summation).
> `Functorial = { identity, fusion }` governs rewriting of **terms** (collapsing two
> `map`). They share the English word "composition" but are algebras over distinct
> objects: `Composition` over field sequences, `Functorial` over traversal pipelines.
> Fusing them would be the dual of the error already made confusing `AbelianGroup`
> with `Module` (the `USD²` bite). Siblings in the catalog, disjoint in semantics.

**The conceptual jump this theory formalises:** until now every catalog theory decided
**type equality** (`m·s ~ s·m`, `U32<be> ≠ U32<le>`). `Functorial` is the first that
decides **term equality** (`map(map(xs,f),g) ≡ map(xs, g∘f)`). It is a genuine
extension of the "theory" role, and must be named as such: an equational theory over
terms is a **rewriting system**, and its decidability requires confluence +
termination of the system, not just unification. The two rules `identity`/`fusion` are
trivially terminating (each application reduces the number of `map`) and confluent
(they do not overlap), so the lock holds — but it is a *different* lock (rewriting
confluence) from the type-theories' lock (unification termination). This is
load-bearing and must be stated in the doc: **the closed catalog now has two classes
of theory — type-unification and term-rewriting — and both are admitted only by
decidable construction.**

---

## 4. Dispatch and monomorphisation

### 4.1 How the shape flows through the pipeline

Call-site `xs |> map(f)` with `xs : s[A]`:

1. **Inference.** `map` resolves as an op of protocol `Sequence`. The typer infers
   `type-of(xs) = s[A]`. Since `s` is a `Shape` variable, dispatch **is not resolved
   yet** — it is left as a pending protocol-op, exactly as an op over generic `Self`
   waits for monomorphisation today (protocols.md §How dispatch resolves).
2. **Monomorphisation.** When the call-site specialises, `s` binds to a concrete
   constructor (`s := List`, `s := Tree`, ...) by the live substitution. Now
   `type-of(xs) = List[A']` with everything concrete.
3. **Impl-table.** The post-specialisation rewrite looks up `(Sequence, List)` in the
   impl table (key `(P, Shape-constant)`, `O(1)` hash) and redirects to
   `__pimpl_Sequence_List_map`. **Direct call, zero indirection, zero instance
   search.**

This is the same mechanism as #174 (polymorphic impls that recurse on the tparam),
extended from a `Type` tparam to a `Shape` tparam. The table key goes from `(P, T)` to
`(P, s)` where `s` is a `Shape` constant — same cardinality, same `O(1)`.

**HKT poison #2 (constraint resolution / instance search / overlapping) dies here**,
exactly as it dies today for ordinary protocols: total monomorphisation +
single-dispatch + orphan rule. No dictionary passing (the shape is concrete in
codegen), no instance backtracking, no overlapping (orphan rule over `(Sequence, s)`).

### 4.2 Public signatures: `fn process[s: Shape](xs: s[Int]) : Int`

**Decision — YES, allowed in public signatures, with MANDATORY annotation of the
tparam's kind (`[s: Shape]`), exactly as effect rows are mandatory in public
signatures.**

```kai
pub fn total[s: Shape](xs: s[Int]) : Int = foldl(xs, 0, add)   # requires Sequence[s]
```

The `[s: Shape]` tparam is visible in the signature. The body calls `foldl`, an op of
`Sequence`. Here the only subtlety appears: `total` requires `s` to have
`impl Sequence`. How is that expressed without reintroducing constraint-propagation?

> **Decision — the bound `Sequence[s]` is inferred from the body and verified
> per-monomorphisation, NEVER propagated through the signature.** Just like #877's
> free-fn bound ("the bound is effectively documentation until the body forces the
> op"). `total` does not carry `[s: Shape + Sequence]` in the signature; it carries
> only `[s: Shape]`. The use of `foldl` in the body *implies* that every `s` `total`
> is instantiated with must have `impl Sequence for s`, and that is checked **at the
> monomorphisation site** (`total[List]` checks `impl Sequence for List` exists → yes;
> `total[SomeType]` without impl → error pointing at the `foldl` in the body). Zero
> constraint travels with the public signature. The public signature announces
> `[s: Shape]` (the kind, mandatory so the caller knows it passes a container), not
> `[s: Sequence]` (the bound, which would be the vetoed constraint-propagation).

This preserves the #877 line and kind-system-design §"No protocol bound on the
carrier": the kind in the signature (mandatory, like the effect row), the bound
implicit and verified per-instance (never propagated). The analogy is exact:
`[s: Shape]` is to `Sequence` what `[T]` (carrier) is to `Module` — it declares the
slot, the theory/protocol verifies per-mono.

---

## 5. Fusion, reformulated

### 5.1 The rewrite rules modulo the theory

The optimizer applies the two `Functorial` equations as directed rules
(left→right), with the purity gate as a side-condition:

```
FUSION:    map(map(xs, f), g)  ⟿  map(xs, g ∘ f)      if pure(row(f)) ∧ pure(row(g))
IDENTITY:  map(xs, id)         ⟿  xs                    (always; id has no row)
MAP-INTO-FOLD:  foldl(map(xs, f), init, comb)  ⟿  foldl(xs, init, λacc a. comb(acc, f(a)))
                                                       if pure(row(f))
```

The **MAP-INTO-FOLD** rule is the key derivative: it is not a `Functorial` axiom per
se, but a **theorem** from combining `fusion` with the definition of `foldl` as a
traversal. It is what replaces the hand-written `list.map_sum`/`list.map_foldl`. It is
justified: `foldl ∘ map f = foldl with f fused into the combiner`, valid for any
`Sequence` model because `foldl` traverses in the same order as `map` (functoriality
guarantees `map` preserves structure, so `foldl` over the mapped structure = `foldl`
over the original with `f` applied per element).

The **purity gate is untouched** — it is structural (reads rows, not names), already
sound (`pipe_fusion_labels_pure`: pure rows or `Mutable`-masked). It stays the
side-condition, exactly as today. The only difference: today the rule is hardcoded and
gated to `mod == "list"`; now the rule is the `Functorial` equation and it fires for
**any** `s` with `impl Sequence`.

### 5.2 The target synthesis pipeline

**Decision — the fused-target synthesis moves to KIR post-mono (inlining +
collapse), it does NOT stay in the typer with the live substitution.**

This is the most important decision and it contradicts the current precedent (fusion
today lives in the typer with the live substitution, `try_fuse_map_pipe`).
Justification:

The current mechanism works because it has a **closed catalog of hand-written
targets** (`list.map_sum`, etc.): the typer recognises the shape and rewrites to the
name of the pre-written target. That model **does not generalise** — it would need one
hand-written target per `(model × terminal × source)`, the combinatorial explosion
#1134 identified. To decouple from `list`, the target can no longer be pre-written:
**it must be synthesised.**

In KIR post-mono, the compiler has the **concrete `foldl`/`map` of the model** (already
monomorphised to `List`, `Tree`, whatever). Then:

1. **Inline** the concrete `map` and the concrete `foldl`/terminal (the KIR inliner
   already exists, #1158).
2. **Collapse** the produce-then-consume: the `map` that builds the intermediate
   structure and the `foldl` that consumes it fuse by the `fusion` law, eliminating
   the intermediate. In `Vec`/`List` this is loop-fusion (Rust/LLVM do it); in `Tree`
   it is recursion fusion (TRMC + recursion inlining).

This is exactly #1134's mechanism (C) ("fusion by inlining + loop-collapse at
KIR/codegen") — but **driven by the theory's `fusion` law**, which tells the inliner
*when* the collapse is valid (purity side-condition) and *what* to collapse (the
`map∘map`, `foldl∘map` pattern). Without the law, (C) would be a blind inliner that has
to *discover* the fusion opportunity; with the law, the inliner *knows* where to look.
**The theory is the oracle that drives the inliner.**

> **Why move to KIR and not stay in the typer.** In the typer, synthesis would have to
> build the fused-target AST (as `pipe_fusion_compose` builds the closure today). For
> an arbitrary model, that means the typer would have to know the *implementation* of
> the model's `foldl` to fuse it — but the typer only sees the *signature*, not the
> monomorphised body. The concrete body only exists post-mono. That is why synthesis
> belongs in KIR: it is the first point where the compiler has the model's `foldl` *as
> code*, not as a type. Precedent #1158 (KIR inliner) is what makes this viable;
> without it, this proposal has nowhere to live.

**The hand-written `list.map_sum` stay as the `list` model's fast-path.** They are not
deleted: they are the `impl Sequence for List` with a hand-optimised `map_sum` that the
rewrite prefers over the generic synthesis when it exists. A user model without a
fast-path gets the synthesis (inline+collapse), which is good but may not match the
hand-written on the native backend — see §8, real risk.

---

## 6. Law verification

The functor laws are not decidable in general (verifying `map(xs, id) ≡ xs` for all
`xs` is program equality). kaikai does not *prove* them — it **tests them by default
and axiomatises them explicitly**. This aligns with Tier 1 §1 (audited escapes, not
incidental): "documented laws" (Haskell, where nobody checks them) → "laws tested by
default" (kaikai).

### 6.1 Semantics of the autogenerated checks

When the user writes `impl Sequence for Tree { map(...) = ...; foldl(...) = ... }`, the
compiler **autogenerates** a `check` block (property-based, using the `DCheck` that
already exists in the AST) per `Functorial` law:

```kai
# autogenerated from `impl Sequence for Tree` + `theory Functorial = { identity, fusion }`
check "Sequence[Tree] identity" {
  forall xs: Tree[Int] { assert eq(map(xs, id_int), xs) }
}
check "Sequence[Tree] fusion" {
  forall xs: Tree[Int], f: (Int) -> Int, g: (Int) -> Int {
    assert eq(map(map(xs, f), g), map(xs, x => g(f(x))))
  }
}
```

`kai test` runs them. Red → the impl violates the functor law → **the compiler
refuses to use that impl for fusion** (or fails the test, decision §6.3).

**The generators.** Here is the subtlety to resolve:

> **Decision — `s[A]` values are generated via the `impl` itself, specifically via a
> generator derived from the `type` structure (not via the `map`/`foldl` under
> test).** For arbitrary `A`, instantiate `A := Int` (the canonical witness type, with
> a standard `Int` generator) — property-based over `Int` suffices for parametric
> functions (parametricity: a function `∀a. s[a] -> ...` that works for `Int` works
> for every `a`, a Wadler-style free theorem). To generate a `Tree[Int]`, the
> generator walks the `type Tree[a] = Leaf | Node(Tree[a], a, Tree[a])` and produces
> random trees using the **constructors** (`Leaf`, `Node`), NOT the `map` under test
> — using `map` to generate the input of a `map` test would be circular. The
> structural generator comes from the `#[derive]`-style walk of the `type` (the same
> walk `#[derive(Eq)]` uses). It requires `s` to be a `type` with visible
> constructors; for an opaque `s` (private constructors), the user must provide the
> generator by hand (`check` with an explicit generator) or mark `axiom`.

`A := Int` as witness is sound by parametricity, but with an honest caveat: if the
`impl` type-cases on `A` (impossible in kaikai — no `typeof`), the witness would not
suffice. Since kaikai has no runtime type reflection, `A := Int` is sufficient.
Register as an invariant: **the law checks assume the protocol ops are parametric in
`A` — guaranteed because kaikai has no runtime type-casing.**

### 6.2 The `axiom` surface

`axiom` (which already exists as an audited escape, Tier 1 §1) exempts an impl from
generating/running the check, declaring the law assumed:

```kai
impl Sequence for ExternalBuffer {
  map(xs, f) = ...      # backed by a C ring buffer, can't cheaply construct arbitrary instances
  foldl(xs, i, c) = ...
} axiom Functorial      # "I assert these obey the functor laws; auditor's responsibility"
```

`axiom Functorial` on the impl skips the check autogeneration for that impl. It is
audited: it appears in the escapes grep (`axiom`, alongside `panic`, `?`, FFI), and a
lint can require a comment justifying why. **Tier 1 coherence is total**: the law goes
from "tested by default" to "axiomatised explicitly", the same safe-default/
audited-escape pair that governs null (`Option` by default, `panic` audited) and
effects (visible by default, FFI audited).

### 6.3 What if the check fails?

**Decision — a red law check is a TEST FAILURE (blocks `kai test`), NOT a compile
error.** An impl that violates `identity` compiles and runs; simply `kai test` is red,
and **the optimizer does not fuse that impl** (fusion requires the laws; without them,
the non-fused code is emitted, which is correct if slower). This separates correctness
(always guaranteed, fusion or not) from optimisation (only with verified laws). A user
can ship an impl with a broken law — their code is correct, just does not benefit from
fusion, and their test suite tells them. It is the honest discipline: fusion is an
opt-in-by-correctness optimisation, not a compilation requirement.

---

## 7. Tier 1 amendment

The current letter (CLAUDE.md Tier 1 §3): *"no Haskell-style type-class resolution
(no HKT, no constraint propagation, no functional dependencies, no type families)."*

**Replacement wording:**

> **no Haskell-style type-class resolution: no type-lambdas (higher-order
> unification), no instance search / constraint propagation, no functional
> dependencies, no type families, no overlapping instances.** Single-dispatch
> protocols may quantify over a `Shape` (arity-1 type constructor, atomic — no
> composition, no partial application, no type-lambda), so `s[A] ~ List[Int]` unifies
> by **first-order applicative** unification (decidable; the mgu is `s := List, A :=
> Int` or fails). A protocol may declare **equational laws from the closed theory
> catalog** (`Functorial = { identity, fusion }`) that the optimizer exploits for
> fusion; laws are **verified by autogenerated property checks**, never by a decision
> procedure, and exempted only via the audited `axiom` escape. This is the HKT-power /
> no-HKT-cost line: shapes give `Functor`/`Foldable` expressiveness; the bans on
> type-lambdas and instance search keep unification first-order and dispatch
> `O(1)`-monomorphic.

What the amendment **keeps forbidden** (the poisons, named):
- **type-lambdas** (higher-order unification) — poison #1, the source of
  undecidability.
- **instance search / constraint propagation** — poison #2, the source of the
  compile-time blow-up.
- **fundeps, type families, overlapping** — unchanged.

What the amendment **opens** (bounded):
- `Shape` arity-1 atomic → first-order applicative unification (decidable, the one
  Haskell uses for the decidable part).
- closed-catalog equational laws → guided fusion, verified by tests.

The rhetorical key: the amendment does **not** say "now HKT is allowed". It says "no
type-lambdas, no instance search" — which are the concrete mechanisms that made HKT
dangerous. `F[A] ~ List[Int]` was never the undecidable part; the undecidable part was
`F` = type-lambda + searching for `Functor F`. Forbidding those two by name, and
allowing applicative shapes, is a *more precise* line, not a laxer one.

---

## 8. Costs, risks and verdict

### 8.1 Hidden costs, with a position

| Cost | Magnitude | Position |
|---|---|---|
| **New kind + application rule in the typer** | The `Kind` enum is today `KType \| KUnit` (2 variants); adding `KShape` is mechanical (precedent #1108: ~29 sites, 3 files). Habitant equality reuses `Structural` (zero engine). The one real new mechanism is the `Shape-App` formation/unification rule — pure first-order, cheaper than the abelian engine (no linear-equation solving). | Low. `Structural` is free; the application rule is the cheapest new formation in the family (less than `Composition`, far less than `AbelianGroup`). |
| **Error quality** | A `s[A] ~ Map[K,V]` must say "arity mismatch: `Map` takes 2 type args, `Shape` requires 1", not a generic mgu-fail. A forbidden `s[t[A]]` must explain "at most one Shape variable per position". | **Real risk, budgetable.** It is the classic "half expressiveness" trap: the user writes something that *looks* reasonable (`Map` in a `Sequence`) and the rejection must be intelligible, not a unification vomit. Budget specific diagnostics per formation prohibition (§1.2), with an LLM-friendly hint ("use `impl Sequence for Map[K]` fixing K"). |
| **Compile time** | Two new engines: Shape unification (cheap) + Functorial rewriting in KIR (fires only in pipelines with `map`). The autogenerated check runs in `kai test`, not `build`. | Low in `build` (the rewriting is local to call-sites with a pipe). Measurable: gate on selfhost byte-id + time. |
| **Synthesis vs hand-written on native** | The synthesised fusion (inline+collapse) may not match the hand-written `list.map_sum` on the native backend, where RC and layout matter. | **The material risk of the proposal.** If the generic synthesis lands 20-30% behind the hand-written on native, the decoupling is a perf regression for the `list` case (the most common). **Mitigation (already in the design): the hand-written stay as the `list` model's fast-path** — synthesis only runs for user models. So the common case does not regress, and the user gains fusion they did not have before. Gate: native rb-tree/list bench, synthesis-vs-hand-written, synthesis must be within ~1.15× of hand-written or the fast-path stays and synthesis is marked "best-effort". |
| **Future pressure: `Compose[F,G]`, arity-N** | Users will ask for functor-compose and `Map[K,V]` as `Sequence`. | Register as explicit out-of-scope (as #180 registered multi-method). `Compose` is genuine HKT (two stacked shape variables) — stays forbidden. Arity-N is post-Orongo (Rust GATs precedent). The doc lists this as "explicitly not supported", like protocols.md §"What protocols cannot do". |

### 8.2 The four attacks

**(a) Do atomic shapes + rows + Measure-under-shape break decidability or
principality?**

No. Verified by constructing the worst case: `map(xs: s[Real<m>], f: (Real<m>) ->
Real<km> / e) : s[Real<km>] / e` instantiated with `xs : List[Real<Metric.m>]`.
Unification decomposes into three **strictly nested, non-interfering** chains:
1. `s[·] ~ List[·]` → `s := List` (first-order Shape).
2. inside `[·]`: `Real<m> ~ Real<Metric.m>` → `m ~ Metric.m` (abelian Measure, decides
   by symbol — precedent #1108: the cancellation key carries the kind).
3. `/e` with `f`'s row (rows).

Each engine operates in its sort and hands control to the next one inward; none sees
the other two. Composing three mgus of first/known-order preserves the unique mgu →
**principality intact**. The only case that *could* break is if one engine produced a
substitution another could not consume — but since the sorts are disjoint (`s :
Shape`, `m : Measure`, `e : Effect` never unify with each other — sort-mismatch is an
immediate error), there is no cross-talk. Sound.

**The real sub-attack:** can a `Shape` variable appear where a `Measure` is expected or
vice versa and slip through? No — §1.3 requires `s :: Shape` in the constructor
position and `A :: Type` in the argument; a `Real<m>` with `m : Measure` is *inside*
the `Type` argument, never in the Shape position. The sort-check at formation
guarantees it before unification.

**(b) Does inference stay single-pass?**

Yes, with an honest nuance. Shape unification is single-pass (first-order, resolves in
the HM flow). The protocol-op **dispatch** is post-inference/pre-mono (it already is
today for ordinary protocols — protocols.md §Resolution). The **fusion rewriting** is
post-mono (KIR). None of the three reintroduces a global fixpoint nor a second
inference pass. The chain `lex → parse → resolve → infer → mono → perceus → lower`
(Tier 1 §3) gains work *inside* `infer` (Shape unif) and *inside* `mono`/KIR (dispatch
+ fusion), not a new pass. Single-pass preserved in the sense that matters: no
iterate-to-fixpoint over the whole program (which is what Haskell's constraint
propagation does).

**(c) Can a user write something reasonable that the system rejects inexplicably?**

Yes — three cases, and they are **the half-expressiveness trap**, the proposal's
cultural risk:
1. `protocol P[m: Shape] { op(x: Map[K, m[V]]) }` — `Map` arity-2 rejected, and the
   user may not understand why `Map` is not "a container like the others".
   **Mitigation:** a diagnostic saying "`Map` has arity 2; `Shape` is arity-1. To use
   `Map` as a Sequence, `impl Sequence for Map[K]` fixing the key type" — turns the
   rejection into a recipe.
2. `impl Sequence for Compose[List, Option]` — functor-compose, rejected. The user
   coming from Haskell expects it. **Mitigation:** a diagnostic "functor composition
   (two stacked Shape variables) is not supported; wrap in a `type` — `type ListOpt[a]
   = { unwrap: List[Option[a]] }` — and `impl Sequence for ListOpt`". The manual wrap
   is the escape, like `newtype Compose` in Haskell.
3. `fn f[s: Shape](xs: s[Int])` whose body calls no `Sequence` op — accepted without a
   bound, and if the user *believed* `[s: Shape]` implies `Sequence`, they are
   surprised when `f[SomeNonSequence]` compiles. **Mitigation:** this is identical to
   the known #877 gap (bound as documentation until the body forces it); document
   likewise.

All three are *rejections-with-a-recipe*, not blind rejections — but only if the
diagnostics are budgeted. **Without that budget, the proposal generates exactly the
"the compiler hates me" friction that Tier 3 (LLM authorability) and Tier 2
(approachable) penalise.** The design is sound; the ergonomics depend on the messages.

**(d) Is this really different from "Rust traits with trimmed GATs", and if not, why
not copy that formulation directly?**

It is **more restricted** than traits+GATs, deliberately, and the difference is what
preserves Tier 1:

- **Rust traits with GATs** allow variable-arity associated types (`type Item<'a>`)
  and propagating bounds (`where T: Iterator`, resolved by trait resolution with
  backtracking). Rust's trait resolution engine is **Turing-complete** (proven;
  proc-macros and `where` clauses can diverge). kaikai **cannot** copy that: Tier 1 §3
  vetoes constraint resolution precisely because it is the compile-time blow-up.
- **`Shape` is traits WITHOUT the trait solver.** Dispatch is not backtracking
  resolution — it is `O(1)` lookup in a `(P, s)` table post-mono, with no propagating
  bounds (§4.2). There is no `where s: Sequence` that travels; there is a local
  per-monomorphisation check. It is "single-dispatch protocol quantified over an atomic
  constructor", not "trait with associated type and solver".

Why **not** copy Rust directly: copying traits+GATs would bring the trait solver, which
is the Tier 1 red line. The `Shape` formulation takes the *expressiveness* of
`Iterator`/`Functor` (quantify over the constructor) without the *mechanism* (solver +
GATs). It is Rust-`Iterator`-without-solver: monomorphised, single-dispatch, O(1) —
exactly what #1134 called "mechanism (C), Rust lineage". The difference from "trimmed
GATs" is that GATs are *associated types with lifetime/type params* (variable arity in
the output), while `Shape` is *arity-1 in the protocol input* — simpler, and without
the part that needs a solver.

**Attack (d) verdict:** distinct from Rust by absence of the trait solver; not
directly copyable because copying would bring the vetoed solver. The kaikai formulation
is genuinely the decidable-by-construction subset of traits, not an arbitrary trim.

### 8.3 Verdict: edition and order

**Orongo or post-Orongo? — Post-Orongo, with one exception.**

The *sound core* of this proposal (kind `Shape : Structural`, the `Shape-App`
formation rule, first-order unification, theory `Functorial`, mono dispatch) is
implementable and sound. But shipping it complete in Orongo has two real technical
dependencies (not calendar — code):

1. **The KIR inline+collapse synthesis depends on the KIR inliner (#1158) being mature
   and on TRMC+recursion-inlining working for non-`list` models (Tree).** Without that,
   fusion for user models is "best-effort" and may regress perf. This is a technical
   dependency verifiable by bench, not a time conjecture.
2. **The open "list default" fork** interacts: synthesis (C) is easier with `Vec`
   default (trivial loop-collapse in LLVM) than with linked-list (fusion = TRMC +
   recursion-inlining). This proposal **partially defuses** that fork — if fusion is no
   longer coupled to `list`, the question "which collection is default?" separates from
   "which collection fuses?". But the synthesis *mechanism* still prefers `Vec`.

**Recommended order:**

1. **First, resolve the "list default" fork** (independent). This proposal defuses it:
   decide the default by other criteria (ergonomics, RC), knowing fusion no longer
   forces it.
2. **In Orongo: the surface lock, NOT the engine.** Reserve the `[s: Shape]` and
   `protocol P[s: Shape] : Functorial` syntax as edition-level (the Tier 1 amendment is
   an edition decision), but **do not** ship the synthesised fusion yet. Reason: the
   Tier 1 amendment ("no HKT" → "no type-lambdas, no instance search") is a public
   commitment that belongs at an edition boundary; doing it in 1.0 avoids a second
   edition-bump later. Shipping the kind + application rule + dispatch (which are sound
   and cheap) gives quantified `Sequence` with dispatch, even if fusion is still the
   hand-written `list` path.
3. **Post-1.0: the synthesised fusion**, once the KIR inliner + TRMC-for-Tree are
   bench-verified to within ~1.15× of hand-written. This is the piece with the perf
   technical dependency; attack it when the gate (native bench synthesis-vs-hand-
   written) is binary.

**I would go for: amend Tier 1 and ship the `Shape : Structural` kind + `Shape-App`
rule + dispatch in Orongo (cheap and sound edition lock), leaving the synthesised
fusion as the first post-Orongo lane gated by the native bench — and resolve the "list
default" fork BEFORE both, because this proposal defuses it and it is worth deciding
the default without fusion pressure.**

---

## Two notes for whoever converts this into `docs/`

- The most fragile load-bearing claim is §3.2 (**`Functorial` introduces a second
  class of theory — term-rewriting vs type-unification**). The current catalog assumes
  every theory decides *type* equality. `Functorial` decides *term* equality. The
  decidability lock changes from "unification termination" to "rewriting
  confluence+termination". It is sound for `{identity, fusion}` (trivially
  confluent/terminating), but the doc must state it explicitly, because a future user
  wanting `theory MyLaws = { ...term equations... }` would hit a *different* lock from
  the one protecting the type theories.

- The decision to move synthesis from the typer to KIR (§5.2) **contradicts the
  current precedent** (today it lives in the typer). It is correct — the typer does not
  see monomorphised bodies — but it is an architectural change an implementation lane
  must understand before touching `pipe_fusion.kai`. It is not an incidental refactor.
