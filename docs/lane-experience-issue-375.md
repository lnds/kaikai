# Lane experience — issue #375 (`stdlib/collections/hashset`: HashSet[t])

## ⚠️ The issue #375 body is STALE — read this first

Anyone arriving at issue #375 from the tracker will read a body written
against two things that **never shipped**. Do not build against it.

1. **`Hashable` → `Hash`.** The issue constrains elements with
   `[t: Hashable]`. There is no `Hashable` protocol in kaikai. The
   hashing protocol is `Hash` (`stdlib/protocols.kai:287`,
   `protocol Hash { fn hash(...) }`). HashSet elements are constrained
   by `impl Hash` + `impl Eq` (via use, not an explicit bound — see
   Finding 2), the same way HashMap keys are.

2. **persistent → mutable.** The issue specifies a *persistent*
   HashMap and persistent HashSet signatures (`add : HashSet[t]`,
   returning a fresh set). HashMap shipped **mutable** (#374 was
   redesigned mid-lane — the pure HAMT benchmarked ~2× slower than the
   AVL `Map`; see `docs/lane-experience-issue-374.md`). Its `put` /
   `remove` mutate in place and return `Unit`, every op carries
   `/ Mutable`. HashSet therefore mirrors the *as-shipped* HashMap:
   `add` / `remove` mutate in place and return `Unit`, every op carries
   `/ Mutable`.

The instruction the lane followed: **mirror the shipped sibling, not
the stale issue.** Read `stdlib/collections/hashmap.kai` first, copy its
mutation model, effect discipline, and constraint idiom verbatim, and
treat the issue body only as the list of *which operations* to provide
(union/intersection/difference/is_subset etc.), never *what shape* they
take.

## Scope as planned vs scope as shipped

Scope shipped exactly as the corrected brief specified — no pivot, no
down-scope. A thin wrapper over the mutable HashMap:

- **Carrier** `HashSet[t] = { inner: HashMap[t, Unit] }`. A member is a
  HashMap key whose stored value is the single `Unit` value `()`. The
  wrapper holds no state of its own — `size` reads through to
  `hashmap.size`, `to_list` to `hashmap.keys`, etc.
- **12 pub fns, short module-relative names** (no `hashset_*` aliases,
  mirroring hashmap's decision): `empty`, `size`, `is_empty`,
  `contains`, `add`, `remove`, `to_list`, `from_list`, `union`,
  `intersection`, `difference`, `is_subset`. Called `hashset.add(s, x)`
  under `import collections.hashset`.
- **Mutation model.** `add` / `remove` mutate in place, return `Unit`,
  carry `/ Mutable`. The set-algebra ops (`union` / `intersection` /
  `difference`) build and return a FRESH set and leave both arguments
  untouched — the same contract the list-backed `Set` follows. Only
  `add` / `remove` mutate.
- **Fixtures** `examples/stdlib/hashset_basic.kai` (Int + String
  round-trip, re-add no-op, 100K members across several resizes,
  `from_list` dedup, remove-missing no-op) and `hashset_ops.kai`
  (union/intersection/difference/is_subset on small Int sets with
  hand-verified expected results, plus args-unchanged assertions).
  Both golden-gated by `test-stdlib` (auto-globs `examples/stdlib/*.kai`).
- **Docs** `stdlib-layout.md` (tree + prose row) + `stdlib-roadmap.md`
  (inventory row + tree line).

**Zero compiler change.** Unlike #374 (which added the `m[key]`
`synth_index` arm), this lane is pure stdlib + fixtures + docs. Selfhost
stays byte-identical trivially — no stage2 source is touched.

## Design decisions and alternatives considered

**Delegate-everything vs reimplement the table.** The wrapper could
have inlined a second bucket array specialised for "value is always
Unit", saving the `Pair[t, Unit]` cons cell per entry. Rejected: it
would duplicate the entire chaining + resize machinery (the bulk of
hashmap.kai) for a one-word-per-entry saving, and would drift out of
sync the first time hashmap's internals change. The whole reason
HashSet earns its slot cheaply is that it is a *thin* wrapper. The
`Unit` value is a tagged immediate in the runtime, so the per-entry
cost is one pointer slot in the `Pair`, not a heap box. Acceptable.

