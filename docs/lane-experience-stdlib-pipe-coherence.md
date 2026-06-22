# Lane experience — stdlib pipe-participation coherence

Audit-then-fix lane: make stdlib collection pipe-participation
coherent now that head-owner pipe dispatch (#594) and row-carrying
heads (#773) are shipped. Before this lane `[T]` / `Option` /
`Result` / `Map` / `Stream` rode the `|` / `||` / `|?` pipes but
`Set` / `HashSet` / `Queue` / `Stack` / `HashMap` exported zero
canonical combinators — a silent asymmetry the language's own
"any type can participate" promise contradicted.

## Per-collection decision table

| Collection | Decision | Element | Notes |
|------------|----------|---------|-------|
| `Set`      | **rides** | `a`           | `map` collapses output collisions (a set never holds two equal elements). Order preserved. Pure. |
| `Stack`    | **rides** | `a`           | top → bottom order preserved through every pipe. Pure. |
| `Queue`    | **rides** | `a`           | head → tail (FIFO) order preserved. Pure. |
| `HashSet`  | **rides** | `t`           | mirrors `Set`; collisions collapse. Combinators carry `/ Mutable` (they read the mutable table). |
| `HashMap`  | **rides** | `Pair[k, v]`  | mirrors `Map`; duplicate output keys collapse, last wins. Combinators carry `/ Mutable`. |
| `Map`      | re-confirmed | `Pair[k, v]` | already shipped 3 canonical fns (#613); shape is sound and consistent — left unchanged. |

Every collection now rides the pipes — **no collection opted out**,
so there is no "deliberately doesn't because X" doc note to write.
`Option` / `Result` keep their pre-existing explicit rejection (use
`!` / `and_then`), unchanged by this lane.

### Element-semantics calls (decided in-lane, documented in `#[doc]`)

- **`Set.map` / `HashSet.map` collapse duplicates.** Re-inserting the
  mapped results through the set's own `insert`/`add` is the
  least-surprising element-shaped meaning — a set cannot hold equal
  elements, so `{1, -1} | abs == {1}`. Documented as a shrink.
- **`HashMap.map` is pair-shaped, not value-shaped.** Matches `Map`
  exactly (`(Pair[k, v]) -> Pair[k, w]`, dup keys → last wins). A
  value-only `map` would have been an incoherence with `Map`, which
  was the whole point of the lane. `transform_values` already covers
  the value-only case for `Map`.
- **`Stack`/`Queue` preserve their structural order** through the
  pipes (top→bottom, head→tail). `from_list` rebuilds in that order.

## Design rationale — mutable collections DO ride

The one genuine design call was whether the mutable `HashSet` /
`HashMap` should ride or carry a "does not ride" doc note. They ride.

- The incoherence the lane exists to kill is precisely "`Map` rides
  but `HashMap` doesn't" and "`Set` would but `HashSet` doesn't". A
  pure collection that rides and its mutable sibling that doesn't is
  itself an asymmetry a user hits.
- The mutable combinators carry `/ Mutable` honestly (they read the
  table). The matcher arms the expected pipe row with an **open
  fresh-tail row var**, so the concrete `Mutable` label is absorbed
  by Rémy-style tail unification — `/ Mutable + e` dispatches exactly
  as `/ e` does. Verified with a fixture, not assumed.
- HashMap's `/ Mutable` reaches the call site regardless of pipes:
  any use of `hashmap.from_pairs` already forces `/ Mutable` on the
  caller's row (its public signature declares it; the local-Array
  masking pass does not strip an explicitly-declared effect). So the
  pipes add no new effect surprise — the container was always mutable
  and always observable. `asu`'s "masking makes main stay pure"
  expectation holds for locally-constructed *Arrays*, not for a
  HashMap whose public API declares `/ Mutable` deliberately.

## Structural surprises the brief did not anticipate

1. **`list.flat_map` has NO effect row var.** Its signature is
   `(xs: [a], f: (a) -> [b]) : [b]` — pure `f` only. Delegating a
   collection's `flat_map` to it would silently drop `f`'s effect
   row and break the canonical `flat_map` shape. Every collection's
   `flat_map` is therefore written as its own row-threading loop
   (`set_flat_map_loop`, `stack_flat_map_loop`, …) carrying `/ e`.
   `list.map` / `list.filter` DO carry `e`, so `map`/`filter` can
   delegate. (Fixing `list.flat_map` itself is out of lane — it
   would shift `--emit=kir` goldens and is core, not collections.)

2. **Non-List head dispatch is edition-gated.** `build_head_owner_map`
   only walks user/imported `pub type` heads when edition ≥
   `hanga-roa` (#603). A bare `.kai` compiled without `--edition`
   defaults to tongariki, where the matcher rejects every non-List
   head ("no module declaring type `T` is in scope"). The fixtures
   therefore compile with `--edition hanga-roa`. This was the
   surprise that made the first Set spike fail despite a correct
   signature — the gate, not the signature.

3. **The pre-existing `Map` pipe fixture was orphaned.**
   `examples/stdlib/map_pipes/` (a `kai.toml` hanga-roa package) is
   referenced by NO Makefile target and NO CI tier — its own header
   admits "not wired into test-stdlib … smoke-testable via
   `bin/kai run`". So `Map`'s pipe surface had zero automated
   coverage. The new `test-pipes-collections` tier closes that gap
   with a wired `map_pipes.kai` alongside the five new collections.
   The orphaned package was left in place (harmless; removing it is
   churn outside the coherence goal) — a follow-up could delete it.

## Fixtures added

`examples/pipes/collections/{set,stack,queue,hashset,hashmap,map}_pipes.kai`
+ `.out.expected` goldens, wired into the new `test-pipes-collections`
Makefile target (compiled `--path ../stdlib --edition hanga-roa`,
build artifacts namespaced `pc-` so `make -j` does not race). The
target joins `TEST_LIGHT_TARGETS`, `test-fast`, and the `.PHONY`
list, so it runs under `make test` → `tier1`.

HashSet/HashMap fixtures assert size + membership (bucket order is
unspecified); the pure collections assert ordered list output. Each
fixture exercises `|?` filter, `|` map, and `||` flat-map.

## Coverage gaps / follow-ups

- `list.flat_map` lacks an effect row var (issue-worthy: a `||` over
  `[T]` with an effectful `f` would not type-check; the negative is
  not currently exercised). Out of this lane (core + goldens).
- `examples/stdlib/map_pipes/` orphaned package could be deleted.
- A negative fixture pinning "tongariki rejects `Set | f`" was not
  added — the edition gate is covered by `examples/editions/*` and
  the existing `pipe_no_module` negative; adding a collection-
  specific one is optional.

## Gates

selfhost byte-id ✓ (additive stdlib, no compiler change) · tier0 ✓ ·
`test-pipes` + `test-negative` (incl. `pipe_wrong_signature`,
Option/Result rejections) stay green ✓ · `km score` A++/A on all
edited files, cogcom avg ≤1.2 max ≤3, zero new dup groups ✓.
