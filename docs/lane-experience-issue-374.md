# Lane experience — issue #374 (`stdlib/collections/hashmap`: HAMT-backed HashMap[k, v])

## Scope as planned vs scope as shipped

The issue pinned a HAMT carrier and 12 flat `hashmap_*` public
functions in a single new stdlib file. Shipped:

- **HAMT carrier** in `stdlib/collections/hashmap.kai` — the node
  sketch from the issue verbatim (`HEmpty` / `HLeaf(Int,k,v)` /
  `HCollision(Int,[Pair])` / `HBranch(Int,[HamtNode])`),
  `HashMap[k, v] = { priv root, priv size }`. Branching factor 32
  (5 bits/level via `bit_ushr`+`bit_and 31`), bitmap-compressed
  children (`HBranch` allocates exactly `popcount(bitmap)` slots,
  compressed index = `popcount(bitmap & (bit-1))`).
- **All 12 pub fns, exactly as named**: `hashmap_empty`,
  `hashmap_size`, `hashmap_is_empty`, `hashmap_get`,
  `hashmap_contains`, `hashmap_put`, `hashmap_remove`,
  `hashmap_keys`, `hashmap_values`, `hashmap_to_pairs`,
  `hashmap_from_pairs`, `hashmap_merge`.
- **Fixtures**: `examples/stdlib/hashmap_basic.kai` (primitive-key
  API, 10K + 100K round-trip, iteration-order stability) and
  `examples/stdlib/hashmap_collision.kai` (forced `HCollision`).
  Both golden-gated by `make -C stage2 test-stdlib` (tier1).
- **Benchmark**: `benchmarks/hashmap/` (bench fixture + README with
  a sample run and the crossover analysis).
- **Docs**: `docs/stdlib-layout.md` + `docs/stdlib-roadmap.md`
  updated per the doc-discipline section of CLAUDE.md.

Pure stdlib lane: **no** edits to stage1, the typer/resolver, the
LLVM/C codegen, or `bin/kai`. The whole carrier rides existing
intrinsics (`math/bits`) and the existing protocol-dispatch path.

Two items in the issue's acceptance gate **did not ship as written**,
both for the same reason — they require typer changes that are out of
the stdlib lane's scope. They are surfaced below as findings, not
silently dropped.

## Finding 1 — the sugar `m[key]` does NOT dispatch to HashMap (deferred)

The issue's gate item "Sugar `m[key]` dispatches to HashMap when type
is HashMap" lives entirely in the typer. `synth_index` in
`stage2/compiler/infer.kai` (lines ~9935-9948) hardcodes a closed
two-case dispatch:

```
TyCon(_, name, _) ->
  if name == "Map" { EModCall("map", "get") }
  else             { EVar("array_get") }
```

Adding a `name == "HashMap"` arm (lowering to
`EModCall("hashmap", "hashmap_get")`) is a one-line change, but it is
a typer edit. The brief explicitly said: *if sugar dispatch requires
typer changes (out of allowed scope), STOP and surface it as a
finding.* So the sugar is deferred. It is purely additive (the arm
only fires when the inferred container type is `HashMap`), so it can
land later with zero risk to existing `Map`/`Array` indexing. The 12
functions + tests are the core deliverable and shipped complete.

