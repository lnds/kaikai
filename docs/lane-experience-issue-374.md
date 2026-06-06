# Lane experience — issue #374 (`stdlib/collections/hashmap`: HashMap[k, v])

## Scope as planned vs scope as shipped — a carrier redesign mid-lane

The issue body pinned a **pure persistent HAMT** (Hash Array Mapped
Trie) and 12 flat `hashmap_*` functions with pure signatures
(`put : HashMap`). I built exactly that first. Then the benchmark
showed it was *slower than the AVL `Map` it was meant to replace*, the
maintainer's call was to make HashMap a **mutable hash table behind the
`Mutable` effect**, and the carrier was rewritten. What shipped:

- **Mutable hash table** in `stdlib/collections/hashmap.kai` —
  separate chaining, `HashMap[k, v] = { buckets: Ref[Array[[Pair]]],
  count: Ref[Int], cap: Ref[Int] }`. Bucket array + count + capacity
  in `Ref` cells so a resize swaps the whole array atomically inside a
  `/ Mutable` body. Capacity doubles at load factor 0.75.
- **12 pub fns, the issue's names** — but mutating signatures:
  `put`/`remove` mutate in place and return `Unit`; every op carries
  `/ Mutable` (the read ops dereference the `Ref` cells).
- **Fixtures** `examples/stdlib/hashmap_basic.kai` (full API, 10K +
  100K round-trip across several resizes, iteration-order determinism,
  in-place mutation) and `hashmap_collision.kai` (forced one-bucket
  chain via a constant-hash sum-type key). Golden-gated by
  `test-stdlib`.
- **Benchmark** `benchmarks/hashmap/` (Clock-timed program + README).
- **Docs** `stdlib-layout.md` + `stdlib-roadmap.md`.
- **`m[key]` indexing sugar** — one additive arm in the typer's
  `synth_index` (`stage2/compiler/infer.kai`) so `m[key]` lowers to
  `hashmap.hashmap_get(m, key)` when `m : HashMap`, exactly as `Map`
  lowers to `map.get`. The `/ Mutable` effect propagates from the
  lowered callee automatically (the sugar adds no row of its own).

Almost a pure stdlib lane: the one compiler touch is the additive
`synth_index` arm above (see Finding 1) — added on the maintainer's
explicit request for usability. It does not touch the parallel #747
codegen lane (`emit_llvm.kai`), and selfhost stays byte-identical.

## The pivot: why HAMT lost and mutable won

The HAMT was correct and the fixtures passed, but the benchmark was the
deliverable that mattered and it was damning. Measured at N=10K, 1M
lookups:

- pure HAMT: build ~8.9ms / lookup ~142ms
- AVL `Map`:  build ~5.1ms / lookup ~95ms

The HAMT was **~1.7× slower on build and ~1.5× slower on lookup**, and
the gap *widened* at 100K. A HashMap slower than the tree it replaces
has no reason to exist.

Diagnosis (measured, not assumed — the first README's "it's the lists
in `put`" was an incomplete guess): I isolated **pure lookup** (no
build) and the HAMT was still 1.5× behind. Lookups do not rebuild
lists, so the cost had to be in the read path — and it was:
`HBranch(Int, [HamtNode])` stored children in a **linked list**, so
`list.nth(kids, i)` is O(i) per level. With branching factor 32 and a
densely-filled root, every lookup walked up to 32 cons-cells per level.
Lookup was O(depth × popcount), not O(depth). A branch-fill probe
confirmed the root and upper levels were near-full.

Two ways out: (a) `Array[HamtNode]` children for O(1) slot access but
still copy-on-write per `put`; (b) a genuinely mutable table. The
maintainer chose (b) — *"la idea de hash_map es implementar hash
mutables, es un effect"*. A mutable-chaining prototype with resize
settled it:

- mutable HashMap: build ~2ms / lookup ~50ms
- AVL `Map`:        build ~5ms / lookup ~100ms

**~2-2.5× faster than `Map` on both.** That is what shipped. Final
numbers in `benchmarks/hashmap/README.md`: ~3× build and ~3× lookup at
N=100K, ~1.9× on a 1M-lookup hammer.

