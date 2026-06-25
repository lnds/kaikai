# Lane experience — issue #914: index TyEnv by name

## Scope as planned vs as shipped

**Planned:** add a per-name index to `TyEnv` so `ty_env_lookup` /
`ty_env_collect_candidates` / `ty_env_modules_exporting` stop walking
the whole multi-thousand-entry assoc list per call site, recovering
self-compile throughput — with byte-for-byte identical resolution.

**Shipped:** exactly that. A new `stage2/compiler/infer_index.kai`
holds a persistent, hash-keyed AVL `NameIndex[a]`. `TyEnv` carries an
`index : NameIndex[TyEntry]` mirroring `entries`. `ty_env_lookup` and
`ty_env_collect_candidates` consult the index; `ty_env_modules_exporting`
was found to be **dead code** (no callers) and left on the linear path.

## Index design

- **Bucket key** = a name's last `::`-segment (`ty_bucket_key`),
  applied identically on insert and query. `list::map` and bare `map`
  hash to the same bucket `map`; a qualified lookup target `mod::name`
  buckets under `name`. The **scan inside the bucket re-applies the
  exact byte predicate** of each function (`== target` for lookup, the
  `::target` suffix extract for candidates), so a hash or bucket
  collision is filtered out, never returned. This is what makes the
  bucketing airtight regardless of how names share a segment.
- **Order invariant:** within a bucket, chain order == insertion order
  restricted to that name (both newest-first). Every payload is
  prepended to `entries` and its bucket chain at the same moment, so a
  per-name filtered walk over `entries` yields the identical
  subsequence as the bucket chain. This preserves the #748/#749
  root-fn-shadows-stdlib precedence (newest wins).
- **Structure:** a height-balanced AVL tree keyed by `string_hash(key)
  mod 4096`. Arithmetic-only hashing (`((h % m) + m) % m`) — no bit
  primitives — so the bundle compiler (kaic1) can build the module.

## How equivalence to the linear walk was proven

1. **Standalone diff-test** (`scratchpad`, not committed): for a deep
   env with adversarial shapes asu flagged — qualified target,
   byte-suffix false-friend (`flatmap` must not land in the `map`
   bucket), 3-segment keys, hash collisions, deep same-name shadowing,
   empty bucket — the index output was diffed against the linear walk.
   Byte-identical on every case.
2. **selfhost byte-id on BOTH backends** (C and native): the real
   corpus. If the index resolved any name differently, the emitted C
   would diverge and byte-id would fail. Both green.
3. **Full `test-shadowing` suite** (18 fixtures incl. the new
   `name_index_deep_collisions`): root-fn-shadow, qualified-vs-bare
   coexistence, protocol-op candidate collection — all green.

## The structural surprise the brief did not anticipate

The first three index structures (fixed-depth binary **radix trie**,
generic and monomorphic, wrapper-record and bare-sum, shared and
disjoint payload) all **self-compiled in gen-A but crashed gen-B** with
`field access on non-record` / `EXC_BAD_ACCESS`. Root cause (asu): a
read-walk over an **owned** recursive sum that returns a value from a
**deep leaf**, threaded back up through frames that each **discard a
sibling**, makes Perceus decref the returned value. A standalone unit
test with **Int payloads passes** (Int is immediate); the bug needs a
**boxed/record payload** and only surfaces in gen-B — so a naive local
test is a false-green.

The fix is the **AVL Map idiom**: the read returns the chain **bound at
the matching node** (`INode(nk, nv, _, l, r) -> if key==nk { nv }`),
never a value threaded up through discarding frames. Confirmed
diagnostic: reading the same owned structure twice (first read borrowed,
not last-use) made the crash vanish. The lesson is recorded in memory
(`perceus_read_walk_must_reemit_bound_value`) — a reusable anti-pattern
for any future pure index/cache over a recursive sum.

## Throughput before/after

Self-compile (`kaic2 main.kai`, best-of-3, `KAI_MAX_HEAP=8g`, same
machine):

- **baseline (no index):** ~51.5 s
- **with index:** ~41.7 s

≈ **19% faster** (1.24×). The O(env × call_sites) quadratic in the
per-call-site name lookups is replaced by O(log buckets) AVL lookups;
env construction now pays an extra AVL insert per binding, but the
lookup win dominates. Emitted C size is essentially unchanged
(116560 vs 116200 LOC — the delta is the new module's own code).

## Qualified-key subtlety

`ty_env_lookup` is called with both bare and `mod::name` targets;
`ty_env_collect_candidates` is only ever called with bare targets (all
three call sites pass an `EVar` name). The last-`::`-segment bucket key
handles both: a qualified target buckets under its final segment, and
the in-bucket `== target` / `::target` predicate disambiguates. No call
site passes a qualified target to `collect_candidates`, so the
query-side normalisation reduces to the bare case — verified by grep
before relying on it.

## Follow-ups

- `ty_env_modules_exporting` is dead code (left linear); a future
  cleanup lane could remove it and `modules_exporting_loop` /
  `ty_entry_split_mod` if nothing reintroduces a caller.
- `index_buckets() = 4096` is a fixed constant; if the env ever grows
  far beyond a few thousand distinct names the AVL depth stays
  O(log names) regardless, but the bucket constant could be revisited.
