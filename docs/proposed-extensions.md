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

**Closed extensions** (whether landed or rejected) are documented
in their corresponding milestone retrospectives, fixtures under
`examples/`, and CHANGELOG entries — not here. This file tracks
only what is still open.

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
| `Map[K, V]` + `m["key"]` indexing          | landed v1 (read-only sugar; tree carrier) — write sugar deferred to v2 alongside HAMT | — |
| Slice syntax `a[i..j]`                     | proposed | `Vector[T]` landing     |
| `Range[T]` as a first-class iterable       | proposed | collection design       |
| Binary pattern matching `<<...>>`          | proposed | parser + match exhaustiveness |
| Multi-arg `match` sugar — `match a, b { ... }` | landed v1 — N ≤ 4, column-aware diagnostics deferred | parser |
| `\|\|` flat-map pipe + `Sequence` protocol | proposed | parser + protocol dispatch in typer |
| `protocol P requires Q` (impl-site coherence check) | proposed (#172) | m12.8 protocols (landed) |

### Closed (for reference; details in commits / CHANGELOG)

- **Landed**: `todo!`, `axiom`, `kai effects --json` (`--effects-json`),
  `?e` effect holes, `import ?name`, record punning, `variants[T]()`,
  `!` postfix, `@` as-pattern, bit ops (flat + dotted), pipeline `_`
  placeholder, `++` operator, `main()` row inference, `use Effect`,
  protocols (single-dispatch m12.8), units of measure m12.5,
  `const NAME` (2026-04-28), method references via `.field`
  placeholder lambda (v1; receiver-bound `obj.method` deferred),
  record destructuring in `let`.
- **Rejected**: tuples as a second product form (m8.5 measurement
  gate, 2026-04-27 — see *Tuples — REJECTED* below for the
  retrospective).

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

## Tuples — REJECTED (2026-04-27)

This decision is preserved here because future contributors will
ask "why no tuples?" and need to find the answer rather than
re-debate it.

**Verdict**: tuples do **not** earn a slot as a second product form
in kaikai. Records cover anonymous-product use cases at the same
or smaller signature length once `Pair[a, b]` exists in stdlib.

### How the gate ran

After m8 closed (fibers + actors + nurseries shipped), a measurement
protocol picked a representative effect-heavy program and rewrote
it under each posture. Decision rule:

- **≥10% line-count savings** OR **≥30% reduction in average
  signature length on multi-return functions** → accept.
- Below those thresholds → reject formally.

The gate ran against a parser combinator suite (an arithmetic
calculator with two-level precedence and parens) — 7 multi-return
parsers `p_*` returning the "value + rest of input" shape, the
densest plausible exercise of multi-return.

**Methodology adjustment.** The natural strawman (one record per
parser shape: `ParsedExpr`, `ParsedInt`, etc.) was rejected as
unfair — it inflates the pro-tuples case. The honest baseline uses
one *generic* record `Parsed[a] = { value: a, rest: [Token] }`,
which kaikai already supports.

**Metrics (n = 7 multi-return parsers)**:

| Metric                              | Generic record | Tuples     | Δ      |
|-------------------------------------|---------------:|-----------:|-------:|
| Lines (non-blank, non-comment)      |            135 |        132 | −2.2%  |
| Avg signature length (chars)        |          51.43 |      54.43 | +5.8%  |

Both gates fail. Tuples save 3 lines (one for the `Parsed[a]`
declaration, two for collapsed constructions) — far short of 10%.
Signatures with tuples are *longer* than with generic records:
`Parsed[Expr]` is 12 chars; `(Expr, [Token])` is 15.

### Where tuples still win, and how kaikai answers without them

After the verdict, a stress test surveyed the canonical use cases
of tuples in other languages and found three honest gaps; **none
required tuples-as-a-type**.

1. **`Pair[a, b]` in stdlib** — a single generic record collapses
   the pattern of ad-hoc `type StmtsRewrite = { ... }` declarations.
   ```kai
   pub type Pair[a, b] = { fst: a, snd: b }
   ```
2. **Multi-arg `match` sugar** — `match a, b { PatA, PatB -> ... }`
   desugaring to a nested match. The `(a, b)` is *transient*: never
   escapes the arm. Tracked as the only pending language-surface
   item this gate produced; see proposal 9 below.
3. **Record destructuring in `let` + record punning** — together,
   `let { fst, snd } = transfer(a, b)` matches the ergonomics of
   tuple destructuring. Both shipped in m7d.

**Why the gate's verdict generalises**: tuples in other languages
excel where the host has weak records (Haskell pre-RecordDotSyntax,
OCaml's nominal-only records) or no generics over records. Kaikai
has neither deficit — `{ fst: a, snd: b }` is as terse as `(a, b)`,
and `Pair[a, b]` (10 chars) competes with `(a, b)` (5 chars) only
in the smallest cases.

The places tuples win unambiguously (transient pattern matching)
are not tuple-as-type; they are syntactic sugar over multi-arg
match.

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

## 6. `Map[K, V]` — landed v1 (issue #128, 2026-05-02)

`Map[K, V]` shipped as a v1 with the read-side indexing sugar.
The retrospective lives at the end of this section; the original
proposal text below stays for context. Write-side `m[k] := v`
and HAMT are deferred to v2 (out of scope for v1).

```kai
let scores: Map[String, Int] = map_empty()
    |> map_put("alice", 10)
    |> map_put("bob",   7)

# Read indexing sugar (v1):
let n = scores["alice"]            # Option[Int]

# Write indexing sugar — DEFERRED. The typer rejects this with
# a diagnostic pointing at map_put. v2 (alongside HAMT) brings
# it back with the right semantics over a persistent container.
# scores["charlie"] := 42         # error in v1
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

### Retrospective — v1 landed 2026-05-02 (issue #128)

The three coupled axes resolved as:

1. **Persistent vs ephemeral**: persistent. The carrier is a
   height-balanced AVL tree keyed by `<` (`O(log n)` lookup,
   insert, remove). HAMT was deferred to v2 — it wants a
   `Hashable` protocol kaikai does not have today.
2. **Indexing syntax**: same `[]` as `Array`, **read-only**.
   The typer dispatches `e1[e2]` by inspecting the inferred
   container type at the index site:
   - `Array[T]` keeps lowering to `array_get(e1, e2)`.
   - `Map[K, V]` lowers to `map_get(e1, e2) : Option[V]`.
   - Unresolved type variables default to `Array[T]` so
     existing diagnostics stay intact.
   `m[k] := v` is **rejected** with a typed error pointing at
   `map_put(m, k, v)`. The write semantics over a persistent
   container needs its own design; that lane lands alongside
   the HAMT carrier in v2.
3. **Key constraints**: any type for which the runtime's `<` /
   `==` operators are total — `Int`, `Real`, `Char`, `String`
   in the v1 runtime. Records and sum types as keys panic at
   runtime on the first comparison; lifting them to a primitive
   key is the recommended workaround until v2 introduces an
   `Ord` protocol. `Hashable` was not pulled into v1.

What lives where after the lane:
- Carrier + public surface — `stdlib/collections/map.kai`.
- Typer dispatch — `stage2/compiler.kai` `synth_index` /
  `SIndexAssign` arm.
- Sugar specification — `docs/syntax-sugars.md` §4.
- Regression fixtures — `examples/stdlib/map_tree_basic.kai`
  (positive, includes a 1000-element round-trip) and
  `examples/stdlib/map_assign_error.kai` (negative — typer
  must reject `m[k] := v`).
- Insert order is **no longer preserved**. `map_keys`,
  `map_values`, and `map_to_pairs` walk the tree in key order.
  Callers that needed insert order moved to a `[Pair[K, V]]`
  list (or stayed where they were — see the JWT encoder demo
  for an example that adapted to sorted output).

The unblock target was `ahu`'s Registry primitive (named-actor
lookup at production scale). v1 closes that block; v2 follows
when HAMT and a `Hashable` protocol come together.

## 7. Slice syntax `a[i..j]`

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

## 8. `Range[T]` as a first-class iterable

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

## 9. Multi-arg `match` sugar — `match a, b { PatA, PatB -> ... }`

```kai
# Today (synthesises a transient tuple-shaped pattern):
fn count_toques(guess: [Int], target: [Int]) : Int = match (guess, target) {
  ([], _)                  -> 0
  (_, [])                  -> 0
  ([g, ...gs], [t, ...ts]) -> {
    let here = if g == t { 1 } else { 0 }
    here + count_toques(gs, ts)
  }
}

# Proposed (no synthetic tuple, no nested match):
fn count_toques(guess: [Int], target: [Int]) : Int = match guess, target {
  [], _                    -> 0
  _, []                    -> 0
  [g, ...gs], [t, ...ts]   -> {
    let here = if g == t { 1 } else { 0 }
    here + count_toques(gs, ts)
  }
}
```

A **scheduled mini-lane** that surfaced from the m8.5 stress test
(see *Tuples — REJECTED* above). Four `demos/*` files (`forth`,
`toquefama`, `9d9l/huffman`, `9d9l/toquefama`) use the
`match (a, b) { (PatA, PatB) -> ... }` shape today, where the
`(a, b)` is *transient*: it never escapes the match arm and no
signature returns it. With tuples rejected as a type, a parser
sugar that scrutinises N expressions and matches N patterns per arm
gives every demo a clean form without introducing tuples-as-a-type
through the back door.

### Semantics

The desugaring is mechanical: `match e1, ..., eN { p1, ..., pN -> body | ... }`
lowers to a nested `match e1 { p1 -> match e2 { p2 -> ... pN -> body } | ... }`
form. Wildcards in any column behave the same; guards on an arm
attach to the innermost branch. The compiler emits the same
exhaustiveness check it already runs on multi-arity sum types.

The N is constrained at parse time (probably ≤ 4 to keep the
diagnostics readable; revisit if a use case wants 5+). N must equal
across every arm; mismatched arities are a parse-time error.

### What it buys

- **No transient tuple type** to support a transient pattern shape.
  The four affected demos drop their synthesised `(a, b)` and read
  their two scrutinees positionally.
- **Same surface area as the existing `match`** for diagnostics
  and codegen. The desugaring runs in the parser; downstream passes
  see only nested matches.

### What it costs

- **Parser bookkeeping**: a comma-separated scrutinee list before
  `{`, plus a comma-separated pattern list per arm. Ambiguity vs
  function-call args is local to the parser's match-head state.
- **Diagnostics**: a per-column "missing pattern" message is
  doable but more work than today's single-scrutinee form. A v1
  can defer column-aware diagnostics to a follow-up.

### Constraints

1. **Same N across all arms.** Mixed-arity arms are rejected at
   parse time.
2. **No defaults / "rest" columns.** A wildcard `_` covers any
   single column; there is no syntax for "any number of remaining
   columns".
3. **Sugar only.** No new ExprKind, no new type, no Pattern node.
   Desugars to nested matches in the parser.

### Decision posture

**Landed v1** in PR #129 (parser-only sugar, no typer impact, no
new AST nodes). The desugar emits a block of `let __mm_s_i = e_i`
bindings followed by a chain of nested `EMatch` calls; each arm
references the same fall-through subtree at the column-1 wildcard
branch, so an outer-pattern match whose inner pattern fails falls
through to the *next outer arm* rather than aborting. The terminal
"no arm matched" branch is `todo!("non-exhaustive multi-arg match")`
(panic at runtime, any-type at inference, no `/ Fail` row pollution).

Constraints in v1:
- `2 ≤ N ≤ 4` — single-scrutinee form unchanged at N=1; N≥5 is a
  parse-time error (`multi-arg \`match\` supports up to 4 scrutinees`).
- Same N across every arm — mismatched arities are a parse error.
- Wildcards behave per column; no syntax for "any number of
  remaining columns".
- Diagnostics report against the desugared nested form. A
  column-aware "missing pattern in column 2" message is deferred;
  open a follow-up issue if it bites in practice.

The four demos that motivated the lane shed their synthetic tuple /
`Pair[a, b]` workaround:
- `demos/forth/main.kai` — `step(stack, tk)` matches `tk, stack`.
- `demos/toquefama/main.kai` — `count_toques(guess, target)` drops
  the `Pair { fst, snd }` wrapper.
- `demos/9d9l/toquefama/main.kai` — `count_famas` matches
  `guess, target` directly.
- `demos/9d9l/huffman/main.kai` — `decode_step` matches
  `cur, bits`.

**Cost paid**: 4-line parser cap + a `test-match` fixture target
(`examples/match/`). No selfhost regression; demo baseline holds.
**Depends on**: nothing structural.

## 10. Binary pattern matching `<<...>>`

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

## 11. `||` flat-map pipe + `Sequence` protocol

```kai
# map (today, hardcoded for [T]):
[1, 2, 3] | { x -> x * 2 }                    # [2, 4, 6]

# flat-map (proposed):
[1, 2, 3] || { x -> [x, x * 10] }             # [1, 10, 2, 20, 3, 30]

# parser tokens out of CSV-shaped input, one token per line:
read_lines(input) || split_csv | trim
```

A second pipe operator `||` for **flat-map**, paired with a
`Sequence[F[_]]` protocol that defines the dispatch target for both
`|` (map) and `||` (flat-map). The protocol unifies eager
collections (`[T]` today) with lazy / streaming containers (`Stream[T]`
when ahu introduces it) under one uniform surface.

### Surface

| Operator | Today                          | Proposed                              |
|----------|--------------------------------|---------------------------------------|
| `\|>`    | apply-pipe (first-arg threading) | unchanged — still the general pipe  |
| `\|`     | map-pipe, hardcoded for `[T]`  | `Sequence.map` dispatched by the inferred LHS type |
| `\|\|`   | (unused; available — `or` covers boolean OR) | `Sequence.flat_map` dispatched by the inferred LHS type |

`||` is free in kaikai's surface because boolean OR is spelled
`or` (Python-style). No collision with the existing grammar.

### The `Sequence` protocol

```kai
protocol Sequence[F[_]] {
  fn map[A, B](self: F[A], f: A -> B) : F[B]
  fn flat_map[A, B](self: F[A], f: A -> F[B]) : F[B]
}

# v1 stdlib impl:
impl Sequence for [_] {
  fn map(xs, f)      = list.map(xs, f)
  fn flat_map(xs, f) = list.flat_map(xs, f)
}
```

A single protocol carries both methods because every type that
sensibly implements one implements the other in this ecosystem
(`[T]`, future `Stream[T]`, future `Vector[T]`). Splitting `Map`
and `FlatMap` apart pays the cost of two protocols today for a
hypothetical `Validation`-style accumulator that is **not** on
the roadmap. If such a type ever lands, it lives outside
`Sequence` with its own surface — same posture as `Option` /
`Result` keeping `!` instead of joining `Sequence`.

The protocol is **single-dispatch** (`docs/protocols.md`,
m12.8) — `O(1)` impl-table lookup, no HKT propagation, no
constraint resolution. It composes cleanly with kaikai's Tier
1 #3 commitment.

### Why `||` doubles `|`

The visual family reads top-to-bottom: `|>` apply, `|` map (one
level), `||` flat-map (one level deeper, hence "doubled"). The
mental rule is one sentence: *one bar maps, two bars flatten*.

This is **not** the same flow as the rejected `<-` monadic bind
(see *Deliberately not on this list*). `<-` proposed a generic
binding form for `Option` / `Result` / effects with do-notation
semantics; `||` is a binary pipe over containers in the
`Sequence` family, with `Option` / `Result` *explicitly outside*
the protocol. The `!` postfix continues to cover propagation;
`||` covers in-pipe flat-map over sequences. Different problems,
different surface.

### What it buys

- **Fills the gap that `|` leaves**: today, flat-map over a list
  inside a pipeline forces a break in the chain
  (`xs |> list.flat_map { x -> ... }`), which reads worse than
  the `|`-style code around it. `||` keeps the pipeline visually
  uniform.
- **Strategic alignment with ahu**: ahu is the natural home for
  `Stream[T]` (lazy, possibly infinite, with backpressure —
  Elixir GenStage / Flow analogue). The day ahu introduces
  `Stream[T]`, it adds `impl Sequence for Stream[_]` and the
  pipe surface works without changes to the language. No
  retrofit, no breaking change.
- **Names the concept honestly**: `Sequence` is a domain word
  (a type that produces elements one at a time, in order). It
  carries no theoretical baggage (`Functor` / `Monad` / `Bind`),
  no laws to promise, no do-notation pendant. Aligns with
  *approachable core, novel where it pays off* (CLAUDE.md Tier
  2 #5).
- **LLM benefit (Tier 3)**: one protocol with two operators is
  easier for an LLM to use correctly than ad-hoc helpers or
  two hardcoded built-ins. The dispatch rule is local to the
  LHS type and visible in `--holes-json`.

### What it costs

- **Typer dispatch for `|` migrates from hardcoded to protocolar.**
  Today `EMapPipe` is hardcoded to `list.map`; the typer would
  learn to look up `Sequence.map` for the inferred LHS type.
  For `[T]` this is one protocol-table entry — the codegen lowers
  to the same `list.map` call, so runtime cost is unchanged.
  For tagged unions where the tag is unresolved, the typer
  defaults to `[T]` (same fallback rule that the `Map[K, V]` v1
  uses for `e1[e2]` indexing).
- **A v1 with one impl can read as over-engineered.** Mitigation:
  the v1 lane lands `Sequence` *with the migration of `|`*, so
  the protocol carries its weight from day one (two operators,
  one dispatch path) rather than sitting alongside a hardcoded
  `|` until a second impl arrives.
- **Operator ergonomics on shift-less keyboards.** `||` is
  shift-bar twice; some non-US layouts make it awkward. Same
  cost as boolean `||` in C-family languages; not a blocker.
- **Parser**: `||` is two `|` tokens with no whitespace between.
  LL(1)-friendly: the lexer tokenises `||` as a single `BARBAR`
  token; same precedence as `|`, left-associative. Same precedence
  rule as `|`, so chains read left-to-right uniformly.

### Constraints (explicit, to keep the surface tight)

1. **`Option` / `Result` do NOT implement `Sequence`.** They keep
   `!` for propagation. This is a hard rule — it prevents the
   pipe operators from drifting into a generic monadic bind.
2. **The protocol's two methods land together.** A `Sequence`
   impl provides both `map` and `flat_map`; partial impls are
   rejected at resolve time. This avoids a slow drift into two
   separate protocols.
3. **No `pure` / `return` in the protocol.** `Sequence` does not
   provide a way to lift a single value into the container. Each
   container exposes its own constructor (`[x]`, `Stream.of(x)`,
   etc.). This is what keeps the protocol non-monadic.
4. **No do-notation, no `for` comprehension over `Sequence`.**
   The pipe operators are the surface; comprehensions and bind
   blocks would be a third mechanism.
5. **Same-line attachment rule applies.** `xs || { x -> ... }`
   parses as flat-map only when `||` and `{` are on the same
   line, mirroring the existing rule for `|` (see
   `docs/syntax-sugars.md` §1).

### Decision posture

Land as a single lane that bundles:
1. The `Sequence` protocol declaration in stdlib.
2. `impl Sequence for [_]`.
3. Lexer tokenisation of `||` as `BARBAR`.
4. Parser entry for the `||` binop at the same precedence as
   `|`, left-associative.
5. Typer migration of `|` from hardcoded `EMapPipe` to
   `Sequence.map` dispatch (with the unresolved-LHS default to
   `[T]`).
6. Codegen unchanged for `[T]` (lowers to the same `list.map` /
   `list.flat_map` calls).
7. Regression fixtures in `examples/sequence/` (positive: round
   trips over `[T]`; negative: `Option` rejected with a typed
   error pointing at `!` and `opt_and_then`).

The lane is independent of any milestone in flight (m12.6
refinement waves, Anga Roa Perceus). It can land any time after
m12.8 protocols (already closed). The natural window is the
"language-surface consolidation" pass that closes
`design.md`'s open decision.

**Cost**: medium. Parser + lexer (small), typer dispatch
migration (the bulk of the work), one stdlib impl, fixtures.
**Depends on**: nothing structural; protocols (m12.8) already
landed.

**Reference**: Elixir `Stream` + `Enum` (the same operations
work over both, dispatched by protocol; closest analog), F#
`seq` computation expression (same idea, different surface),
Rust `Iterator::flat_map` (single trait, two methods —
structurally the same as `Sequence`).

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
  Note: `||` is *not* unused — it is reserved for the flat-map pipe
  proposal (§11). The point here is only that it does **not** duplicate
  boolean `or`.
- **`<-` as monadic bind / effect shorthand**: `!` (already landed)
  covers the propagation pattern for `Option` / `Result`; kaikai's
  direct-style effects do not need a bind operator. Adding `<-`
  would be a second mechanism for the same flow. The flat-map pipe
  `||` (§11) is **not** the same proposal: `||` is a binary pipe
  over containers in the `Sequence` family (`[T]`, future `Stream[T]`),
  with `Option` / `Result` explicitly outside the protocol. Different
  problems, different surface.
- **`$` low-precedence apply (Haskell-style)**: `|>` and `|` already
  avoid paren pyramids. `$` would be a third way to write the same
  chain.
- **`::` path or type ascription**: `.` already separates module
  paths (`math.vector.dot`) and `:` annotates types. `::` would
  collide with one of them without adding intent.
- **Tuples as a second product form**: see the *Tuples — REJECTED*
  retrospective above. The 2026-04-27 measurement gate failed both
  thresholds (line count and signature length).

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

### For language-surface features (sections 4–11)

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
- **`Map[K, V]` + indexing**: **landed v1** (issue #128,
  2026-05-02) — see §6's retrospective for what shipped and
  what stayed deferred. v2 (HAMT carrier + `Hashable` protocol +
  write-side `m[k] := v` sugar) is still tied to the broader
  collection-design pass alongside `Vector[T]`.
- **Slice syntax `a[i..j]`**: lands *after* `Vector[T]` ships.
  Copying semantics over `Array[T]` is the first-approximation
  implementation; view-based slices with region tracking wait
  for region types to prove themselves elsewhere.
- **`Range[T]` as a first-class iterable**: lands together
  with the collection design pass (`Vector[T]`, `Map[K, V]`).
  Designing iterability across these three types in one go
  avoids retrofitting helpers later.
- **Multi-arg `match` sugar**: the only language-surface item with
  a clear scheduled landing window. Parser-only, ~1 day. Lands
  when no other lane is touching the parser's match-head state.
- **Binary pattern matching `<<...>>`**: a milestone in its own
  right (m13–m14 range). Bring forward only if a fintech-toolkit
  binding (FIX 4.4 likely first) becomes a concrete priority.
- **`||` flat-map pipe + `Sequence` protocol**: lands as a single
  bundled lane (protocol + `[T]` impl + `|` migration to protocol
  dispatch + lexer/parser + fixtures). Independent of milestones in
  flight; depends only on m12.8 protocols (already closed). Natural
  window is the language-surface consolidation pass that closes
  `design.md`'s open decision. Defer if no concrete pipeline pain
  has surfaced — but the strategic case (ahu `Stream[T]` arriving
  without a retrofit) is the main reason to land it before ahu
  needs it.

The goal is to keep the surface small. A handful of orthogonal,
well-integrated extensions is worth more than a pile of clever
features.
