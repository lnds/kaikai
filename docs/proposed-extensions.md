# proposed extensions

Catalogue of extensions for kaikai, grouped into two families:

1. **LLM-friendly diagnostics** — extensions in the same family as
   `docs/typed-holes.md`: the compiler emits structured,
   machine-consumable information that an LLM (or LSP) can act on.
2. **Language-surface features** — additions to the core language
   itself. These are candidates for *closing* `design.md`'s open
   decision on concrete-syntax consolidation, not for opening new
   ones.

None of these are adopted yet. They are catalogued here so that,
when the core stabilises, we have a short list of coherent moves
rather than a pile of ad-hoc additions.

Each entry states what it buys, what it costs, and what it depends
on.

## Status summary

### LLM-friendly diagnostics family

| Extension                                  | Status        | Depends on              |
|--------------------------------------------|---------------|-------------------------|
| `todo!(msg) : T`                           | scheduled m7d | typed holes             |
| `kai type <pos> --json`                    | proposed      | stage-2 type checker    |
| Counterexample JSON for exhaustiveness     | proposed      | match exhaustiveness    |
| `axiom name : T`                           | scheduled m12.7 | stage-2 type checker  |
| `kai effects <target> --json`              | scheduled m7f | effect inference        |
| `?e` — effect holes                        | scheduled m7f | typed holes + effects   |
| `import ?name` — dependency holes          | scheduled m7f | module resolution       |
| `kai lint --json` — canonical-form rules   | proposed      | canonical style guide   |

### Language-surface family

| Extension                                  | Status   | Depends on              |
|--------------------------------------------|----------|-------------------------|
| Tuples `(T1, T2, ...)`                     | gated m8.5  | m8 corpus + measurement  |
| Record punning `{ x, y }`                  | scheduled m7d | parser                 |
| `variants[T]()` builtin                    | scheduled m7e | monomorphisation       |
| Sum types with constant attributes         | proposed     | parser + resolution     |
| `!` postfix — `Option` / `Result` propagation | scheduled m7e | `Option` / `Result` in prelude |
| `@` as-pattern in `match`                  | scheduled m7d | parser                 |
| `?.` optional chaining                     | proposed     | parser + type checker   |
| Bit ops (`bit.and` / `bit.or` / `bit.shl` / ...) | scheduled m13 | stdlib module + intrinsic recognition |
| `Map[K, V]` + `m["key"]` indexing          | proposed     | collection design       |
| Slice syntax `a[i..j]`                     | proposed     | `Vector[T]` landing     |
| Method references as values (`obj.method`) | scheduled m7f | parser + brand machinery |
| `Range[T]` as a first-class iterable       | proposed     | collection design       |
| Pipeline placeholder `_`                   | scheduled m7d | parser                 |
| Binary pattern matching `<<...>>`          | proposed     | parser + match exhaustiveness |
| `++` operator (string + list concat)       | scheduled m7d | parser + 2 typer rules |
| `main()` row inference                     | scheduled m7e | typer + runtime default loader |
| `use Effect` — open effect in scope        | scheduled m7e | parser + resolver scoping |
| Protocols (single-dispatch)                | scheduled m12.8 | parser + resolver + vtable codegen |

## 1. `todo!(msg) : T` — principled unimplemented

```kai
fn parse_expr(tokens: [Token]) : Expr = todo!("pending binary ops")
```

`todo!(msg)` is an expression of any type. Structurally a sibling
of `?`:

- Type-checks as the expected type at its position.
- At runtime, aborts via `kai_prelude_panic("todo: #{msg}")`.
- Reported in `--holes-json` with `"kind": "todo"` and the message.

The distinction with `?` is intent. `?` means *I don't know yet*;
`todo!` means *I know, not yet*. Persists through reformats and is
grep-able, replacing informal `// TODO` comments with a typed marker
that the checker tracks.

**Cost**: low. One new token, reuses the hole runtime.
**Depends on**: typed holes.

## 2. `kai type <pos> --json` — queryable type-at-position

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

## 3. Counterexample JSON for exhaustiveness

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

## 4. `axiom name : T` — postulated symbols

```kai
axiom unsafe_cast[A, B] : (A) -> B
axiom db_layer_exists : Database / Io
```

An `axiom` declares a symbol with a type but no body. The compiler
accepts it, type-checks its uses, and lists it in `--axioms-json`.

- Unlike `todo!`, axioms live at top level and can be called from
  anywhere. They declare *this will never have an implementation
  here* (FFI boundary, architectural placeholder, library seam).
- By default, calling an axiom at runtime aborts via
  `kai_prelude_panic`. Axioms can be bound to an external symbol
  via `Ffi` to receive a real implementation at link time.
- Every binary carries a manifest of which axioms it was built
  against, so an auditor (human or LLM) can see the trust surface.

**Cost**: low to medium. One new top-level form, a side list, a
manifest pass.
**Depends on**: stage-2 type checker.

## 5. `kai effects <target> --json` — effect graph as data

```
$ kai effects src/ --json
[
  { "fn": "greet",
    "effects": ["Io"],
    "handled_in": null },
  { "fn": "main",
    "effects": [],
    "handlers_installed": [
      { "effect": "Io", "at": "main.kai:9" }
    ] }
]
```

Summary of which effects each function performs and where handlers
are installed across the call graph. Complements `kai type` for
effect-row-level queries.

**Cost**: low once effect inference is working — it walks the
inferencer's output.
**Depends on**: stage-2 effect inference.

## 6. `?e` — effect holes

```kai
fn run() : Int / ?e {
  perform Io.read_line() |> string_to_int |> unwrap
}
```

