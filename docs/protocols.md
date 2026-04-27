# Protocols for kaikai

## Status: Landed (m12.8)

Stage 2 supports `protocol`, `impl`, and `#derive(...)` syntax with the
five stdlib protocols (`Show`, `Eq`, `Ord`, `Hash`, `Serialize`) defined
in `stdlib/protocols.kai`. The compiler resolves dispatch at the AST
level (between inference and monomorphisation), so each statically-typed
call site emits a direct `__pimpl_<P>_<T>_<op>` call on both backends
(C and LLVM). See `docs/lane-experience-m12.8.md` for the implementation
retrospective and v1 limitations.

Single-dispatch ad-hoc polymorphism via explicit `protocol` declarations
and `impl` blocks. Modeled on Clojure protocols, Elixir protocols, Go
interfaces (with explicit declaration), and the lightweight subset of
Rust traits.

**Not Haskell typeclasses.** No higher-kinded types, no constraint
propagation in signatures, no functional dependencies, no type families,
no overlapping instances. Single-dispatch only.

This document is the spec; `proposed-extensions.md §28` is the catalog
entry; `stage2-design.md §m12.8` is the schedule.

## Motivation

Three concrete pain points in current kaikai:

1. **`Money<USD>` printing**: `decimal_to_string(m.amount) ++ " USD"`
   on every interpolation. With `protocol Show` + `impl Show for
   Decimal[u: Unit]`, becomes `#{m.amount}` automatic.
2. **Generic equality / comparison / hashing**: every record / sum
   type written today needs ad-hoc `eq_<type>`, `hash_<type>`,
   `cmp_<type>` functions. With `protocol Eq` / `Ord` / `Hash`,
   one `impl` block covers it.
3. **Serialization**: `to_json(x: Money) : String`, `from_json(s:
   String) : Result[Money, JsonError]` per type. With `protocol
   Serialize`, opaque types (Decimal, BcryptHash) get the same
   surface as records.

Without protocols, each of the three has to be solved independently
with bespoke conventions or magic-name hacks. With one mechanism, all
three (and future cases) compose.

## Design

### Declaration

```kai
protocol Show {
  show(x: Self) : String
}

protocol Eq {
  eq(a: Self, b: Self) : Bool
}

protocol Ord {
  cmp(a: Self, b: Self) : Int
}

protocol Hash {
  hash(x: Self) : Int
}
```

`Self` is a reserved type-parameter referring to the type the protocol
is implemented for. Every protocol op takes `Self` (or values of
`Self`) at least once.

A protocol may declare default implementations:

```kai
protocol Show {
  show(x: Self) : String
  show_with(x: Self, prefix: String) : String =
    prefix ++ show(x)         # default; can be overridden per-impl
}
```

Default impls dispatch to other ops of the same protocol, so they are
written in terms of the protocol's own surface — no recursion into
external state.

### Implementation

```kai
impl Show for Int {
  show(n) = int_to_string(n)
}

impl Show for Money[u: Unit] {
  show(m) = decimal_repr(m.amount) ++ " " ++ unit_name(u)
}

impl Eq for Point {
  eq(a, b) = a.x == b.x && a.y == b.y
}
```

Polymorphic impls (impl for a parametric type) work when the
parameters are concrete instances of `Unit` / `Int` / etc.; the
compiler monomorphizes per concrete instantiation just like any
generic function.

### Use sites

Protocol ops are called as ordinary functions:

```kai
let s = show(my_money)              # dispatches to Show for Money[u]
let same = eq(p1, p2)               # dispatches to Eq for Point
let order = cmp(t1, t2)             # dispatches to Ord for Transaction
```

In string interpolation, `Show` is invoked implicitly when an
interpolation expression has a type that has `impl Show`:

```kai
"alice: #{m}"
# m: Money[USD]
# expands at compile time to "alice: " ++ show(m)
```

If the type has no `impl Show`, the typer emits an explicit error
pointing at the missing impl — no silent fallback to bytes or
`<opaque>`.

### Coherence rules

**One impl per (protocol, type) pair.** If a second `impl Show for
Money[u]` appears in the same compilation unit, the resolver emits a
duplicate-impl error.

**Orphan rule** (Rust-style): an `impl P for T` is allowed only when
**P** is declared in the current module **or** **T** is declared in
the current module. This prevents two foreign packages defining
conflicting impls on a type they both import.

The orphan rule keeps coherence local: the compiler can determine the
single applicable impl by searching only the modules transitively
imported. No global registry.

### What protocols cannot do (deliberately)

