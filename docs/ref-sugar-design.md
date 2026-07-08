# `Ref[T]` surface sugar — `&` make, `:=` set, `@` deref (design proposal)

**Status:** proposal, not accepted. Consolidates a design conversation. Motivated by
#1113 (`:=` rejects a `Ref` record field) and by `Ref[T]` surfacing in everyday user
code (graph nodes with rewritable edges — `kaikai-vs-rust` cases 1 & 6), which
refutes the original rationale for leaving `Ref` verbose.

## The problem

`Ref[T]` — the transportable mutable cell (unlike a local `var`, a `Ref` is a
first-class value you can put in a record, pass, and return) — has no surface sugar.
Every use spells the effect and the op by hand:

```kaikai
let r = Mutable.ref_make(0)         # create
Mutable.ref_set(r, 99)              # write
let v = Mutable.ref_get(r)          # read
```

Three problems:

1. **`Mutable.` leaks an implementation detail.** A user who wants "a mutable cell"
   must name the effect that governs it. The effect belongs in the inferred row (Tier
   1 #1: effects visible) — but the user should not have to *write* it. `var x := 0`
   already hides `State` this way; `Ref` does not get the same courtesy.
2. **`ref_make`/`ref_set`/`ref_get` are verbose** where neighbouring languages have
   operators.
3. **Inconsistent with `var`.** `var x := 0` / `x := v` / bare `x` is the clean local
   surface (`docs/effects-stdlib.md` §Mutable: "`:=` is the single mark of
   mutability"). A `Ref` deserves an equally clean surface.

### Why the original "keep it verbose" rationale no longer holds

`docs/effects-stdlib.md:287` deliberately withheld sugar from `Ref[T]`:

> The same sugar is **not** extended to `Ref[T]` … Rationale: `Array[T]` shows up in
> everyday user code, `Ref[T]` is a low-level tool used mostly by the stdlib and FFI
> adapters where explicit ops double as documentation.

That premise is refuted by practice. A graph node `{ id: Int, next: Ref[Slot] }` is
user modelling, not stdlib plumbing (#1113 came from a two-node cyclic graph). Users
model mutable-pointer-in-a-record with `Ref`, and are forced into `Mutable.ref_set(...)`
or, worse, a one-cell `Array[T]` purely to reach the `[0]`-indexed form `:=` accepts.
Both are workarounds for a missing sugar, not a modelling choice.

## The proposed surface — `&` / `:=` / `@`

```kaikai
let r = &0        # ref_make  —  `&` takes a reference (the sigil is free today)
r := 99           # ref_set   —  reuses the existing `:=` (also closes #1113)
@r                # ref_get   —  `@` dereferences (freed by the `as` migration below)
```

Desugar (the effect stays in the inferred row, never written by the user):

| Surface | Desugars to |
|---|---|
| `&x` | `Mutable.ref_make(x)` |
| `r := v` | `Mutable.ref_set(r, v)` |
| `@r` | `Mutable.ref_get(r)` |

`&` and `@` are the take-reference / dereference pair every systems programmer reads
without a glossary (C, C++, Go). `Mutable` disappears from the surface entirely; it
remains in the row type inferred for any function that touches a ref, so Tier 1 #1
(effects visible in the type) is preserved — only the *spelling burden* is removed.

### Sigil availability (verified)

- **`&`** is free — not a lexer token, no bit-and-as-sigil (bitwise ops live in
  `stdlib/math/bits.kai` via UFCS, not as a symbol). Available for `ref_make`.
- **`@`** is currently `TkAt`, used for **as-binding patterns** (`whole @ [_, ...]`,
  `parse.kai:1235`). To use `@` for deref, the as-binding must move to the `as`
  keyword (below). This is the load-bearing dependency of the elegant trio.
- **`!`** (`TkBang`) and **`^`** (`TkCaret`, power / UoM) are taken — not candidates
  for deref.

## Freeing `@`: migrate as-binding to the `as` keyword

Today `@` binds a name to a whole pattern:

```kaikai
match xs {
  whole @ [first, ...rest] -> use(whole, first, rest)
}
```

kaikai already has the `as` keyword (`TkAs`, used in `import X as Y` and
`with Eff as a`; `parse.kai:1993` already parses `as`-aliases). The as-binding reads
*more* naturally with `as` than with `@` — "bind `whole` **as** the whole pattern":

```kaikai
match xs {
  whole as [first, ...rest] -> use(whole, first, rest)
}
```

`@` was an ML/Rust inheritance kaikai has no reason to carry. Migrating it frees `@`
for its more natural pointer-deref meaning.

### Migration cost (measured)

`@` as-binding appears in **~5 user-visible sites** — all the project's own fixtures
(`examples/sugars/m7d_14_at_pattern_*`, `examples/library_mode/def_at_pattern_as`) plus
a few compiler-internal pattern lowerings (`kir_lower_variant/walk/bind.kai`). There is
**no external user base** — the project has one user — so the stability constraint
(Tier 2 #4, no breaking user code within an edition) is not a real cost here: the only
code to migrate is this repo's, via `kai migrate`.

Clean path: accept **both** `@` and `as` in pattern position for one step (a
soft-deprecation), migrate the repo's sites to `as`, then drop `@`-as-binding. Since
the user base is one, a direct migration (`kai migrate` rewrites `@`→`as` in patterns,
then `@` becomes deref-only) is also acceptable.

## Coexistence with `var` / local cells — not a competing model

kaikai already has a mutability surface for **local cells** (`State`):

| | Local cell (`var`) | Transportable `Ref` |
|---|---|---|
| create | `var x := 0` | `&0` |
| write | `x := v` | `r := v` |
| read | `x` (bare) | `@r` |

The **write is shared** (`:=`) — consistent, one mark of mutability. The **read
differs by design**: a `var` *is* the mutable variable (you read the variable, bare);
a `Ref` is a first-class value that *holds* a cell (you read *through* it, hence `@`).
The `bare` vs `@` distinction reflects a real difference — a cell is a variable, a ref
is a value pointing at one — not an inconsistency. Two concepts, two reads, one shared
write.

## Closing #1113 falls out of this

#1113 (`:=` rejects `n.field := v` where `field : Ref[T]`) is subsumed: extending the
`:=` LHS grammar (`parse.kai:3256`) so a `Ref`-typed `record.field` desugars to
`Mutable.ref_set(record.field, v)` is exactly the `r := v` rule applied to a field
target. The bare-`Ref` `:=` case and the `Ref`-field case are the same sugar; shipping
`&`/`:=`/`@` closes #1113 as a subset.

## Scope / staging (if implemented)

Three separable pieces, in dependency order:

1. **`:=` on a `Ref` field** (closes #1113 alone) — the smallest cut: extend the `:=`
   LHS grammar for a `Ref`-typed field. Additive, no `@` migration needed. Ships the
   reported bug fix without the full trio.
2. **`&` make + `:=` set** — add the `&x` prefix form desugaring to `ref_make`, and the
   bare-`Ref` `:=` desugar (a superset of piece 1). Still no `@` migration.
3. **`@` deref + as-binding migration** — migrate `@`-as-binding to `as`, free `@`,
   add `@r` deref. This is the piece that touches existing syntax (the migration) and
   completes the symmetric `&`/`@` pair.

Piece 3 is the only one that changes existing syntax; pieces 1–2 are purely additive.
The trio is most coherent shipped whole, but 1 alone is a valid minimal cut for #1113.

## Not decided

- Whether `&`/`@` should also apply to `Array` cells or stay `Ref`-only (Arrays have
  `a[i]` / `a[i] := v` already; adding `&`/`@` there would be redundant — likely
  `Ref`-only).
- Whether a bare `Ref` read (no `@`) should ever be allowed by inference in
  unambiguous contexts, or whether `@` is always required (leaning: always `@`, so the
  deref is explicit and never confused with a plain value).
- Interaction with the native RC bugs #1110/#1112: `Ref` desugaring must not
  reintroduce the discarded-owned-value leak (#1112) — a `&x` whose ref is dropped
  must decref through the same path a `var` cell does.

## References

- #1113 — `:=` does not accept a `Ref`-typed record field.
- `docs/effects-stdlib.md` §`Mutable` — the ref API + the original "keep it verbose"
  rationale this proposal revises.
- `docs/syntax-sugars.md` — the `var x := 0` / `x := v` / naked-read cell sugar this
  mirrors.