The compiler infers `?e` and reports it like a regular hole: `?e`
resolves to `Io + Fail`. Useful when the user does not yet know which
effect row a function belongs to — common during exploratory work
and when an LLM is drafting a signature.

**Cost**: low. Effect rows are already unification variables during
inference; this exposes one as a named hole.
**Depends on**: typed holes + effects.

## 7. `import ?name` — dependency holes

```kai
import ?parse_expr
```

When the user knows the symbol they need but not the module, the
compiler searches the stdlib and project modules and reports
candidates:

```
foo.kai:1:8: import hole

  looking for a module that exports `parse_expr`:
    - syntax.parser           (pub fn parse_expr(...))
    - experimental.pratt      (pub fn parse_expr(...))

  replace `?parse_expr` with one of those paths.
```

**Cost**: low. Needs a symbol index over the loaded modules; the
resolver already visits them.
**Depends on**: module resolution (stage 2).

## 8. `kai lint --json` — canonical style as data

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

## 9. Tuples — anonymous products

```kai
pub fn player_turn(hand: [Card], deck: [Card]) : ([Card], [Card]) {
  if hand_score(hand) >= 21 { (hand, deck) }
  else { ... }
}

let (new_hand, new_deck) = player_turn(hand, deck)
```

`kaikai-minimal.md` excludes tuples for the minimal subset ("No
tuples (use records; or use sum-type variants with positional
fields)"). For **full kaikai** the status is open: it falls under
`design.md`'s *"Concrete syntax consolidation: eliminate redundancies
— collections `[]`/`()`/`{}`"*, which is explicitly deferred.

This entry is here so the debate can be closed concretely rather
than by default.

### What they buy

- **Anonymous products** where field names are noise. Pairs like
  `(hand, deck)`, `(key, value)`, `(found, rest)` are semantically
  pairs; inventing two field names is ceremony that obscures intent.
- **Positional destructuring** for small fixed products. List
  patterns already support `[a, b, ...rest]`; tuples extend the same
  idea to finite-arity products.
- **Shorter signatures**. `: ([Card], [Card])` vs
  `: { hand: [Card], deck: [Card] }` saves both the field names in
  the return type and the field-access chatter at every use site.

### What they cost

- **Second form of product type** alongside records. Violates "one
  canonical form" unless justified by a crisp split: tuples for
  anonymous products, records for named ones.
- **Syntactic disambiguation**: `(x)` is grouping. `(x,)` for
  1-tuples (Python-style) is the mandatory wart.
- **Parser bookkeeping**: a parenthesised expression list must be
  distinguished from function-call argument lists. Local cost.

### Decision posture

Two coherent positions:

1. **Reject**: keep records, keep the surface small. Record
   punning (proposal 10 below) covers most of the ergonomic gap
   without adding a second product form.
2. **Accept**: give anonymous products their own form. Reserve
   records for cases where field names carry meaning. This matches
   how kaikai already distinguishes `[...]` (ordered) from records
   (named) — tuples complete the matrix for finite arity.

Neither is obviously right. A concrete measurement: rewriting the
blackjack example (≈176 lines of kaikai) to use tuples where it uses
records for anonymous pairs produces **zero line-count savings** —
every `{ x: x, y: y }` expression already fit on its own line, and
`(x, y)` takes the same line. The gain is per-line readability, not
fewer lines. That is a real but smaller win than one might guess.

Line-count impact may be larger in code that uses many multi-return
helper functions, where `let (a, b) = ...` collapses two
binding-lines into one. Blackjack does not exercise that pattern
heavily.

### Decision gate — post-m8

The decision is **deferred to a measurement protocol after m8
closes**. Rationale: blackjack (the only concrete data point so
far) is sequential, has no fibers/actors/nurseries, and does not
exercise the patterns where tuples plausibly help most:

- `Spawn.spawn(...)` returning `(Pid, Fiber)`.
- `nursery { n -> ... }` closing with `(results, errors)`.
- Parsers/scanners returning `(value, rest_of_input)`.
- Pattern-destructured multi-return in `match` arms.

Post-m8 (fibers + actors + nurseries shipped, Doc B catalog
fully populated), pick a representative effect-heavy program
(actor-based service, parser combinator suite, or supervised
worker pool) and rewrite it under each posture. Decision rule:

- **≥10% line-count savings** OR **≥30% reduction in average
  signature length on multi-return functions** → **accept**;
  schedule a follow-up milestone for tuples + destructuring
  patterns.
- **Below those thresholds** → **reject formally**; close the
  open decision, remove tuples from this catalog, document
  record punning (#10) as the canonical answer.

The gate runs as a one-day measurement task scheduled in
`docs/stage2-design.md` immediately after m8.

**Cost**: low-to-medium. Parser changes are local; type-system
extension is one product rule per arity.
**Depends on**: m8 close (fibers + actors + nurseries available
for the measurement corpus).

## 10. Record punning `{ x, y }` — additive sugar

```kai
# Today:
{ hand: hand, deck: deck }

# With punning:
{ hand, deck }
```

When the field name matches the variable in scope, repeating both is
noise. Punning desugars `{ x, y }` to `{ x: x, y: y }`. Not a new
product form; purely a parser-level rewrite. Same shape as the
placeholder `.` lambda (`. < 5` desugars to `(x) => x < 5`).

Applies symmetrically in patterns: `let { hand, deck } = pair`
already works when field names are explicit; punning lets it be
written `let { hand, deck } = pair` without the `: hand` / `: deck`
suffix.

Aligns with all principles — doesn't add a construct, only shortens
a common pattern. Independent of the tuples decision: even if
tuples ship, records still benefit.

**Cost**: trivial. A desugar at parse time.
**Depends on**: nothing.

## 11. `variants[T]()` — enumerate sum-type constructors

```kai
type Rank = Two | Three | ... | Ace

let all_ranks = variants[Rank]()
# [Two, Three, Four, Five, Six, Seven, Eight, Nine, Ten,
#  Jack, Queen, King, Ace]
```

A builtin (not a macro) that, for a sum type `T` with
*nullary* constructors only, returns the list of all constructors.
The compiler has this information after resolution; exposing it is
mechanical.

Restricted to nullary constructors on purpose: once any variant
takes arguments, "list all inhabitants" is ill-defined.

**Cost**: low. A prelude entry, resolved at monomorphisation.
**Depends on**: stage-2 monomorphisation pass.

## 12. Sum types with constant attributes

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
   resolution. If someone later proposes "extend this to derive
   across types," that is a separate, much bigger proposal.

### Alternative available today

The refactor `fn rank_info(r) -> { value, label }` + two accessors
achieves the same effect in 19 lines vs the 15 lines of the attribute
declaration. For a single enum, savings are modest. The case for
this proposal rests on codebases with many small enums carrying a
couple of constants each — the multiplier is in breadth, not depth.

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

## 13. `!` postfix — `Option` / `Result` propagation

```kai
fn parse_config() : Result[Config, Error] {
  let raw = read_file("config.toml")!
  let parsed = parse_toml(raw)!
  validate(parsed)
}
```

The `!` suffix on an expression of type `Option[T]` or `Result[E, T]`
unwraps the success case into a `T` and short-circuits the enclosing
function on the failure case. Equivalent to Rust's `?`, renamed
because `?` is already taken by typed holes.

**Status**: already reserved in `docs/kaikai-minimal.md` ("`!` is
reserved (post-minimal uses for `Option`/`Result` propagation)").
This entry formalises the semantics.

### Semantics

- `expr!` on `Option[T]`:
  - `Some(x) -> x`
  - `None` → the enclosing function returns `None`.
  - The enclosing function's return type must be `Option[U]` for
    some `U`.
- `expr!` on `Result[E, T]`:
  - `Ok(x) -> x`
  - `Err(e)` → the enclosing function returns `Err(e)`.
  - The enclosing function's return type must be `Result[E, U]`
    for the same error type `E` (no `From`-style coercion — that
    would be a typeclass).
- Type error if the enclosing function's return type is incompatible.
  The error message must name both types and point at both the `!`
  and the function signature.

### What it buys

- Linear success paths: the happy case reads top-to-bottom, without
  match pyramids.
- Errors propagate with one character, not a five-line match.
- The `!` is **visible at the call site** — it is not implicit
  propagation. Readers see where the early returns are.

### What it costs

- Adds a postfix operator. Parser work is local.
- Fixes the semantics to `Option` and `Result` only. No user-defined
  "monad-like" types can participate — that would require typeclass
  resolution, which Tier 1 forbids.

### Explicit non-goals

- **No `From`-style error conversion.** If the function returns
  `Result[AppError, T]` and the callee returns `Result[IoError, T]`,
  `callee()!` is a type error. The user must convert explicitly.
  Rust added `From` to its `?` and paid with typeclass complexity;
  kaikai does not.
- **No user-defined `!`.** It is not a trait method. It is compiler
  sugar bound to two specific types.

**Cost**: low. Parser + two desugar rules (one for `Option`, one for
`Result`) in the typed-IR pass.
**Depends on**: `Option` and `Result` in prelude (present since
kaikai-minimal).

## 14. `@` as-patterns in `match`

```kai
match xs {
  all@[h, ...t] if needs_original(h) -> {
    log(all)                    # the full list, not rebuilt
    process(h, t)
  }
  [] -> default()
}
```

`name@pattern` binds `name` to the whole scrutinee when `pattern`
matches, *and* destructures as `pattern` for the remaining pattern
variables. Same semantics as Haskell and Rust.

### What it buys

- Access to both the destructured pieces and the whole value in one
  arm, without rebuilding `[h, ...t]` into a new list or introducing
  a `let all = xs; match all { ... }` wrapping.
- Common in linting, logging, and audit code where the original
  structure must be referenced alongside its parts.

### What it costs

- Adds one pattern form. The grammar gains `Ident "@" Pattern` as a
  pattern alternative.
- No runtime cost — the binding is already computed.

### Constraints

- Only in `match` arms and `let` bindings, both already established
  pattern contexts.
- The bound name must be a fresh identifier (no shadowing of an
  existing binding in scope — diagnostic required).

### Symbol reuse: `@` in two roles

`docs/effects-stdlib.md` (Doc B / m7b) uses `@cap` as a prefix
unary operator on expressions — `@counter` means `counter.get()`
on a `State[T]` capability binding, `@config` means
`config.ask()` on a `Reader[T]` one. The contexts are disjoint:
this proposal's `@` is *infix*, between an identifier and a
pattern, and appears only where patterns are legal (`match`
arms, `let` bindings). Doc B's `@` is *prefix*, on expressions.

No grammatical ambiguity — the parser can tell which role applies
from the surrounding production. If this proposal lands, the
documentation for both should cross-reference the other so readers
know `@` plays two disjoint roles.

**Cost**: trivial. One grammar rule plus one resolve-pass case.
**Depends on**: parser.

## 15. `?.` optional chaining

```kai
# user: Option[User]; u.profile: Option[Profile]; p.display_name: String
let name: Option[String] = user?.profile?.display_name
```

Safe navigation over `Option`. Desugars to nested `opt_and_then`
calls that short-circuit on `None`. The semantics matches
Swift / Kotlin, except restricted to `Option` (no reference nullability
in kaikai).

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
  `list_nth` + `opt_and_then` covers it for now. Reconsider if
  demand appears.

**Cost**: low. Parser addition plus one desugar in the typed-IR
pass (to nested `opt_and_then`).
**Depends on**: parser + type checker.

## 16. Bit operations — schedule m13 (intrinsic functions)

```kai
import bit

fn pack_byte(bits: [Int]) : Int =
  bits |> reduce(0, (acc, b) => bit.or(bit.shl(acc, 1), b))

fn extract(byte: Int, pos: Int) : Int =
  bit.and(bit.shr(byte, pos), 1)
```

A concrete demand surfaced (`demos/9d9l/huffman` bit-packing,
upcoming `crypto` / `encoding/base64` stdlib modules). Bit ops ship
as **named functions in `stdlib/core/bit.kai`**, not as new
operators. The compiler recognises them as intrinsics and lowers
each call directly to the backend's bit op — same code emitted as
if `&`/`^`/`<<`/`>>` were syntax, no function-call overhead.

### Why functions, not operators

- **Surface stays small**: no new tokens, no precedence rules, no
  parser change. CLAUDE.md Tier 1 #3 (fast compilation) and Tier 2
  #6 (few visible concepts) both reward this.
- **No conflict with `|`**: the map-pipe keeps its slot. Asymmetric
  operator sets (`&`, `^`, `<<`, `>>` but not `|`) are avoided too;
  the function form is uniform.
- **Performance**: the compiler treats `bit.and` / `bit.or` /
  `bit.xor` / `bit.shl` / `bit.shr` / `bit.not` as **intrinsics** —
  they are recognised by name in the typed IR and emitted directly
  as the corresponding C / LLVM bit op. Zero call overhead;
  monomorphisation is unnecessary because they are already
  monomorphic on `Int`.
- **Readability**: in code that does heavy bit manipulation
  (compression, crypto, parsing binary formats), the bit ops are
  one node per call — the same density as operator chains. The
  cost is a few extra characters per line, which buys parser /
  lexer / precedence-table simplicity.

### The `bit` module

```kai
# stdlib/core/bit.kai

pub fn bit.and(a: Int, b: Int) : Int           # intrinsic
pub fn bit.or(a: Int, b: Int)  : Int           # intrinsic
pub fn bit.xor(a: Int, b: Int) : Int           # intrinsic
pub fn bit.not(a: Int)         : Int           # intrinsic
pub fn bit.shl(a: Int, n: Int) : Int           # intrinsic, signed left shift
pub fn bit.shr(a: Int, n: Int) : Int           # intrinsic, arithmetic right shift (sign-extending)
pub fn bit.ushr(a: Int, n: Int) : Int          # intrinsic, logical right shift (zero-fill)

# Non-intrinsic helpers (real functions, but pure):
pub fn bit.popcount(n: Int) : Int
pub fn bit.leading_zeros(n: Int) : Int
pub fn bit.trailing_zeros(n: Int) : Int
pub fn bit.rotate_left(n: Int, k: Int) : Int
pub fn bit.rotate_right(n: Int, k: Int) : Int
```

The first seven are intrinsics — recognised by the compiler at the
typed-IR level and lowered directly. The auxiliary helpers are
ordinary kaikai functions implemented in terms of the intrinsics.

### What it costs

- **Stdlib**: one new module file.
- **Compiler**: an intrinsic-recognition pass between type
  inference and codegen. Walk the typed IR; for each call to a
  `bit.*` symbol that the table marks intrinsic, replace with the
  corresponding low-level node. ~50 lines.
- **Backend (C and LLVM)**: emit the corresponding op for each
  intrinsic node. ~30 lines per backend.
- **No lexer / parser / precedence change**.

Total ~1 day at observed velocity.

### Decision posture

**Schedule m13**, alongside property testing and bench. The crypto
and encoding stdlib modules planned for m14 lean on this; landing
the intrinsics just before keeps that stdlib code performant
without resorting to FFI.

## 17. `Map[K, V]` — hash-map / associative container

```kai
let scores: Map[String, Int] = Map.empty()
    |> Map.insert("alice", 10)
    |> Map.insert("bob",   7)

# With indexing sugar (shipped with this proposal):
let n = scores["alice"]                    # Option[Int]
scores["charlie"] := 42                    # insert-or-update
```

kaikai has no general-purpose associative container in v1 —
users compose with pairs and `list_*` helpers, or reach for
`Array[T]` when keys are integer-shaped. A `Map[K, V]` type
closes the gap.

### What it buys

- An idiomatic container for lookup-by-key, which is the second
  most common shape after sequences. Today the workaround is
  `[(K, V)]` with linear scans — fine for tiny maps, abysmal
  at scale.
- The natural place for the already-reserved `m["key"]` and
  `m["key"] := v` indexing, which `docs/syntax-sugars.md` §4.3
  notes but intentionally leaves undefined until `Map` lands.

### What it costs

- A new stdlib type plus its algorithms (insertion, lookup,
  iteration, deletion). HAMT-style persistent or mutation-with-
  `Mutable` — that decision is the hardest part of this
  proposal.
- A second indexing shape in the grammar (non-integer keys),
  which the checker must disambiguate from `Array` indexing.
  Only a problem if the decision is to make `Map` indexable
  with the same `[]`; if the decision is a distinct operator
  (`m#["key"]` or similar), the cost collapses.
- Equality and hashing: the language needs a story for what
  types can be map keys. `String` / `Int` are obvious; records
  less so.

### Decision axes

Three coupled decisions to close this:

1. **Persistent vs ephemeral.** `Map[K, V]` as a persistent
   structure with structural sharing (no `Mutable` in the row)
   vs a mutable hash table (paying `Mutable`). Persistent
   matches the functional-first slant of kaikai; ephemeral is
   typically faster.
2. **Indexing syntax.** Same `a[i]` as `Array`, a distinct
   operator, or method-only (no indexing sugar).
3. **Key constraints.** Any type, types with derived `Eq` /
   `Hash`, or a hand-curated set (`String`, `Int`, …).

None is obviously right; each pushes the design in a different
direction. This entry exists so the decisions can be made
together rather than drifting independently.

**Cost**: medium-to-high. A new type plus semantic design;
several days of type-system and stdlib work.
**Depends on**: a collection-design pass that also closes
`Vector[T]` (see Doc B §`Mutable` *Known gap*).

## 18. Slice syntax `a[i..j]`

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

## 19. Method references as first-class values (point-free)

```kai
# Today (lambda required):
options | (f) => n.spawn(f)
xs      | (x) => x.label
list    |> reduce(0, (acc, x) => acc + x.cost)

# With point-free:
options | n.spawn
xs      | .label              # placeholder already covers this case
list    |> reduce(0, (+).on(.cost))   # if `(+).on(...)` is also added
```

A method or operation reference, written as `obj.method` or
`Type.method` without trailing parens, would denote the
function value `(args...) => obj.method(args...)`. Same idea as
Kotlin's `obj::method`, Scala's `obj.method _`, or Python's
unbound-method syntax — applied to kaikai's effect ops and
records.

### What it buys

- The map pipe (`|`) and other higher-order helpers
  (`reduce`, `filter`, `each`) compose without lambda
  ceremony when the function you want to apply is already a
  named method or op.
- Encourages reading code as a flow of operations rather than
  an explicit lambda-per-step pipeline. Several functional
  languages take this further with first-class composition;
  kaikai already has `|>` and `|`, so first-class method refs
  is the missing piece for clean pipelines.

### What it costs

- Parser ambiguity to resolve: `obj.method` in expression
  position must be valid both as a value (the reference) and
  as the start of a call (`obj.method(args)`). Solvable with
  one-token lookahead — if the next token after `.method` is
  `(`, parse as call; otherwise as reference.
- The placeholder `.field` lambda (`xs | .label`) already
  covers a subset of the use case — for record field access.
  This proposal extends the same convenience to ops and
  methods generally.
- A method reference closes over the receiver — `n.spawn`
  captures `n`. Lifetime / region implications need a small
  amendment to the brand machinery (`n.spawn` cannot escape
  the nursery any more than `Fiber[T]` can).

### Decision posture

Wanted in the language; deferred until after m7b. The current
workaround is the explicit lambda `(f) => n.spawn(f)`, which
adds 13 characters per call site but compiles unambiguously
today. Once m7b lands and effect rows are stable, revisit
this proposal as a clean ergonomic improvement.

**Cost**: medium. Parser change (one extra production), brand
extension for receiver-bound references, and a clear story
about evaluation order (when does the receiver get
evaluated — at the reference site, or at the call site?).
**Depends on**: parser; effect-row stability from m7b.

## 20. `Range[T]` as a first-class iterable

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

## 21. Pipeline placeholder `_`

```kai
# today (with |> applying as first arg)
x |> add(2)            # add(x, 2)

# with placeholder
x |> add(2, _)         # add(2, x)
x |> insert(map, _, v) # insert(map, x, v)
```

`_` marks the position the piped value should land in. The
default (no `_`) keeps current semantics — first argument for
`|>`, mapped element for `|`. Without `_` the user falls back to
`x |> (a) => fn(b, a)`, which kills the pipeline shape every
time the desired slot is not the first.

The placeholder is an *expression-level* token, distinct from `.`
(implicit-lambda placeholder used in `xs | . > 0`). The two do
not overlap: `.` builds an unary lambda over the lambda-implicit
position; `_` substitutes inside an existing call expression.
Multiple `_` in the same call are an error (would silently
duplicate side effects of the LHS).

**What it buys**: keeps long pipelines readable when functions
do not consistently take the value as first argument
(common in stdlib: `String.replace(haystack, needle, repl)`,
`Map.insert(m, k, v)`).

**What it costs**: low. Parser sugar — desugars to a fresh
binding (`let __tmp = lhs in fn(arg, __tmp, arg)`) at the
pipeline-application site. No type-system change.

**Depends on**: parser. Lands cleanly post-m7b without affecting
effect rows or monomorphisation.

**Reference**: F# 7 (`|> add 2 _`), Hack (`|> $$.method()`),
Scala 3 (`_` in eta expansion). The F# 7 form is the closest
match; the desugaring is identical.

## 22. Binary pattern matching `<<...>>`

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
prelude. Independent of effects/fibers — cleanly post-m12.

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

## 23. `++` operator — string and list concatenation

```kai
let greeting = "hello, " ++ name ++ "!"
let merged   = sort(lesser) ++ [pivot] ++ sort(greater)
```

A single binary operator covering the two concat cases that
appear constantly in functional code. Resolves to the existing
prelude functions in lowering:

- `String ++ String  : String`  → `string_concat(a, b)`
- `[a]    ++ [a]     : [a]`     → `list_append(xs, ys)`

Right-associative, lower precedence than `+`/`-`/`*`/`/`,
higher than comparison operators. No typeclass; the typer has
exactly two hardcoded rules — same posture as numeric `+`
overloaded for `Int` and `Real`.

### What it buys

- **List concat reads as concat**: `sort(lesser) ++ [pivot] ++
  sort(greater)` versus `list_concat([sort(lesser), [pivot],
  sort(greater)])` or nested `list_append`s. The list case is
  the one that actually hurts; string case rides along.
- **Familiar to every senior**: Haskell, Elm, Erlang use `++`;
  Elixir uses `<>`/`++`; OCaml uses `^`/`@`; F# uses `+`/`@`.
  Whichever one a senior knows, kaikai's `++` reads correctly
  on first glance.
- **Removes a verbosity that interpolation does not solve**:
  interpolation handles strings; lists have no interpolation
  analogue.

### What it costs

- **Parser**: one new binop token, one precedence slot. ~10
  lines.
- **Typer**: two type rules, no overload machinery. ~30 lines.
- **Codegen / desugar**: rewrite `++` to `string_concat` /
  `list_append` based on the typed AST. ~30 lines.
- **Stdlib doc churn**: mark `string_concat` and `list_append`
  as "lowering targets; prefer `++` in source".

Total ~0.5 day at observed velocity.

### Constraints

- **Migration, not addition**: surface drops `string_concat` and
  `list_append` from idiomatic style — they remain in stdlib as
  the lowering targets but are not the form callers reach for.
  This avoids the *intent overlap* concern from CLAUDE.md (the
  Tier 2 rule against multiple forms with the same intent).
- **Strings only via `++`, not `+`**: kaikai keeps `+` purely
  numeric. Overloading `+` for strings (Java/JS/Python style)
  collides with the strict-arithmetic discipline and would
  require typeclass-style dispatch the language explicitly
  rejects.

### Reference

Haskell (`++` for `[a]` and `String = [Char]` unified), Elm
(`++` for any `appendable`), Erlang (`++` for lists, `<>` for
binaries; kaikai folds both into `++`), F# (`@` for lists,
`+` for strings — kaikai picks the more uniform answer).

## 24. `main()` row inference

```kai
# today: every effect main uses must appear in the row
fn main() : Unit / Console = Console.print("hi")

# proposed: main is special — typer infers the row, runtime
# loads the corresponding default handlers automatically
fn main() = Console.print("hi")
```

The current rule "row annotation mandatory in public signatures"
bites at the entry point with no benefit: `main` has no caller
that needs to read its row, and there is no API surface to
stabilise. Forcing the annotation is friction without payoff —
a senior writing their first kaikai program is asked to declare
something they cannot use wrong.

### What it buys

- **Hello-world is one line again**: `fn main() = Console.print("hi")`.
- **Demos in `demos/`** drop ceremonial `: Unit / Console + Stdin +
  Fail` rows on `main` that exist only to satisfy the typer.
- **Reduces a confusing error class**: today, removing the row
  from `main` yields "undefined name `Console`" because the
  resolver does not load the effect that nothing in the
  signature references. With inference, the body's mention of
  `Console.print` is enough to load the effect.

### What it costs

- **Typer**: relax the annotation requirement for the `main`
  symbol; run row inference on the body and adopt the inferred
  row as `main`'s declared row. ~30 lines.
- **Runtime / driver**: the default-handler loader already walks
  `main`'s row to pick which defaults to install. With inference
  the row comes from the typer pass instead of the source
  signature; the loader otherwise unchanged.
- **Diagnostics**: when `main`'s body uses an effect with no
  default handler, the existing "effect not handled" diagnostic
  fires with the same shape as today. No new error class.

### Constraints

- **Only `main`**: every other public function still requires an
  explicit row. The exception is justified by `main` being the
  entry point with no caller; generalising to "all public fns"
  re-opens the row-inference debate the design already closed.
- **Return type still optional for `main`**: `fn main() = ...`
  reads as `: Unit` by default; `fn main() : Int = exit_code`
  stays valid for explicit exit codes.

**Cost**: low. ~0.5 day.
**Depends on**: m7a (effect rows in types) ✓.

**Decision posture**: scheduled for m7e — pure ergonomics, zero
risk. Removes a recurring papercut without compromising the
"effects visible in types" principle: effects are still visible
at every call site (`Console.print`, `File.read`, etc.); they
are simply not restated in `main`'s signature where they were
already implied.

## 25. `use Effect` — open an effect in scope

```kai
# file-level — applies to every fn in the file
use Console

fn greet(name: String) = println("hello, #{name}")
fn shout(msg: String)  = eprint("ERR: #{msg}")
fn main()              = greet("world")
```

```kai
# block / fn-level — narrowed scope
fn write_log(line: String) = {
  use File
  let f = open("log.txt")    # File.open
  write(f, line)             # File.write
  close(f)                   # File.close
}
```

A declaration that brings an effect's operations into the
surrounding scope without the cap selector. After `use E`,
every op declared in `E` is callable by its bare name, provided
the resolution is unambiguous.

### What it buys

- **Compactness for I/O-heavy demos**: hello-world, fizzbuzz,
  beer_song read as `println(...)` instead of `Console.println(...)`,
  matching the ergonomics every senior expects from the language.
- **No global pollution**: scoped to the function (or block) it
  appears in; outside that scope the operations are not visible
  by their bare name.
- **No regression of "effects visible"**: the row of the
  function still declares `/ Console`. The dot syntax simply
  becomes optional inside the scope where `use` opens it.
- **Clear error story**: if two open effects expose the same op
  name (e.g. `Console.print` and `File.print` both in scope),
  the resolver emits an ambiguity error and forces explicit
  qualification. Bare names work only when unambiguous.

### What it costs

- **Parser**: `use Name` as a statement — one new keyword,
  trivial. ~10 lines.
- **Resolver**: track an effect's ops as additional bindings in
  the local scope; reject ambiguous bare-name resolutions.
  ~50 lines.
- **Typer**: unchanged. The bare name still resolves to the
  same op call; the row of the calling function still gains
  the effect.
- **Diagnostics**: one new error class — "ambiguous bare name
  `print`: in scope from both `Console` and `File`; qualify
  explicitly." Cheap to produce.

### Constraints

- **Three valid scopes**:
  - File-level `use` at the top of a `.kai` file applies to
    every declaration in the file. Recommended for files
    that lean heavily on a single effect (I/O-bound demos,
    `Console`-only CLI programs).
  - Block / fn-level `use` narrows the open to a scope.
    Useful when a single fn touches a different effect from
    the file's default, or in a library file that wants no
    file-level imports.
  - Inner `use` shadows outer for the duration of its scope.
- **Multiple `use` allowed** with an ambiguity rule: if two
  open effects expose the same op name (e.g. `Console.print`
  and `File.print` both in scope), the resolver emits an
  ambiguity error and forces explicit qualification. Bare
  names work only when unambiguous.
- **Does not remove the cap from the row**: `fn main() : Unit
  / Console = { use Console; println("hi") }` still has
  `/ Console` in its row — `use` is purely a name-resolution
  shortcut, not an effect-handling change.
- **Composes with #24**: `main()` row inference removes the
  row annotation; `use` removes the cap selector. Together
  they collapse hello-world to:
  ```kai
  use Console
  fn main() = println("hi")
  ```
  Neither requires the other.

### Reference

OCaml `open Module`, F# `open`, Rust `use`, Haskell `import M`
without qualifier. The mechanism is identical to those
languages, applied to *effects* rather than modules — the
underlying resolution mechanism is the same.

**Cost**: low. ~0.5 day.
**Depends on**: m7a (effect rows + cap selectors) ✓.

**Decision posture**: scheduled for m7e alongside #24
(`main()` row inference). The two ergonomic wins compose into
"hello-world is one line" — the most-asked first impression of
any new language.

## 26. Protocols — single-dispatch ad-hoc polymorphism

```kai
protocol Show {
  show(x: Self) : String
}

impl Show for Money[u: Unit] {
  show(m) = decimal_repr(m.amount) ++ " " ++ unit_name(u)
}

let usd = 100.50<USD>
println("balance: #{usd}")            # "balance: 100.50 USD"
```

Explicit `protocol` declarations + `impl` blocks. **Single dispatch**
on the first-position type tag. Modeled on Clojure protocols, Elixir
protocols, Go interfaces (with explicit declaration), and the
lightweight subset of Rust traits.

This is **not** Haskell typeclasses. Specifically: no higher-kinded
types, no constraints in signatures, no functional dependencies, no
type families, no superclass constraints, no overlapping instances.
Single dispatch only.

### Why this is consistent with CLAUDE.md

CLAUDE.md Tier 1 #3 prohibits "type-class resolution". The phrase
targets the Haskell-style solver — constraint propagation, instance
chains, multi-param dispatch with functional dependencies — which is
the source of slow compile times and subtle resolution rules.

Single-dispatch protocols (Go / Clojure / Elixir / Rust traits in
their non-HKT form) do **not** invoke that solver. Resolution is an
`O(1)` hash lookup by `(protocol, type)` pair, scoped to the
transitively imported impl table. Coherence is enforced by the
orphan rule (impl must live in the protocol's module or the type's
module) plus single-impl-per-pair check. No constraint propagation.

The distinction is real and well-attested:

| Capability | Haskell typeclass | kaikai protocol |
|---|---|---|
| Higher-kinded types | yes (`Functor`, `Monad`, ...) | **no** |
| Constraints in signatures | `Show a => a -> String` | **no** |
| Multi-param classes | yes (with fundeps) | **no** (single dispatch) |
| Type families | yes | **no** |
| Compile cost | high (solver) | low (`O(1)` lookup) |
| Runtime cost | mostly direct (after monomorph) | direct when monomorphic, 1 indirection otherwise |

CLAUDE.md should be amended to clarify the distinction; the m12.8
milestone includes that doc update.

### What it buys

- **`#{x}` works for any type with `impl Show`**. The interpolation
  no longer requires explicit conversion functions for primitive-like
  types or the user's custom data.
- **One mechanism, multiple uses**: `Show`, `Eq`, `Ord`, `Hash`,
  `Serialize` all share the same machinery. Users add new protocols
  for domain-specific cases (`Auditable`, `Renderable`, etc.) within
  the same coherence rules.
- **`#derive(Show, Eq)` for records**: structural impls auto-generated
  by the compiler when the user opts in via the `#derive` annotation.

### What it costs

- **Parser**: `protocol`, `impl`, `Self`, `#derive` keywords. ~50
  lines.
- **Resolver**: per-module impl table; orphan-rule check; single-impl
  enforcement. ~60 lines.
- **Typer**: resolve protocol op calls; substitute the impl function
  during monomorphization. ~50 lines.
- **Codegen**: vtable per `(protocol, type)` pair; lookup helper for
  late-binding sites. ~50 lines (C backend; LLVM port follows the
  m7c pattern).
- **Stdlib**: declare `Show` / `Eq` / `Ord` / `Hash` / `Serialize`;
  impls for primitives. ~80 lines.
- **`#derive` annotation**: structural-impl generator. ~50 lines.
- **Tests**: ~80 lines.

Total ~450 lines, **2-3 days at observed velocity**.

### Constraints

- **Single dispatch only**. Two-arg dispatch (e.g. `convert(from: T,
  to: U)`) is not supported. Workaround: free function with explicit
  match.
- **Pure protocols**: protocol ops cannot have effect rows. Behaviour
  with effects (audit, log, persist) goes via effects, not protocols.
- **Closed coherence**: orphan rule enforced at compile time. No
  global registry, no overlapping impls.
- **Opt-in derivation**: `#derive(Show)` is required for auto-generated
  impls on records / sum types. No silent default impls.
- **No HKT, ever**. Re-opening this means a separate proposal with
  fresh cost analysis, not extension of this one.

### Reference

- Clojure protocols: <https://clojure.org/reference/protocols>
- Clojure multimethods: <https://clojure.org/reference/multimethods>
  (more flexible dispatch; kaikai picks the simpler protocol model)
- Elixir protocols: `defprotocol` + `defimpl`
- Go interfaces: structural typing without explicit `impl`; kaikai
  picks the explicit-impl model for clarity at impl sites
- Rust traits: kaikai's design subset is "traits without HKT,
  without GAT, without `dyn`, without coherence-orphan-tricks"

**Decision posture**: scheduled m12.8, after m12.7 axiom and before
m13. Lands as its own milestone with full design doc in
`docs/protocols.md`.

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
  `King` all want the same value `10`). The broader proposal #12
  (sum types with constant attributes) covers the same use-case
  without the uniqueness constraint, which is why this narrower
  form is not on the list.
- **`deriving Show` / typeclass-style auto-derivation**: directly
  conflicts with the principle *no costly type-class resolution*.
  If automatic stringification is ever desired, ship it as a `kai`
  tooling codegen, not as a typeclass. Note that proposal #12
  (attribute sums) handles the label case for *closed* sum types
  without needing typeclasses — that is the intended solution.
- **Early `return` statement**: kaikai is expression-based; *last
  expression is the value* is canonical. Adding `return` would
  create a second exit form and nest imperative control flow into
  an otherwise expression-oriented surface.
- **`&&` / `||` for Bool**: duplicate `and` / `or` without distinct
  intent. `and` / `or` stay as the canonical boolean operators.
- **`<-` as monadic bind / effect shorthand**: `!` (proposal #13)
  already covers the propagation pattern for `Option` / `Result`;
  kaikai's direct-style effects do not need a bind operator. Adding
  `<-` would be a second mechanism for the same flow.
- **`$` low-precedence apply (Haskell-style)**: `|>` and `|` already
  avoid paren pyramids. `$` would be a third way to write the same
  chain.
- **`::` path or type ascription**: `.` already separates module
  paths (`math.vector.dot`) and `:` annotates types. `::` would
  collide with one of them without adding intent.

## Adoption criteria

### For LLM-friendly diagnostics (sections 1–8)

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

### For language-surface features (sections 9–20)

These land only alongside the closing of *"Concrete syntax
consolidation"* in `design.md`. Any decision on tuples or punning
should be made as part of that one conversation — not drip-fed.

- **Tuples**: ship only if the decision is to accept them as the
  canonical form for anonymous products, simultaneously reserving
  records for named-field use. A split rule is required, not both
  forms as interchangeable.
- **Record punning**: safe to land independently of the tuples
  decision — it's strictly additive sugar.
- **`variants[T]()`**: safe to land alongside stage-2
  monomorphisation.
- **Sum types with constant attributes**: land only after confirming
  (a) the pattern appears in ≥3 independent enums in stdlib or user
  code, and (b) the constraint set in section 12 has survived at
  least one design review without drifting toward methods,
  typeclasses, or non-constant attributes. Until then, prefer the
  `fn_info + accessors` refactor.
- **`!` postfix (`Option` / `Result` propagation)**: safe to land
  once `Option` and `Result` are fully in the stdlib and a small
  corpus of error-handling code exists to verify the ergonomics.
  The syntax is reserved today (`kaikai-minimal.md`); formalisation
  just fills in the semantics.
- **`@` as-pattern**: strictly additive to the pattern grammar,
  independent of everything else. Land whenever a case study shows
  it avoiding a meaningful amount of `let x = scrutinee; match x { ... }`
  boilerplate.
- **`?.` optional chaining**: land after `!` has been used enough
  to confirm the two do not overlap in practice. `?.` is local;
  `!` propagates. If the code base mostly wants propagation, `?.`
  may not be worth the complexity — re-evaluate then.
- **Bitwise operators**: deferred. Stays as named functions
  (`bit_and`, `bit_or`, …) until there is code that demonstrably
  suffers. This is the "wait for demand" case, not the "principle
  block" case.
- **`Map[K, V]` + indexing**: land together with `Vector[T]`
  (Doc B §*Out of scope for v1*) as a single collection-design
  pass. The persistent-vs-ephemeral, indexing-syntax, and
  key-constraint axes in §17 are coupled decisions.
- **Slice syntax `a[i..j]`**: lands *after* `Vector[T]` ships.
  Copying semantics over `Array[T]` is the first-approximation
  implementation; view-based slices with region tracking wait
  for region types to prove themselves elsewhere.
- **Method references as values**: wanted in the language for
  cleaner pipelines (`options | n.spawn` over the explicit
  lambda). Deferred until after m7b so effect rows are stable
  and the brand machinery can be extended cleanly to
  receiver-bound references.
- **`Range[T]` as a first-class iterable**: lands together
  with the collection design pass (`Vector[T]`, `Map[K, V]`).
  Designing iterability across these three types in one go
  avoids retrofitting helpers later.

The goal is to keep the surface small. A handful of orthogonal,
well-integrated extensions is worth more than a pile of clever
features.