- **No constraints in signatures**: there is no `fn sort[T : Ord](xs:
  [T]) : [T]`. Generic functions take an explicit comparator
  (`fn sort[T](xs: [T], cmp: (T, T) -> Int) : [T]`). If you want the
  comparator to come from a protocol, the call site passes `cmp` from
  `Ord` explicitly:
  ```kai
  list.sort_by(transactions, cmp)   # cmp: (Transaction, Transaction) -> Int
                                    # provided by impl Ord for Transaction
  ```
  This is intentional. Keeping constraints out of signatures is the
  sharp distinction from Haskell typeclasses and the main reason
  compile times stay flat.

- **No higher-kinded types**: `protocol Functor[F[_]]` does not parse.
  Type parameters of protocols are first-order (`protocol P[T]`), and
  the only way `Self` is parametric is when the impl target is
  parametric (`impl Show for Money[u: Unit]`).

- **No multi-method dispatch**: dispatch always uses `Self` (the
  first-position type). For two-arg dispatch (e.g. `convert(from:
  T, to: U)`), the user writes a free function and dispatches manually
  via match.

- **No overlapping impls**: the orphan rule plus single-impl coherence
  rules out the situation Haskell `OverlappingInstances` has to handle.

### Resolution algorithm

For a call `op(x, ...)` where `op` is declared in `protocol P`:

1. The typer infers `T = type-of(x)`.
2. The resolver looks up `(P, T)` in the impl table built from
   transitively imported modules.
3. If exactly one impl is found, the call is rewritten to the impl's
   function. Monomorphization proceeds normally.
4. If zero impls are found, the typer emits "no impl of `P` for `T`",
   listing the imports that would provide one if any exist in the
   ecosystem (LLM-friendly hint).
5. If more than one impl is found, the orphan rule was violated
   somewhere upstream — the resolver emits the conflicting modules.

The lookup is `O(1)` hash by `(P, T)` key. No constraint propagation,
no unification beyond what monomorphization already does.

### Runtime cost

For monomorphic call sites (the typer can statically pick the impl
during monomorphization), the call is **direct** — no indirection.
Same cost as a regular function call.

For sites that genuinely need late binding (passing a value through
generic code that does not see the concrete type until runtime), the
cost is **one vtable indirection** — same as Go interfaces, Clojure
protocols, Elixir protocols. Negligible for all practical purposes.

The runtime emits one **vtable per protocol per implementing type**,
declared `static` in the C / LLVM output. Tables are linked at compile
time; no dynamic registry.

### Codegen sketch (C backend)

```c
/* per protocol declaration */
typedef struct kai_show_vtable {
  KaiValue *(*show)(KaiValue *self);
} kai_show_vtable;

/* per (protocol, type) impl */
static kai_show_vtable kai_show_vtable_for_money_usd = {
  .show = kai_money_usd_show,
};

/* per call site */
KaiValue *kai_call_show(KaiValue *self) {
  kai_show_vtable *vt = kai_lookup_vtable(KAI_PROTOCOL_SHOW, self->type_tag);
  return vt->show(self);
}
```

For statically-resolved sites, the lookup is elided and the call is
direct.

## Stdlib protocols (the v1 set)

Five protocols ship with the m12.8 milestone:

| Protocol | Ops | Use cases |
|---|---|---|
| `Show` | `show(x: Self) : String` | interpolation, debug, logging |
| `Eq` | `eq(a: Self, b: Self) : Bool` | equality, set membership, dedup |
| `Ord` | `cmp(a: Self, b: Self) : Int` | sort, min, max, ordered containers |
| `Hash` | `hash(x: Self) : Int` | hash maps (m14), hash sets, memoization |
| `Serialize` | `to_string(x: Self) : String`, `from_string(s: String) : Result[Self, String]` | json/csv/fix bindings via wrappers |