The lesson: the issue body's data-structure choice (pure HAMT, "the
right pick for kaikai's design") was wrong for kaikai's *current*
runtime, where a persistent trie's allocation + RC churn buries the
asymptotic win. The benchmark caught it; without it I would have
shipped a slower default collection with a confident retro.

## Design decisions (mutable carrier)

- **Refs for everything that resizes.** `buckets`/`count`/`cap` are all
  `Ref` cells, not record fields, because a record is immutable —
  `Mutable.ref_set(m.cap, …)` is the only way to mutate "the table"
  through a handle passed across function boundaries. `Ref` + `Array`
  mutation crosses fn boundaries cleanly (it is NOT masked the way a
  function-local Array is — masking only hides arrays built *and*
  filled in one body, per #251/#252).
- **Separate chaining over open addressing.** Chains are simple
  `[Pair[k,v]]`, pure to splice, and degrade gracefully under a bad
  hash (the collision fixture drives every key into one chain and still
  passes). Open addressing would be faster on cache but needs tombstone
  bookkeeping on remove; chaining is the right v1 simplicity/perf point.
- **Resize policy.** Double when `count*4 > cap*3` (load factor 0.75),
  rehash all chains into a fresh `Mutable.array_make(cap*2)`. Amortises
  to O(1) per insert.
- **Negative hashes.** `((h % cap) + cap) % cap` — `hash` may wrap
  negative per the `impl Hash` contract.
- **Size accounting.** Chain `put`/`remove` return `(chain, changed?)`
  so `count` stays exact in one walk without a separate membership
  probe.

## Findings surfaced (not fixed — out of the stdlib lane's scope)

### Finding 1 — `m[key]` sugar (deferred, then SHIPPED on request)
Initially deferred: the dispatch lives in the typer (`synth_index` in
`stage2/compiler/infer.kai`, the closed `name == "Map"` arm) and the
brief said to surface typer changes as findings, not make them. The
maintainer then asked for the usability directly ("¿no se puede usar
`[]`?"), so the arm was added: `m[key]` lowers to
`hashmap.hashmap_get(m, key)` when `m : HashMap`. Three things made it
low-risk: (1) it is a single additive arm that only fires for the
`HashMap` head — existing `Map`/`Array` indexing is untouched; (2) the
`/ Mutable` effect propagates for free because `synth_index` re-walks
the constructed `ECall`, so the lowered `hashmap_get`'s row flows into
the call site — no special-casing the effect; (3) **selfhost stays
byte-identical** (verified `kaic2b.c == kaic2c.c`), because the arm
changes emission only for programs that actually index a `HashMap`,
and the compiler does not. The design note above `synth_index` (which
predicted "revisit only if a third indexable container appears") was
updated from a 2-case to a 3-case dispatch — HashMap is that third
container, and the closed dispatch is still the right shape (no `Index`
protocol). The write-side `m[key] := v` is NOT provided.

### Finding 2 — `[k: Hashable]` is not expressible; `Hash` is the protocol
#373 shipped `Hash` (method `hash`), not "Hashable". kaikai has no
protocol bounds on free-function type params (Tier 1 #3) — `[k: Hash]`
is a parse error. The requirement is a runtime contract (panic on a key
type without `impl Hash`), same as the AVL `Map`'s `<`.

### Finding 3 — record keys do NOT dispatch; sum types DO
A generic `hash(key)` inside the module dispatches via `kai_head_tag` at
runtime. Primitives and **user sum types** (with `impl Hash` + `impl
Eq`) work — verified; the collision fixture relies on sum-type keys.
**User records** do not: boxed through `key: k` they read head tag 0 and
`hash` panics, even with `#[derive(Hash)]`. Direct `hash(record)` works;
only the generic-collection boundary loses the tag. Compiler-side
monomorphisation/dispatch gap (typer/runtime), same class as the AVL
`Map`'s `<` on non-primitives. Follow-up worth its own issue (needs
authorisation to open).

### Finding 4 — `kai bench` segfaults on any `/ Mutable` block body
Discovered when wiring the benchmark: `bench "x" { Mutable.ref_make(0) }`
— a trivial mutable body — segfaults the bench runner. Since HashMap is
mutable, *no* HashMap operation can be benchmarked through `kai bench`.
Pre-existing bench/effect-runtime bug, unrelated to this carrier. The
benchmark works around it by timing with `Clock.monotonic_now()`
(`benchmarks/hashmap/hashmap_vs_map.kai` is a `main`, not a `bench`
fixture). Worth its own issue.

## Structural surprise — in-file `test` blocks calling privates break import

The HAMT cut put white-box `test` blocks inside hashmap.kai (like
`core/list.kai`) to reach private collision helpers. That broke **every**
`import collections.hashmap`: the privacy validator runs on `test`-block
bodies *before* the #318 "drop prelude tests on import" pass, so a
`test` block calling a private fn trips a privacy error on plain import —
failing `test-stdlib-modules` (tier1). Minimal repro confirmed: pub-only
`test` block imports fine, one private call breaks it. Resolution: no
in-file test blocks; collision coverage is black-box in
`hashmap_collision.kai` (which the mutable carrier kept). The
privacy-vs-test-drop ordering is itself a latent compiler quirk.

## Fixtures + coverage

- `hashmap_basic.kai` (38 asserts): empty, single insert, batch
  get/contains, in-place update (size stable), remove (incl. missing),
  keys/values (sorted view), to_pairs/from_pairs (duplicate collapse),
  merge (left-biased, mutates `a`, leaves `b`), 10K + 100K round-trip
  (multiple resizes), re-put keeps size, iteration-order determinism
  across same-input builds.
- `hashmap_collision.kai` (21 asserts): one-bucket chain via a
  constant-hash sum-type key — insert/get/overwrite/remove/drain/
  re-insert.

Both golden-gated by `make -C stage2 test-stdlib` and import-checked by
`tools/test-stdlib-modules.sh`. No Makefile edit needed.

`hashmap_basic.kai` also covers the `m[key]` indexing sugar (4 asserts:
present/absent keys, let-RHS position) now that it ships.

**Coverage gaps (deliberate):** record keys (don't work, Finding 3);
full primitive-key hash collision (unreachable by hand — sum-type
constant-hash is the deterministic substitute); `kai bench` HashMap
timing (segfaults, Finding 4 — Clock workaround used).

## Cost vs estimate

Issue budgeted 5-7 days. The real cost was two builds, not one: the
HAMT (carrier ~half a day, then a multi-round dispatch-boundary
investigation), the benchmark that condemned it, and the mutable
rewrite (carrier + fixtures + benchmark + docs, ~half a day once the
model was clear). The mutable prototype settled the design in one
measurement. Net effort comparable to estimate; the wasted HAMT work
was the price of not benchmarking the design before committing to it.

Also re-tripped the #373 retro's footgun: the first `Write` of
hashmap.kai landed in the **main checkout** instead of the worktree
(absolute path resolved to the non-worktree `stdlib/`). Caught
immediately (worktree `kaic2` could not find the module); moved it,
verified main clean.

## Follow-ups left for next lanes

1. **#375 (HashSet)** — now unblocked. Build `HashSet[T]` on
   `HashMap[T, Unit]`; it inherits the mutable + `/ Mutable` model.
2. **Record keys through generic protocol dispatch** — head-tag-0 gap
   (Finding 3); fixes HashMap- and Map-over-records.
3. **`kai bench` + `Mutable` segfault** (Finding 4) — bench/effect
   runtime bug; blocks benchmarking any mutable structure.

(The `m[key]` sugar from Finding 1 shipped in this lane — no longer a
follow-up.)

## Gate

- `make tier0`: green — selfhost byte-identical (`kaic2b.c == kaic2c.c`),
  demos baseline, arena. Additive lane, determinism by construction.
- `make -C stage2 test-stdlib`: green incl. both hashmap fixtures.
- `tools/test-stdlib-modules.sh`: hashmap.kai imports clean.
- tier1 + tier1-ASAN: to CI (the `Ref`/`Array` mutation path is exactly
  what ASAN should scrutinise — flag if it surfaces a UAF on resize).
