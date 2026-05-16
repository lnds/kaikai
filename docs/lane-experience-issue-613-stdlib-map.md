# Lane experience — issue #613 (`stdlib/collections/map`: round-out + qualified migration + #594 pipe compatibility)

## Scope as planned vs scope as shipped

Three coordinated changes to `stdlib/collections/map.kai`:

- **Part A — round-out.** Five new pub fns: `update`, `fold`,
  `merge`, `filter`, `transform_values` (was `map_values` in the
  issue body; renamed during the lane — see "Naming decision"
  below).
- **Part B — qualified migration.** Rename every existing
  `map_*` to its qualified canonical form; retain every flat
  name as a one-liner alias for the tongariki edition.
- **Part C — pipe convention.** Export `map`, `flat_map`,
  `filter` with `Pair[k, v]` element shape so `Map[k, v]`
  participates in `|`, `||`, `|?` post-#594.

Everything in scope shipped. No compiler edits, no runtime
edits, no `bin/kai` edits. The lane stayed inside the single
file `stdlib/collections/map.kai` plus three example fixtures
and the two catalog docs.

## Naming decision: `transform_values` over `map_values`

The issue body's Part A.5 named the new value-transforming
operation `map_values(m, f) : Map[k, w]`. That name collides
with the **renamed legacy** `map_values(m) : [v]` (Part B
flips `pub fn map_values(m)` to `pub fn values(m)` and keeps
`map_values(m)` as a one-liner alias to `values(m)`). The
collision is at the flat-prefix layer; on the qualified layer
the two would be `map.values(m)` (0-arg, returns `[v]`) and
`map.map_values(m, f)` (1-arg, transforms values) — the second
reads as repeated and badly so.

Three alternatives considered:

1. `map.map_values(m, f)` — repeated stem, ugly.
2. `map.with_values(m, f)` — reads OK, but obscures that this
   is a transform, not a setter.
3. **`map.transform_values(m, f)` — chosen.** Describes the
   operation exactly. Pairs with `map.map(m, f)` (pair-shaped
   pipe-canonical) and `map.values(m)` (0-arg projection)
   without lexical clash.

`map.map(m, f)` is reserved for the pipe-convention pair-shaped
transform (Part C). Values-only transforms route through
`transform_values`.

## AVL fold pattern reused for five new ops

`tree_fold` is a single new tree-walker:

```kai
fn tree_fold(t: Tree[k, v], acc: a, f: (a, Pair[k, v]) -> a / e) : a / e
```

Every new op is one line of structural code over `tree_fold` or
`get`/`put`:

| Op                 | Walker         | Worker step                            |
|--------------------|----------------|----------------------------------------|
| `update`           | `get` + `put`  | replace with `f(old)` or insert default |
| `fold`             | `tree_fold`    | `f(acc, pair)`                         |
| `merge`            | `fold(m2, m1)` | `if contains(acc, k) acc else put`     |
| `filter`           | `fold(m, ∅)`   | `if p(pair) put(acc, …) else acc`      |
| `transform_values` | `fold(m, ∅)`   | `put(acc, k, f(v))`                    |
| `map` (pipe)       | `fold(m, ∅)`   | `put(acc, f(pair).fst, f(pair).snd)`   |
| `flat_map` (pipe)  | `fold(m, ∅)`   | `merge(acc, f(pair))`                  |

The `merge`-vs-`fold` collapse is the load-bearing
simplification: once `tree_fold` exists, the pair-shaped pipe
exports (Part C) are two-liner derived combinators. The lane
shipped at ~70 LOC of net new code instead of the ~150 the issue
estimated — the simplification absorbed most of it.

`merge` is left-biased (`m1` keys win) by folding `m2` into `m1`
with a `contains`-gated `put`. The cost is `O(n2 · log n1)` —
one tree lookup plus one tree insert per pair of `m2`.

## Pipe convention dispatch works out-of-the-box

#594's `build_head_owner_map` scans every `pub type T` in the
imported-module decl stream and records the owning module. The
moment `stdlib/collections/map.kai` exports `pub fn map`,
`pub fn flat_map`, `pub fn filter` alongside the existing
`pub type Map[k, v]`, the dispatcher routes `m | f` to
`map.map(m, f)` for `m : Map[k, v]` LHS. The fixture
`examples/stdlib/map_pipes.kai` exercises `|`, `||`, `|?` in
that order against `Map[String, Int]` and succeeds without any
typer or dispatcher changes.

This was the load-bearing acceptance the lane started worried
about. It cost zero LOC of dispatcher work; the entire effect of
#594 was to make the new exports addressable through the
existing pipe syntax.

## Deprecation alias mechanics

Eleven flat-prefix one-liners at the bottom of the file:

```kai
pub fn map_put(m: Map[k, v], key: k, value: v) : Map[k, v] = put(m, key, value)
```

The aliases preserve:

