# proposed extensions

Catalogue of **pending** extensions for kaikai, grouped into two
families:

1. **LLM-friendly diagnostics** — extensions in the same family as
   `docs/typed-holes.md`: the compiler emits structured,
   machine-consumable information that an LLM (or LSP) can act on.
2. **Language-surface features** — additions to the core language
   itself. These are candidates for *closing* `design.md`'s open
   decision on concrete-syntax consolidation, not for opening new
   ones.

This file tracks only what is still open. Once an extension lands
(or is formally rejected), it leaves this file — its record lives
in the milestone retrospectives, fixtures under `examples/`, and
CHANGELOG entries. The one exception is *Deliberately not on this
list* below: a standing guardrail of forms rejected on principle,
kept so they are not re-proposed.

Each entry states what it buys, what it costs, and what it depends
on.

## Status summary

### LLM-friendly diagnostics family (pending)

| Extension                              | Status   | Depends on              |
|----------------------------------------|----------|-------------------------|
| `kai type <pos> --json`                | proposed | stage-2 type checker    |
| Counterexample JSON for exhaustiveness | proposed | match exhaustiveness    |
| `kai lint --json` — canonical-form rules | proposed | canonical style guide   |

### Language-surface family (pending)

| Extension                                  | Status   | Depends on              |
|--------------------------------------------|----------|-------------------------|
| Sum types with constant attributes         | proposed | parser + resolution     |
| `?.` optional chaining                     | proposed | parser + type checker   |
| Slice syntax `a[i..j]`                     | proposed | `Vector[T]` landing     |
| `Range[T]` as a first-class iterable       | proposed | collection design       |
| Binary pattern matching `<<...>>`          | proposed | parser + match exhaustiveness |
| `f.field` is always field access; UFCS needs `f.method()` | proposed (edition-boundary) | resolver + edition bump |

## 1. `kai type <pos> --json` — queryable type-at-position

```
$ kai type foo.kai:10:5 --json
{
  "file": "foo.kai", "line": 10, "col": 5,
  "expression": "xs |> filter(. > 0)",
  "type": "[Int]",
  "effects": [],
  "in_scope": [
    { "name": "xs", "type": "[Int]" }
  ]
}
```

Exposes what the checker already computes at every AST node. Covers
any position, not just holes. The schema deliberately overlaps with
`--holes-json` so tools learn one format.

**Cost**: low. The checker annotates every node with its inferred
type; the query walks to the cursor position.
**Depends on**: stage-2 type checker.

## 2. Counterexample JSON for exhaustiveness

When a `match` is non-exhaustive, the compiler already knows which
patterns are missing. Expose them:

```json
{
  "kind": "non_exhaustive_match",
  "at": "foo.kai:14:3",
  "counterexamples": [
    { "pattern": "Rect(0.0, _)", "reason": "not covered" },
    { "pattern": "Triangle(_, _, _)", "reason": "not covered" }
  ],
  "suggested_arms": [
    "Rect(0.0, h) -> ?",
    "Triangle(a, b, c) -> ?"
  ]
}
```

The suggested arms contain `?` holes, so the LLM can paste the
completion and receive hole reports for the bodies. This closes
the loop: error → concrete fix with holes → LLM fills → compile.

**Cost**: medium. The match-check already computes missing inhabitants
for its current error text; formalising the output is a refactor
plus a schema.
**Depends on**: pattern-match exhaustiveness check (already planned).

## 3. `kai lint --json` — canonical style as data

```json
[
  {
    "at": "foo.kai:3:5",
    "kind": "pipe_simplification",
    "message": "this could be a pipe chain",
    "suggestion": "xs |> filter(. > 0) |> map(. * 2)"
  }
]
```

A small, hand-curated set of canonical rewrites emitted by the
compiler — not a pluggable linter. The rules nudge code toward
*few forms, each with clear intent* (the principle that replaces the
retired *one canonical form per construct*). They are the cultural
scaffold for that standard, not an optional plugin.

**Cost**: medium. The rules must be defined and maintained. The
output pipeline is trivial once the rules exist.
**Depends on**: a canonical-form style guide (cultural design work,
not technical).

## 4. Sum types with constant attributes