Stdlib provides default impls for all primitives (`Int`, `Real`,
`Bool`, `Char`, `String`, `Unit`, `[a]`, `Option[a]`, `Result[e, a]`,
records auto-derived via #derive, sum types auto-derived). User-defined
opaque types must `impl` themselves.

Adding a sixth protocol to stdlib in v1 (e.g. `Numeric`, `Monoid`)
requires a separate proposal — the v1 set is intentionally tight.

## Auto-derivation via `#derive` annotation

For records and sum types whose fields all already have `impl P`, the
user can derive trivially:

```kai
#derive(Show, Eq, Hash, Ord)
type Account = { id: AccountId, balance: Decimal[USD], txs: Int }

#derive(Show, Eq, Hash)
type Shape = Circle(Real) | Rect(Real, Real) | Empty_
```

The compiler generates the obvious structural impls:
- **Records** — field-by-field for every protocol.
- **Sum types** — outer-match per variant for `Show` and `Eq` (the Eq
  arm rematches the second argument with the same variant and conjoins
  per-field `eq` calls; the wildcard returns `false`); per-variant tag
  combined with field hashes via `acc * 31 + hash(field)` for `Hash`.
  The chosen tag-and-fold scheme preserves the Hash↔Eq invariant: two
  values judged equal by the derived `Eq` produce the same `Int` from
  the derived `Hash`.

The annotation is the only meta-instruction needed for protocols
(others — `#deprecated` etc. — live in their own whitelist per
`proposed-extensions.md §27`).

`#derive` is **opt-in**: if the user does not write the annotation, no
impl is generated. This avoids surprising user-defined `impl Show for
Account` getting shadowed by an auto-derived one.

For sum types, `#derive(Eq)` and `#derive(Hash)` reject at compile
time when any variant carries a field whose head type lacks a
reachable `impl P` (explicit `impl P for T`, another `#derive(P)`, or
a builtin from stdlib). The diagnostic names the variant and the
offending field type. Records continue to fall through to the
dispatcher's runtime panic for missing field impls; promoting that
to a compile-time error is left for a follow-up.

### Supported protocols and shapes

| Protocol | Records | Sum types | Notes |
|---|---|---|---|
| `Show` | ✓ | ✓ | renders `Type { f: v, ... }` and `Variant(arg, ...)` |
| `Eq`   | ✓ | ✓ | record fields conjoined with `and`; sum types validated against field-impl set |
| `Hash` | ✓ | ✓ | polynomial mix (`h * 31 + h_next`); sums validated against field-impl set |
| `Ord`  | ✓ | ✓ | lexicographic by declaration order; sums by variant position then payload |
| `Serialize` | — | — | needs return-type-driven dispatch (post-v1) |

### Order semantics for `#derive(Ord)`

Records compare lexicographically in declaration order: the first
field whose `cmp` returns non-zero decides the result. Sum types
compare by **variant declaration position** first (variant 0 < variant
1 < ...); within the same variant, payloads compare lexicographically
by position. The synthesised `cmp` for

```kai
#derive(Ord)
type Tier = Bronze | Silver | Gold | Platinum
```

returns `-1` for `cmp(Bronze, Silver)`, `1` for `cmp(Gold, Bronze)`,
and `0` for `cmp(Platinum, Platinum)`. The same rule extends to
variants with payload: `cmp(Circle(99), Square(0)) == -1` because
`Circle` is declared before `Square`, regardless of the inner radius.

`#derive(Ord)` is consistent with `#derive(Eq)`: when both are
derived (or `Eq` is hand-written to mirror the same field traversal),
`cmp(a, b) == 0` ↔ `eq(a, b)`. The compiler does not enforce this
across hand-written impls; users who derive only one of the two
should keep them in sync.

### Field-impl validation

Before lowering a `#derive(Ord)` annotation, the compiler walks each
field (or variant payload) and confirms the head type appears either
in a same-unit `impl Ord for T` or in another `#derive(Ord)`. Missing
impls are rejected at typer time with a diagnostic naming both the
offending field and its type, e.g.:

```
foo.kai:7:1: error: cannot `#derive(Ord)` for `Tagged`: no `Ord`
impl for field `tag` of type `String` (declare `impl Ord for String`
or `#derive(Ord)` on `String`)
```

The same kind of validation runs for `#derive(Eq)` and `#derive(Hash)`
on sum types (m12.8.x); the gap remaining is records with `Show` /
`Eq` / `Hash` annotations referencing missing field impls — those still
fall through to the dispatcher's runtime `panic`. Tightening that
case to a compile-time error is a follow-up captured in
`docs/m12.8-followup.md`.

## Composition with other features

### With UoM (m12.5)

```kai
unit USD
unit EUR

impl Show for Decimal[u: Unit] {
  show(d) = decimal_repr(d) ++ " " ++ unit_name(u)
}

let usd = 100.50<USD>
let eur = 89.75<EUR>
println("balances: #{usd}, #{eur}")
# → "balances: 100.50 USD, 89.75 EUR"
```

The `u: Unit` parameter monomorphizes per-currency at the call site;
the `unit_name` runtime helper provides the symbol.

### With refinements (m12.6)

```kai
type Email = String where matches /^[^@]+@[^@]+$/

impl Show for Email {
  show(e) = e   # already a string
}
```

Refined types `T where P` inherit `impl P` from their base type
unless the user provides a more specific impl.

### With effects

Protocols are **pure** — `impl P for T` ops cannot have effect rows.
If a use case needs effects (e.g. `print` to console), the operation
goes via an effect, not a protocol.

```kai
impl Show for Money[u: Unit] {
  show(m) = ...   # pure: returns String
}

# Wrong shape — not allowed:
# impl Audit for Account { log(a) : Unit / Audit ... }
# Use an effect, not a protocol, for behaviour with effects.
```

This keeps protocols compositional and predictable. A future protocol
extension to support effect rows is possible but explicitly out of v1.

## What this is not

To make the boundary with Haskell typeclasses unambiguous:

| Capability | Haskell typeclass | kaikai protocol |
|---|---|---|
| Instance declaration | `instance Show Int where ...` | `impl Show for Int { ... }` |
| Higher-kinded types | `class Functor f where fmap :: (a -> b) -> f a -> f b` | **not supported** |
| Multi-param classes | `class Convert a b where ...` | **not supported** (single dispatch only) |
| Functional dependencies | `class C a b | a -> b` | **not supported** |
| Type families | `type family F a` | **not supported** |
| Constraints in signatures | `Show a => a -> String` | **not supported** |
| Superclass constraints | `class Eq a => Ord a where ...` | **not supported** (each protocol is independent) |
| Overlapping instances | `OverlappingInstances` extension | **not supported** (orphan rule + coherence) |

What kaikai protocols **do** support is exactly what Go interfaces +
Clojure protocols + Elixir protocols support: **single dispatch by
type tag, with explicit declaration, on a closed set of impls per
compilation unit**.

## Implementation cost

| Component | Lines | Notes |
|---|---|---|
| Parser: `protocol` + `impl` keywords + bodies | ~50 | `Self` reserved; parameter parsing |
| Resolver: build impl table per module + orphan check | ~60 | `(protocol, type) -> fn` map |
| Typer: resolve protocol calls; substitute impl fn at monomorph | ~50 | reuses the existing monomorphization pass |
| Codegen: emit vtable structs + lookup helpers | ~50 | C backend; LLVM mirror in m7c-style follow-up |
| Stdlib: declare 5 protocols + impls for primitives | ~80 | Show/Eq/Ord/Hash/Serialize for Int/Real/Bool/Char/String/Unit/list/Option/Result |
| `#derive` annotation: auto-generate structural impls | ~50 | walks record / sum type definition |
| Tests + fixtures | ~80 | one fixture per protocol, positive + negative |
| Doc updates: this file + CLAUDE.md clarification + proposed-extensions §28 | ~30 | + cross-refs |
| **Total** | **~450 lines** | **~2-3 days at observed velocity** |

## Risks

1. **Slope to "we need a bit more"**. The first user request will be
   `protocol Functor[F[_]]` or `protocol Convert[A, B]`. Mitigation:
   the design doc (this file) lists what is out of scope as
   "explicitly not supported". Re-opening means a separate proposal
   with explicit cost analysis, not extension of this one.

2. **Coherence in selfhost**. The kaikai compiler itself uses many
   ad-hoc functions (`int_to_string`, `decimal_repr`, etc.). Migrating
   them to protocols is opt-in, not required — the compiler can keep
   the explicit functions while user code adopts protocols. If the
   migration of stdlib to protocols breaks selfhost, revert that
   piece.

3. **Compile-time impact**. The orphan check + impl table build
   adds work. Expect 2-5% slower compile per file. Mitigation:
   measure on selfhost; if it crosses 10%, revisit the algorithm
   (likely a O(n²) somewhere).

4. **Diagnostics quality**. "no impl of `P` for `T`" with bad span
   information is a confusing error. Budget specific work for clean
   diagnostics with suggested impl skeleton (LLM-friendly:
   `--impl-suggest-json` may be worth a follow-up).

## Decision posture

Schedule as **m12.8 — Single-dispatch protocols**, after m12.7 axiom
and before m13 bench + bit ops. Rationale: the typer is maturest
post-m12.7 (refinements + contracts + UoM all landed), so the dispatch
infrastructure plugs into a stable foundation. Lands as its own
milestone with a fresh worktree.

Cost: 2-3 days. Lower-bound for serious value: stdlib's 5 protocols
ship with primitive impls, `#derive` works, demos can use `#{...}`
freely.