**`add` semantics on a present element.** `hashmap.put` overwrites the
existing key's value and keeps the count exact, so `add` of a present
element is a clean no-op on size — no special-casing needed. Verified
by the `re-add keeps size 3` assertion in `hashset_basic`.

**Fresh-set algebra via `to_list` fold.** `union(a, b)` could fold
directly over the internal pairs, but `to_list` (→ `hashmap.keys`) is
already the public read path and keeps the wrapper honest (it never
reaches into HashMap's `priv` fields). The cost is one intermediate
`[t]` per operand; for set algebra that is dominated by the per-element
hash+insert anyway.

**Element constraint idiom — copied verbatim from hashmap.** The pub
fns are written `pub fn add(s: HashSet[t], x: t) : Unit / Mutable`,
with **no explicit `[t: Hash + Eq]` bound**. hashmap.kai does the same:
the `hash(key)` and `== ` calls inside the delegated-to body resolve
the `Hash` / `Eq` requirement by use. kaikai has no Haskell-style
constraint propagation (CLAUDE.md Tier 1 §3), so the requirement is
discharged at the leaf call site, not threaded through the signature.
Copying this idiom verbatim was load-bearing — inventing a `[t: Hash]`
bound would not have parsed against the as-shipped surface.

## Structural surprises the brief did not anticipate

None of consequence. The brief was unusually precise (it had already
done the issue-vs-reality reconciliation), so the lane was mechanical:
read hashmap.kai, mirror it, capture goldens from real runs, wire docs.
The one thing worth recording for the next reader:

- **`bin/kai` in a bare shell shells out to `cc` via `CC`**, but the
  per-fixture harness path is `kaic2 --path ../stdlib <f> > out.c` then
  `cc`. Both work; the parity harness uses `KAI_BACKEND={c,llvm}
  bin/kai build`. All three were exercised — C and LLVM produce
  byte-identical stdout on both fixtures.

## Fixtures added and coverage gaps

- `hashset_basic.kai` + `.out.expected` — empty/add/contains/size/
  is_empty/remove/to_list, re-add no-op, remove-missing no-op,
  `from_list` dedup, 100K-element resize stress (Int), String members.
- `hashset_ops.kai` + `.out.expected` — union (6), intersection (2),
  difference both directions (2/2), disjoint sets, `is_subset` (proper
  subset / non-subset / empty-subset / self-subset / superset-not-
  subset), and explicit "args unchanged after algebra" checks.

**Coverage gap — records as elements.** Inherited verbatim from
HashMap: a user **record** element boxed through the `x: t` type
variable reads runtime head tag 0, so `hash(x)` panics even with
`#[derive(Hash)]`. This is a compiler-side dispatch gap (see
hashmap.kai header lines ~42-50 and `docs/lane-experience-issue-374.md`),
not a HashSet bug. Fixtures use primitive elements (Int/String) to stay
green; the gap is documented in the module header in the same wording
style as hashmap.kai. When the HashMap dispatch gap closes, HashSet
inherits the fix for free (zero HashSet change).

## Real cost vs estimate

Smaller than a typical stdlib lane: no compiler touch, no new runtime,
no benchmark (the performance is HashMap's, already measured in #374).
The work was reading hashmap.kai carefully, writing the wrapper, and
authoring two fixtures whose expected output I computed by hand and
confirmed against real C + LLVM runs.

## Follow-ups left for next lanes

- **Records as elements** unblocks when HashMap's generic-dispatch gap
  closes (same issue that blocks records as HashMap keys / `<` for the
  AVL `Map`). No separate HashSet issue needed — it is one underlying
  compiler fix.
- **Pipe participation** (`|` / `||` / `|?`). `Map` and `HashMap` gained
  pair-shaped `map`/`flat_map`/`filter` exports (#594/#613) so they
  participate in the pipe convention. HashSet could gain element-shaped
  `map`/`filter`/`fold` for the same reason. Out of scope for #375
  (the issue's 12-op surface is the contract); a candidate for a
  follow-up if set pipelines become idiomatic.
- **Symmetric difference** (`a △ b`) is a natural 13th op, omitted
  because the issue's surface does not list it. Trivial to add as
  `union(difference(a, b), difference(b, a))` if requested.