```kai
type Rank with { value: Int, label: String } {
  Two   = { value: 2,  label: "2" }
  Three = { value: 3,  label: "3" }
  Four  = { value: 4,  label: "4" }
  Five  = { value: 5,  label: "5" }
  Six   = { value: 6,  label: "6" }
  Seven = { value: 7,  label: "7" }
  Eight = { value: 8,  label: "8" }
  Nine  = { value: 9,  label: "9" }
  Ten   = { value: 10, label: "10" }
  Jack  = { value: 10, label: "J" }
  Queen = { value: 10, label: "Q" }
  King  = { value: 10, label: "K" }
  Ace   = { value: 11, label: "A" }
}

# Auto-generated at resolution time, as top-level functions:
#   pub fn value(r: Rank) : Int { match r { ... } }
#   pub fn label(r: Rank) : String { match r { ... } }
```

A sum type whose variants each carry **constant** attribute values
known at declaration. For each attribute field, the compiler
generates a top-level projection function (same name as the field)
by expanding the declaration into a `match`. No dispatch, no search,
no polymorphism — pure desugaring.

### What it buys

- Eliminates the *parallel match* pattern — where a single sum type
  has two or three separate `fn_value`, `fn_label`, `fn_short_code`
  functions, each a full match over the same variants. Those matches
  drift: adding a new variant requires touching N functions. This
  proposal co-locates the info.
- Domain code with enums like HTTP status, currencies, priorities,
  months, locales — any closed set of named constants with a few
  associated attributes — shrinks linearly in the number of attribute
  functions.
- LLM benefit (Tier 3): parallel matches are exactly the kind of
  near-duplicate code LLMs desynchronise. One declaration removes
  the duplication.

### What it costs

- A third form of `type` declaration alongside alias, record, sum,
  and the existing sum-with-payload. Counts against "few visible
  concepts".
- Parser additions: `type X with { fields } { Variant = { ... } }`.
  Local to type declarations; no effect on expressions or patterns.
- The generated functions occupy the top-level namespace. Naming
  collisions with other top-level functions become possible —
  resolvable via module scoping but must be detected.

### Constraints (explicit, to keep this safe)

1. **Variants must remain nullary.** Mixing
   `Foo { ... } | Bar(Int) { ... }` is banned — too much going on in
   one declaration.
2. **Attributes are constants.** No function values, no effects, no
   references to other declarations. A literal goes in; a literal
   comes out.
3. **No uniqueness requirement.** Blackjack needs `Jack = { value:
   10, ... }` and `King = { value: 10, ... }`. Rust-style distinct
   discriminators are *rejected* — they would rule out this case,
   which is common enough to matter.
4. **Accessors are top-level functions, not methods.** `value(r)`
   works; `r.value` does **not** (no dot-access for sum-variant
   attributes — that would step toward methods and implicit
   dispatch).
5. **No typeclass drift.** This extension generates *one* function
   per attribute, at *one* type. No ad-hoc polymorphism, no instance
   resolution.

### Decision posture

Land only when:
- Two or more enums in the standard library or common user code
  exhibit the parallel-match pattern.
- Review confirms the constraints above are stable (no drift toward
  methods, typeclasses, non-constant attributes).

Until then, the `fn_info + accessors` refactor is the canonical
pattern.

**Cost**: medium. New type-declaration syntax, small resolution-pass
addition to emit the projection functions.
**Depends on**: parser + resolve pass.

## 5. `?.` optional chaining

```kai
# user: Option[User]; u.profile: Option[Profile]; p.display_name: String
let name: Option[String] = user?.profile?.display_name
```

Safe navigation over `Option`. Desugars to nested `opt_and_then`
calls that short-circuit on `None`. The semantics matches
Swift / Kotlin, except restricted to `Option` (no reference
nullability in kaikai).

### Semantics

- `x?.field` where `x : Option[T]` and `T` has a field `field : F`:
  - `None -> None`
  - `Some(v) -> Some(v.field)` if `F` is not `Option[...]`
  - `Some(v) -> v.field`        if `F` is already `Option[U]`
    (flattens — same as Swift)
- Chainable: `x?.a?.b?.c` short-circuits at the first `None`.
- Does **not** short-circuit the enclosing function. `!` does that;
  `?.` is local.

### What it buys

- Linear, readable navigation through optional fields.
- Complements `!`: `?.` builds an `Option[T]` at the current
  expression; `!` propagates the `None` out. Both are needed for
  different patterns.

### What it costs

- One new postfix-then-field-access syntactic form.
- Parser must distinguish `?.` from separate `?` and `.` tokens.
  Easy with LL(1) + lookahead.
- Type checker needs one rule per invocation site. The flattening
  case (`Option[Option[T]]` → `Option[T]`) mirrors `opt_and_then`.

### Constraints