- The textual `"map_get"` callee name synthesised by the
  `synth_index` lowering at `stage2/compiler.kai:32111` for the
  `m[key]` indexing sugar. The lane confirmed via `grep` that
  this is the only stage 2 reference to a `stdlib/collections/map`
  symbol (the other `map_*` matches in stage 2 are unrelated
  internal helpers — `alias_map_empty`, `subst_map_empty`).
  Stage 1 has zero references.
- Every existing user-side call site in `examples/aspirational/*`,
  `examples/stdlib/jwt_encoder.kai`, and `stdlib/collections/queue.kai`
  (the latter only mentions `map_put` in a comment; no actual
  call).

Selfhost is byte-identical post-rename: tier 0 reports
"selfhost byte-identical" on every invocation through the lane.

The aliases are scheduled to drop at the Orongo edition boundary
per `docs/editions.md` stability discipline. Until then, they are
**not** marked `#unstable` — they are part of the stable surface
under tongariki + hanga-roa, just labeled "legacy."

## Fixture decisions

Three fixtures:

- `examples/stdlib/map_basic.kai` — migrated in place to
  qualified style (`map.put`, `map.get`, …). Same `out.expected`,
  byte-identical output. Confirms the qualified path lowers to
  the same prelude calls as the legacy aliases.
- `examples/stdlib/map_round_out.kai` — new. Exercises the five
  round-out ops with explicit value asserts plus a final
  pair-printout for ordering verification.
- `examples/stdlib/map_pipes/` — new package. Exercises the
  three pipe operators (`|`, `|?`, `||`) against `Map[String, Int]`.
  Pinned to `edition = "hanga-roa"` in `kai.toml` because the
  convention-based pipe dispatch from #594 is gated behind
  `hanga-roa` for non-List heads (per #603). Sits in a
  sub-directory rather than a flat `.kai` because the
  `test-stdlib` glob runs every top-level `examples/stdlib/*.kai`
  under the repo's default `tongariki` edition; smoke-testable
  manually with `bin/kai run examples/stdlib/map_pipes/main.kai`,
  matching the precedent set by `examples/editions/*` (also not
  wired into stage 2 CI per the #603 retro).

A potential extra fixture (`map_aliases.kai`, smoke-testing
every flat-prefix alias) was considered and skipped: the legacy
aliases are already exercised end-to-end by the unmodified
`examples/stdlib/jwt_encoder.kai`, `examples/stdlib/map_tree_basic.kai`,
and the aspirational consumers (`event_ledger`, `calculator`).
Adding a dedicated alias smoke test would duplicate that
coverage.

One small fold-API quirk surfaced during fixture writing:
when the accumulator type is `String` and the body is
`acc ++ p.fst`, the typer cannot bridge the field-access
type through the lambda even with `acc: String` annotated.
The fixture works around this by accumulating into `[String]`
instead. Tracked here as a follow-up sharpener for the
type-narrowing pass, not load-bearing for this lane.

## Collateral fix — `Tree[k, v]` no longer leaks to prelude (closes #625)

The lane originally landed `pub type Tree[k, v]` alongside
`pub type Map[k, v]`. CI surfaced that `Tree` was leaking into
the global prelude namespace, colliding with any user-defined
`type Tree` (e.g. the canonical chapter-5 example from the book).
The fix is a one-liner: drop `pub` from `Tree[k, v]` — the AVL
carrier is an implementation detail of `Map`, not API. `Map[k, v]`
stays `pub`. Closes #625 collaterally.

## Real cost vs estimate

Issue body estimated 0.4 day-agent. Actual was closer to 0.2:

- File rewrite (~310 → ~370 LOC, mostly comments) — single
  Write pass.
- Three fixtures (~30 LOC each).
- Two doc updates (catalog row + roadmap row).
- Tier 0 + tier 1 + lane retro.

The merge-vs-fold simplification absorbed the bulk of the
estimated LOC; the pipe convention dispatch came for free
post-#594.

## Follow-ups left for next lanes

- **Orongo edition cut** — drop the 11 flat-prefix aliases.
  No tracking issue yet; the right moment is the edition bump
  itself.
- **Hashable / Ord protocols (M2 carrier work)** — `merge` and
  `transform_values` currently compose through structural `==`
  and `<`. Once a real `Ord` protocol lands, the AVL invariant
  on record/sum keys (which today panics at the first
  comparison) becomes typed-checkable. Tracked by the v2 HAMT
  follow-up under #128, not by this lane.
- **Typer narrowing for `String ++ field_access`** — the fold
  fixture quirk noted above. Worth opening a small issue for.
- **`map.contains` shadowing** — both `core/list.contains` and
  `core/string.contains` already exist; `map.contains` adds a
  third. The qualified call (`map.contains`) is unambiguous,
  but bare `contains` resolution depends on the first-arg-type
  narrowing pass (per #235). The lane did not surface a
  collision in any fixture; if one shows up later, the
  resolver path is the right place to fix it, not the stdlib
  rename.