The in-file design note at `synth_index` argues against an `Index`
protocol (Tier 1 #3 bans type-class resolution) and prefers the
closed dispatch — adding HashMap is the third arm that note's last
sentence anticipates ("Revisit only if a third indexable container
… appears"). HashMap is exactly that third container; the follow-up
should add the arm, not the protocol.

## Finding 2 — `[k: Hashable]` is not expressible; `Hash` is runtime-dispatched

The issue (and #373) call the protocol "Hashable". The protocol that
#373 actually shipped is **`Hash`** (method `hash(x) : Int`), in
`stdlib/protocols.kai`. There is no `Hashable`. More importantly:

- **kaikai has no protocol bounds on free-function type parameters.**
  Writing `pub fn hashmap_get[k: Hash, v](...)` is rejected by the
  parser: *"type-parameter kind must be `Type` or `Measure`"*. Bounds
  (`impl[T : Hash] …`) only exist on `impl` declarations. This is
  Tier 1 #3 (no constraint propagation) made concrete. So the 12 fns
  use plain `[k, v]` and call `hash(key)` / `==` directly; the
  requirement is enforced where `hash` is called, not in the
  signature.
- A key type lacking `impl Hash` type-checks but **panics at runtime**
  ("no impl of Hash.hash"). This is the same shape as the AVL `Map`,
  which type-checks any key but panics on `<` for non-ordered ones.
  The issue's "Hashable[k] required at compile time" is therefore not
  achievable with today's protocol machinery for generic collections;
  it is a runtime contract, documented in the module header.

## Finding 3 — record keys do NOT dispatch through the generic boundary (sum types DO)

This was the structural surprise of the lane. A generic
`hash(key)` call inside the stdlib module compiles to a **runtime
impl-table dispatch** keyed by `kai_head_tag(key)` (see
`emit_proto_dispatch_shim_c` in `emit_c.kai`). Result:

- **Primitive keys** (`Int`, `String`, `Real`, `Bool`, `Char`) — work.
  Their stdlib `impl Hash` is registered and dispatches correctly.
  Verified end-to-end (`hashmap_basic.kai` keys on `String`/`Int`).
- **User sum types** with `impl Hash` + `impl Eq` — **work**. A sum
  value carries a proper head tag, so the dispatch finds the impl.
  `hashmap_collision.kai` relies on this: a constant-hash sum-type key
  forces every entry into one `HCollision` node.
- **User records** — do **not** work yet, even with
  `#[derive(Hash)]`. A record value boxed through the `key: k` type
  variable reads runtime head tag **0**, so the dispatch panics "no
  impl of Hash.hash for runtime head 0". `hash(record)` called
  *directly* (monomorphised at the call site) works fine — it is
  specifically the generic-collection boundary that loses the head
  tag. This is a compiler-side monomorphisation/dispatch gap
  (typer/runtime), not a carrier bug, and out of this lane's scope.

The AVL `Map` has the analogous gap (its `<` does not dispatch to any
non-primitive `Ord` impl — `map.get(m, CKey{...})` errors with "type
mismatch in `<`"). HashMap is actually *ahead* of Map here: it admits
sum-type keys, which Map cannot key on at all.

**Follow-up (not opened — needs authorisation):** record keys through
generic protocol dispatch. Worth its own issue once a maintainer
confirms the priority; it would unblock `HashMap[Record, v]` and
`Map`-over-records simultaneously.

## Design decisions

**Bitmap layout.** Children live in a persistent `[HamtNode]` sized to
`popcount(bitmap)`, edited with `take ++ [x] ++ drop`. The alternative
— an `Array[HamtNode]` threaded through the pure carrier — would need a
copy-on-write discipline that the stdlib `array.kai` surface does not
yet offer ergonomically, and would not be pure without a copy per
edit anyway. The list keeps the carrier pure and the code compact; it
is also the dominant performance cost (see benchmark) and the first
thing a perf follow-up should change.

**Collision handling.** Two keys equal on the full 64-bit hash but not
by `==` land in `HCollision(hash, [Pair])`, scanned linearly.
`hamt_merge_leaf` is the only producer (the `eh == h` arm); everywhere
else the trie deepens into single-child `HBranch`es until fragments
diverge. A collision down to one pair on remove collapses back to
`HLeaf`; an emptied branch collapses to `HEmpty` so the parent prunes
its bitmap slot.

**Size accounting.** `hamt_put` / `hamt_remove` return
`Pair[HamtNode, Bool]` where the `Bool` says whether a key was already
present / actually removed. This keeps `size` exact in one tree walk,
without a separate `contains` probe per mutation.

**Negative hashes.** `hash` may return a negative `Int` (two's-
complement wrap is the documented `impl Hash` contract). The carrier
uses `bit_ushr` (logical, zero-fill) for fragment extraction so the
sign bit never smears into deep-level fragments — this is the
normalisation the #373 retro flagged as HashMap's responsibility.

**Naming.** The issue pins flat `hashmap_*` names; the existing
`collections/map` convention is short qualified names (`map.get`).
The issue overrides — "ship the 12 public functions exactly as named"
— so the surface is `hashmap.hashmap_put(m, k, v)` under
`import collections.hashmap` (or unqualified via a selective import).
Slightly redundant when qualified, but faithful to the pinned spec
and consistent with #375 (HashSet), which will mirror `hashset_*`.

## Structural surprise — in-file `test` blocks calling privates break import

The first cut put white-box `test` blocks **inside hashmap.kai** (like
`stdlib/core/list.kai` does) to exercise the private `coll_*` /
`hamt_*` helpers — the only way to reach the collision path
deterministically before I knew sum-type keys dispatch. That broke
**every** `import collections.hashmap`: the privacy validator runs on
`test`-block bodies *before* the issue-#318 "drop prelude tests on
import" pass, so a `test` block calling a private function trips
"`coll_get` is private to module `hashmap`" on plain import — which
fails `test-stdlib-modules` (tier1). Confirmed minimal repro: a
`test` block calling only `pub` fns imports fine; one private call
breaks it. `list.kai`'s blocks survive only because they call `pub`
functions exclusively.

Resolution: removed the in-file blocks entirely and moved collision
coverage to `examples/stdlib/hashmap_collision.kai`, black-box through
the public API, once Finding 3 revealed sum-type keys dispatch. This
is strictly better — the collision test now demonstrates the real
user-facing contract and is golden-gated by `test-stdlib`. (The
privacy-vs-test-drop ordering is itself a latent compiler quirk; not
this lane's to fix.)

## Fixtures added + coverage

- `examples/stdlib/hashmap_basic.kai` (36 assertions): empty, single
  insert, small-batch get/contains, update (size stable), remove
  (incl. missing key), keys/values (sorted view for determinism),
  to_pairs/from_pairs (duplicate-collapse), merge (left-biased),
  iteration-order stability (same input built twice → identical
  keys), 10K and 100K round-trip with exact size.
- `examples/stdlib/hashmap_collision.kai` (20 assertions): forced
  `HCollision` via a constant-hash sum-type key — insert, get over
  the bucket, absent-key miss, overwrite within bucket (size stable),
  remove from bucket (siblings survive), full drain to empty,
  to_pairs over the bucket.

Both auto-discovered by `test-stdlib` (golden diff) and
`test-stdlib-modules` (import trampoline). No Makefile edit needed.

**Coverage gaps (deliberate):**
- Record keys: untested because they do not work (Finding 3).
- Full 64-bit primitive-key hash collision: not reachable by hand
  (birthday bound ~2^32, ~100 GB to brute-force). The constant-hash
  sum-type key is the deterministic substitute and exercises the same
  carrier path.
- `m[key]` sugar: untested because it is deferred (Finding 1).

## Cost vs estimate

The issue budgeted 5-7 days. The carrier itself was ~half a day
(de-risked with throwaway prototypes for insert/get/remove + branch
splitting before touching the real file). The bulk of the time went
to *discovering the dispatch boundaries* (Findings 2 & 3) — three
rounds of "why does the user-key test panic" before isolating
record-vs-sum-type. The sugar being a typer change (Finding 1)
removed the issue's "1 day sugar wiring" entirely. Net: well under
estimate, but only because two gate items turned out to be
out-of-scope typer work.

Also re-tripped the #373 retro's exact footgun: the first `Write` of
`hashmap.kai` landed in the **main checkout** instead of the worktree
(absolute path resolved to the non-worktree `stdlib/`). Caught
immediately because the worktree's `kaic2` could not find the module;
moved the file and verified the main checkout was clean. The
`feedback_kaikai_edit_lands_in_main_not_worktree` memory called this
exactly.

## Follow-ups left for next lanes

1. **#375 (HashSet)** — now unblocked. Build `HashSet[T]` on
   `HashMap[T, Unit]`; mirror the `hashset_*` flat surface.
2. **`m[key]` sugar for HashMap** — add the `name == "HashMap"` arm
   to `synth_index` (one line; typer lane). Finding 1.
3. **Record keys through generic protocol dispatch** — the head-tag-0
   boxing gap (Finding 3). Fixes HashMap-over-records AND Map-over-
   records. Needs a maintainer to confirm priority + an issue.
4. **HashMap perf crossover** — Array-backed (copy-on-write) branch
   children + Int unboxing, so the HAMT actually beats the AVL `Map`
   at scale (today it is ~1.6-2.3× slower; see `benchmarks/hashmap/`).

## Gate

- `make tier0`: green (selfhost byte-identical kaic2b.c == kaic2c.c,
  demos baseline, arena gate). Additive lane — no compiler/runtime
  source touched, so selfhost determinism is by construction.
- `make -C stage2 test-stdlib`: green incl. `hashmap_basic` and
  `hashmap_collision`.
- `tools/test-stdlib-modules.sh`: 52/52, hashmap.kai imports clean.
- tier1 + tier1-ASAN: to CI (lane handoff).