- Works on `Option[T]` only — not on `Result`, not on user-defined
  sum types. Same reasoning as `!`: generalising would require
  typeclasses.
- No `?[i]` for optional list indexing in the first pass —
  `list_nth` + `opt_and_then` covers it for now.

### Decision posture

Land after `!` has been used enough to confirm the two do not
overlap in practice. `?.` is local; `!` propagates. If the code
base mostly wants propagation, `?.` may not be worth the
complexity — re-evaluate then.

**Cost**: low. Parser addition plus one desugar in the typed-IR
pass (to nested `opt_and_then`).
**Depends on**: parser + type checker.

## 6. Slice syntax `a[i..j]`

```kai
let prefix = xs[0..5]        # first five elements
let suffix = xs[10..]        # from index 10 to end
let mid    = xs[i..j]        # half-open interval
```

A half-open-range indexing expression that returns a view into
an existing container. The common form across Rust, Python, Go,
Swift.

### What it buys

- The idiomatic way to take a sub-sequence: clearer and shorter
  than `Mutable.array_slice(xs, i, j)` or `drop(i, xs) |>
  take(j - i)`.
- Composes with iteration: `for x in xs[i..j] { ... }` reads
  the intent without manual index arithmetic.

### What it costs

- A new expression form. Parsing cost is small; `..` is unused
  elsewhere and already reserved for range literals.
- A semantic decision: **view** (no copy, shares memory) vs
  **copy** (independent allocation). Views are faster but leak
  lifetime constraints; copies are easy but can be wasteful on
  large arrays.
- Interaction with `Mutable`: a view into a mutable array is
  itself mutable, which pushes into region-type territory
  (same brand mechanism as `Fiber[T]` and `Pid[Msg]`).

### Decision posture

Slice syntax makes most sense alongside `Vector[T]` (post-MVP,
see Doc B §*Out of scope for v1*). `Vector[T]` is persistent
and has no mutation concerns, so "slice = view" is cheap and
unambiguous. For `Array[T]`, a slice either copies (simplest)
or carries the same lifetime brand the region machinery already
supports.

The recommended order is: (i) land `Vector[T]` with copying
slice semantics for `Array[T]` as a first approximation, (ii)
revisit view-style slices once region types have proven
themselves elsewhere.

**Cost**: low for a copying implementation; medium for
view-based slices with region tracking.
**Depends on**: `Vector[T]` landing; range-literal `..`
confirmed in the grammar.

## 7. `Range[T]` as a first-class iterable

```kai
# Today (only as list literal):
each([1..4]) { i -> ... }

# With Range[T] as a value:
each(1..4) { i -> ... }
for i in 1..n { ... }                # if `for` lands too
```

Today kaikai-minimal allows `[1..n]` as syntax for *constructing
a list* with a range fill. `1..n` on its own — without the
brackets — is not a value. Adding `Range[T]` as a first-class
type would let `1..4` evaluate to a value that `each`, `map`,
`filter`, etc., can iterate over directly, without an
intermediate list allocation.

### What it buys

- The common case `each(1..n) { ... }` reads cleanly without
  bracket noise.
- `Range[Int]` is iterable lazily — no allocation of the
  underlying `[Int]`. Memory and start-up time matter for hot
  paths and tight loops.
- Foundation for a future `for ... in ...` form (proposal
  is implicit if/when desired).

### What it costs

- A new built-in parametric type `Range[T]` (probably
  `Range[Int]` and `Range[Char]` to start; full polymorphism
  needs a `Stepable` capability or similar).
- Higher-order helpers (`each`, `map`, `filter`, etc.) need to
  accept either `[T]` or `Range[T]` — easiest with a common
  `Iterable[T]` abstraction, which kaikai does not have today.
  Without it, each helper grows two overloads (or `Range[T]` is
  silently coerced to `[T]`, which loses the laziness benefit).
- Parser distinction between `[1..n]` (list literal) and `1..n`
  (range value). Both should remain legal; `[1..n]` keeps
  meaning "list with these contents", `1..n` keeps meaning "the
  range itself".

### Decision posture

Wanted alongside the collection-design pass that closes Doc B's
`Vector[T]` Known gap (§*Out of scope for v1*). The trio
`Vector[T]` + `Map[K, V]` + `Range[T]` is the natural set to
design together — they share the question "what is the kaikai
notion of an iterable?".

**Cost**: medium. New built-in type, helper overloads,
parser. Coupled with the iterable-abstraction question.
**Depends on**: collection design (`Vector[T]`, `Map[K, V]`).

