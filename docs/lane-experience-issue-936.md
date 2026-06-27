# Lane experience — issue #936: O(n²) `Set` → AVL-backed log-linear

## Scope as planned vs. as shipped

Planned: kill the quadratic in `stdlib/collections/set.kai`. The list
carrier made every multi-element op O(n²) — `contains` was a linear
scan and `insert` copied the whole prefix with `list_append`, so
`from_list(iota(n))` was ∑O(i). It OOMed a 4 GiB heap at 15k while
HashMap and AVL `Map` stayed flat at 50k.

Shipped: exactly that. `Set[a]` now wraps an inner AVL `Map[a, Int]`
plus a monotone `next` counter. Membership/insert/remove are O(log n),
`from_list` O(n log n), and the algebra ops O((|a|+|b|) log). The
public surface (`empty`, `size`, `is_empty`, `contains`, `insert`,
`remove`, `to_list`, `from_list`, `union`, `intersect`, `diff`, `map`,
`flat_map`, `filter`) is byte-identical in signature; only the carrier
and complexity changed.

## Carrier chosen and why

`Set[a] = { priv index: Map[a, Int], priv next: Int }`.

The brief's preferred shape was a bare `Map[a, Unit]`. That gives the
right complexity but AVL iterates in **ascending key order**, which
would silently drop the `Set`'s documented and tested **insertion
order** (`from_list([5,1,5,3,1,3])` is contractually `5,1,3`, not the
sorted `1,3,5`). The existing fixtures `set_basic` and `set_pipes`
assert insertion order explicitly.

So instead of `Unit` the value slot records the **insertion sequence
number**: `insert` hands out `next` (then bumps it) only when the
element is new, keeping the original number on a re-insert. `to_list`
reads `map.to_pairs`, `sort_by`s on the sequence, and projects the
key. This preserves insertion order *exactly* — both existing fixtures
pass byte-for-byte unchanged — while every membership operation rides
the AVL's O(log n). `union`/`intersect`/`diff` rebuild a fresh set by
re-inserting in `s1`'s `to_list` order, so they also keep `s1`'s order
(matching the old contract). The cost is a single O(n log n) `sort_by`
inside `to_list` (and the ops that go through it), which is dominated
by the build itself and never reintroduces the quadratic.

`remove` leaves `next` untouched and does not reclaim the freed
sequence number — correct, because the only invariant `to_list`
relies on is *monotonicity*, not density. Insert-remove-reinsert of
the same element therefore re-appends at the tail, identical to the
old `list_append` behaviour.

## The element-bound decision

The old list carrier used `==` (structural, runtime-provided) and so
accepted **any** element type, including records and sum types. The
AVL carrier compares with `<`, which is a total order only on `Int`,
`Real`, `Char`, `String` (and sum types deriving `Ord`); records
**panic on the first comparison**, exactly as `Map` keys already do.

This is a runtime restriction, **not** a change to the declared
type-level bound: both the old and new `pub type Set[a]` leave `a`
free (no `[a: Eq]` / `[a: Ord]` constraint was ever written, mirroring
`Map[k, v]`). No existing caller in the repo is affected — every
`Set` use is `Set[Int]`, and `HashSet` already carries the same
"primitives + sum types, not records" limitation. The new module
`#[doc]` states the `<` requirement up front. Per the brief, this was
the option that does not break existing callers; the alternative (a
two-carrier Eq-only fallback) was rejected as duplicate machinery with
no caller to justify it.

## How semantics-identity was verified

- `examples/stdlib/set_basic.kai` and
  `examples/pipes/collections/set_pipes.kai` — the pre-existing
  fixtures that pin dedup, membership, size, union/intersect/diff
  results, the pipe combinators, and insertion order at every step —
  both diff **byte-identical** to their unchanged `.out.expected` on
  C and on the native default backend.
- New regression `examples/stdlib/set_scale.kai` (+`.out.expected`):
  builds 50k-element sets, asserts union=75000 / intersect=25000 /
  diff=25000 / dedup-to-50000, and that `to_list` head is still
  `0,1,2,3,4` (insertion order survives at scale). Auto-globbed into
  `test-stdlib` (tier1), runs on both backends.

## Measurements (mac, `KAI_MAX_HEAP=4g`)

| input | before (list carrier) | after (AVL carrier) |
|---|---|---|
| `from_list` 15k | **OOM at 4 GiB** | included below |
| `from_list`+union+intersect+diff 50k | n/a (OOM long before) | 0.55 s, RSS ~289 MB |
| `from_list` 5k+15k+50k (one process) | n/a | 0.42 s, RSS ~59 MB |

The before-figures are the brief's measured baseline (5k 0.7s, 10k
1.8s, 15k OOM). After: 50k completes in well under the 60 s / 4 GiB
host-safety cap with flat, low RSS — the slope is log-linear.

## Compiler-throughput risk

The compiler does **not** use `collections.set` anywhere (greps over
`stage2/` find only "working set" prose, never `import
collections.set` or `set.*` calls). No self-compile throughput
exposure; selfhost byte-id is the regression guard.

## Follow-ups left for next lanes

- If a future caller genuinely needs an Eq-only ordered set (record
  elements), the clean path is a separate `OrderedSet`-style fallback
  or lifting the record to a primitive id — not reviving the list
  carrier.
- `to_list`'s `sort_by` is O(n log n) on each call; callers that
  iterate repeatedly without mutating could cache it. Not worth it
  today — no hot caller exists.