## 8. Binary pattern matching `<<...>>`

```kai
# parse a length-prefixed packet
match buf {
  <<0xFE, version: 8, length: 16, payload: length / binary, rest: binary>> ->
    handle_v1(version, payload, rest)
  <<0xFF, _: binary>> ->
    drop()
  _ ->
    Fail.fail("unknown frame")
}

# build the same shape
let frame : [Byte] = <<0xFE, 1: 8, payload_len: 16, payload: binary>>
```

Erlang/Elixir-style binary pattern matching: in a `<<...>>`
context, each segment carries a size (in bits or bytes), an
optional unit (`/binary`, `/integer`, `/float`, `/utf8`), and
optional endianness/signedness modifiers. The same syntax works
on the LHS of `match` (parsing) and on the RHS of an expression
(building). Match arms get exhaustiveness checks; size variables
referenced later in the pattern (`payload: length / binary`)
must be bound earlier in the same pattern.

**What it buys**:
- **Native protocol parsing**. FIX 4.x/5.0/FAST, ISO 20022 binary
  envelopes, SWIFT MX, custom TCP framing — all of these are
  written today as imperative byte-twiddling. With this feature
  they are one `match` block.
- **Diff against ad-hoc parsers**. Hand-rolled parsers leak
  bounds bugs, off-by-one errors, and silent truncation. The
  compiler checks lengths and bounds in the pattern itself.
- **Differential vs mainstream**. Outside Erlang/Elixir/Gleam,
  no language has this. Rust has `nom` (library, parser
  combinators); Go has `encoding/binary` (manual). Adding it
  to kaikai is differential, not me-too — and aligns with the
  BEAM-heritage of the runtime.
- **Fintech leverage**. C2 toolkit pitch ("type-safe Money +
  audit trail") gains a third leg: "type-safe wire format". The
  three together cover the common fintech surface end-to-end.

**What it costs**: medium-high.
- Parser: new `<<...>>` form on both expression and pattern
  sides. Segments parse as `expr ":" size_expr ("/" type_modifier)*`.
- Typer: each segment is monomorphic (`Int<N>` for integers of
  fixed bit-width, `Bytes` for binary tails). No effect-row or
  HM extension; segments unify by structure.
- Codegen: lower to `array_get` / bit-shift sequences in stage 0,
  and to platform memcpy/load primitives in LLVM (stage 2). No
  runtime support library needed for the basic cases.
- Exhaustiveness: an additional case in the match-checker — a
  `<<...>>` pattern with a `_: binary` tail is exhaustive for
  any byte buffer; otherwise the checker reports "incomplete
  binary match" with a counterexample shape.

**What it does not include (v1 scope)**:
- No bit-level alignment outside byte boundaries beyond fixed
  power-of-two segments. (Erlang allows arbitrary bit sizes;
  kaikai v1 limits to 8/16/32/64 bits + binary tails.)
- No streaming binaries — the pattern matches against an
  in-memory `[Byte]`. Streaming is a separate effect (`Net`
  / `File`) and out of scope here.
- No checksum / CRC builtins inside the pattern. Those stay
  as ordinary functions over the parsed fragments.

**Depends on**: parser, match exhaustiveness, `[Byte]` type in
core. Independent of effects/fibers — cleanly post-m12.

**Decision posture**: candidate for its own milestone in the
m13–m14 range, with a dedicated design doc once the
fintech-toolkit scope is concrete. Bring forward only if
C2-pre prioritises a wire-format binding (FIX 4.4 is the
likeliest first target).

**Reference**: Erlang/OTP `<<>>` syntax (the canonical
prior art, 25 years of production use), Elixir bitstrings
(same semantics, modernised lexis), Gleam `BitArray`
(a recent re-implementation in a typed setting — closest fit
to what kaikai would adopt).

## 9. `f.field` is always field access; UFCS of a function requires `f.method()`

```kai
type Account = { id: String, balance: Int }

fn label(a: Account) : String = a.id        # always field access — unambiguous
fn ndigits(a: Account) : Int  = a.id.len()  # UFCS of `len` — parens required

# Today `a.id` is ambiguous: if a function `id` is in scope, the
# resolver may read `a.id` as bare UFCS (`id(a)`) instead of the
# field. This proposal removes that overload: a bare `.name` is
# *only* field access; calling a free function method-style *always*
# carries parens.
```

Today kaikai allows **paren-less UFCS**: `xs.length` reads as
`length(xs)` when `length` is a free function and `xs` has no
`length` field. That overloads the `.name` syntax — it means "field
access" OR "function applied method-style", and the resolver
disambiguates by a heuristic (is `name` a field of the receiver's
type? else, is there a free function `name` in scope?). When the
receiver's type is not yet resolved (e.g. issue #743, a generic
effect payload), the heuristic misfires and a legitimate field
access is reported as `` `name` is a function in scope, not a
field ``.

This proposal makes the rule mechanical: **`f.name` is always field
access. UFCS of a free function is always written `f.name(args)`,
with parens, even when `args` is empty (`f.name()`).** The `.name`
syntax stops meaning two things.

### Semantics

- `f.name` — field access, unconditionally. If `name` is not a
  field of `f`'s type, it is a field error on that type. The
  resolver never falls back to "maybe `name` is a free function".
- `f.name(args)` — method-style call. Resolves to a field-of-function
  type if `name` is a function-typed field, otherwise to UFCS
  `name(f, args)`. Parens are the marker that a *call* is intended.
- `f.name()` — UFCS of a zero-extra-arg function (`name(f)`).
  Previously spelled `f.name`; now requires the parens.

### What it buys

- **Removes a syntactic ambiguity.** `.name` has one meaning, not
  two. Aligned with Tier 2 #5 *few forms, each with clear intent* —
  field access and function application stop competing for the same
  surface.
- **Eliminates an entire class of resolver misfires**, including the
  misleading half of issue #743 (Bug B). The field-access resolver
  no longer needs a "is there a homonymous function?" fallback, so
  it cannot blame UFCS for what is actually an unresolved-type
  problem. (#743's Tier 1 escape — the typer accepting `b.field`
  without checking it when the receiver type is free — is a separate
  typer fix; this proposal removes the *confusing diagnostic*, not
  the underlying typer bug. Both are wanted.)
- **More LLM-authorable.** A model never has to infer whether `.id`
  is a field or a call; the rule is positional. The compiler-tells-
  the-model bet (Tier 3) is stronger when the surface is
  unambiguous.
- **Better errors generally.** A missing field is reported as a
  missing field on a concrete type, not as a UFCS hint.

### What it costs

- **Breaking change to the language surface.** Paren-less property-
  style UFCS (`xs.length`, `r.is_ok`, `pid.self`) stops resolving;
  it must become `xs.length()`, `r.is_ok()`. This is a Tier 1 #4
  *stability* concern: it changes code a user wrote against. It
  therefore requires an **edition bump** (Tongariki → Hanga Roa →
  Orongo …), the deliberate public commitment editions exist for —
  not an incidental minor. A migration is mechanical (`kai fix`
  could add the parens), which keeps it inside the "never dread
  upgrading" contract: the upgrade has a migration path.
- **An audit of how much stdlib + compiler code uses paren-less
  property UFCS** is a prerequisite to sizing the break. If the
  idiom is rare (most call sites already write `()`), the break is
  small; if pervasive, the `kai fix` migration carries more weight.
- It removes a small ergonomic affordance some users like (reading
  `xs.length` as a property). The trade is paid in keystrokes, not
  in expressive power.

### Constraints

- Method references via the `.field` placeholder lambda and
  receiver-bound `obj.method` (the deferred item in §*Closed*) must
  be reconciled with this rule — `obj.method` as a *value* (not a
  call) would still be field-access-only under this proposal, so a
  method reference needs its own spelling decided alongside.
- Does not change protocol dispatch or `|`/`||`/`|?` pipe
  convention dispatch — those are call sites with explicit operators,
  not `.name` accesses.

### Decision posture

This is the language owner's call: it trades a liked ergonomic
affordance (paren-less property UFCS) for the removal of a
syntactic ambiguity and a class of resolver misfires. The case
*for* is strongest under kaikai's own principles — *few forms with
clear intent*, LLM authorability, and "safety/clarity beats
ergonomics" — and the migration is mechanical. The case *against*
is the breakage and the lost affordance.

Sequencing: it is an **edition-boundary** change, so it lands with
an edition bump, not a minor. Pair the decision with the §*What it
costs* audit (measure paren-less property UFCS usage across stdlib +
compiler) so the break is sized before committing. It is independent
of — and complementary to — the typer fix in issue #743: that fix
closes the Tier 1 escape; this proposal closes the ambiguity that
makes #743's diagnostic misleading. Land the typer fix regardless;
land this when an edition window opens and the audit says the break
is affordable.

**Cost**: medium. Resolver change is small; the edition bump,
migration tooling (`kai fix`), and usage audit are the real work.
**Depends on**: resolver + edition bump (+ optional `kai fix`
migration).

## Deliberately not on this list

These were considered and rejected for the same reasons they are
rejected elsewhere in the design:

- **Macros / reflection**: break the *regular, predictable syntax*
  and *fast compilation* principles.
- **Refinement holes** (`?x : { n: Int | n > 0 }`): require a
  constraint solver. That violates the decidable-and-predictable
  commitment of the type system.
- **Gradual-typing holes** (`?: Dyn`): introduce dynamic typing into
  a language whose central promise is that typed effects cannot
  escape unhandled.
- **Rust-style enum discriminators** (`type T : Int { A = 0, B = 1 }`,
  each variant with a *unique* integer tag): the uniqueness rule
  rules out useful cases (e.g. blackjack, where `Jack`, `Queen`,
  `King` all want the same value `10`). The broader proposal #4
  (sum types with constant attributes) covers the same use-case
  without the uniqueness constraint.
- **`deriving Show` / typeclass-style auto-derivation**: directly
  conflicts with the principle *no costly type-class resolution*.
  Note that proposal #4 (attribute sums) handles the label case for
  *closed* sum types without needing typeclasses — that is the
  intended solution.
- **Early `return` statement**: kaikai is expression-based; *last
  expression is the value* is canonical. Adding `return` would
  create a second exit form and nest imperative control flow into
  an otherwise expression-oriented surface.
- **`&&` / `||` for Bool**: duplicate `and` / `or` without distinct
  intent. `and` / `or` stay as the canonical boolean operators.
  Note: `||` is *not* unused — it is the flat-map pipe (shipped).
  The point here is only that it does **not** duplicate boolean `or`.
- **`<-` as monadic bind / effect shorthand**: `!` (already landed)
  covers the propagation pattern for `Option` / `Result`; kaikai's
  direct-style effects do not need a bind operator. Adding `<-`
  would be a second mechanism for the same flow. The flat-map pipe
  `||` is **not** the same proposal: `||` is a binary pipe over
  containers in the `Sequence` family (`[T]`, future `Stream[T]`),
  with `Option` / `Result` explicitly outside the dispatch table.
  Different problems, different surface.
- **`$` low-precedence apply (Haskell-style)**: `|>` and `|` already
  avoid paren pyramids. `$` would be a third way to write the same
  chain.
- **`::` path or type ascription**: `.` already separates module
  paths (`math.vector.dot`) and `:` annotates types. `::` would
  collide with one of them without adding intent.

## Adoption criteria

### For LLM-friendly diagnostics (sections 1–3)

Each extension lands only when:

1. Typed holes have shipped and been used in anger. They are the
   prototype for this family; the others inherit their shape
   (expected type, in-scope bindings, candidates, text + JSON, stable
   schema).
2. A concrete need has shown up in practice. *LLMs might like this*
   is not enough. A concrete interaction with an LLM or LSP that
   currently fails or is awkward is.
3. The feature fits in ≤500 lines on top of the stage-2 checker.
   Anything larger gets its own design doc first.

### For language-surface features

These land only alongside the closing of *"Concrete syntax
consolidation"* in `design.md`. Any decision should be made as part
of that one conversation — not drip-fed.

- **Sum types with constant attributes**: land only after confirming
  (a) the pattern appears in ≥3 independent enums in stdlib or user
  code, and (b) the constraint set in section 4 has survived at
  least one design review without drifting toward methods,
  typeclasses, or non-constant attributes. Until then, prefer the
  `fn_info + accessors` refactor.
- **`?.` optional chaining**: land after `!` (already shipped) has
  been used enough to confirm the two do not overlap in practice.
  `?.` is local; `!` propagates. If the code base mostly wants
  propagation, `?.` may not be worth the complexity.
- **Slice syntax `a[i..j]`**: lands *after* `Vector[T]` ships.
  Copying semantics over `Array[T]` is the first-approximation
  implementation; view-based slices with region tracking wait
  for region types to prove themselves elsewhere.
- **`Range[T]` as a first-class iterable**: lands together
  with the collection design pass (`Vector[T]`, `Map[K, V]`).
  Designing iterability across these three types in one go
  avoids retrofitting helpers later.
- **Binary pattern matching `<<...>>`**: a milestone in its own
  right (m13–m14 range). Bring forward only if a fintech-toolkit
  binding (FIX 4.4 likely first) becomes a concrete priority.

The goal is to keep the surface small. A handful of orthogonal,
well-integrated extensions is worth more than a pile of clever
features.
