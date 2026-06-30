# Lane experience — issue #1015 L1 (collections, stdlib base)

L1 of three lanes (L1 stdlib base → L2 parser literals `%{…}`/`%[…]` →
L3 docs/retro). L1 freezes the construction surface L2 desugars onto:
a uniform `X.from`, sentinel-free `array.from`, and the four immutable
<-> mutable `.to_X` conversions. `refs #1015` — the issue stays open
until L3.

## Scope as planned vs as shipped

Planned (from the issue's Lanes → L1):
1. Uniform `X.from` across map/set/hashmap/hashset/array.
2. Kill the Array sentinel (`from_list(xs, default)` → `array.from(xs)`).
3. Four `.to_X` conversions (Map<->HashMap, Set<->HashSet).
4. Fix all call-sites (demos, examples, fixtures, stdlib).

Shipped: all four, plus two scope deltas decided in-lane:
- **`array.copy` sentinel killed too.** The issue names only
  `from_list`, but `copy(src, default)` carries the identical sentinel
  for the identical reason (`array_make` seeding). With `array_empty`
  already in hand, dropping it was one branch and left the array API
  with zero sentinels rather than one. Both call-sites updated.
- **Stack/Queue/Stream kept their `from_list`.** They are out of L1's
  enumerated scope and their `from_list` is a *different* constructor
  (stack item order; lazy generator), not the build-from-list verb the
  issue unifies. Renaming them would conflate distinct intents.

## The load-bearing design decision: the empty Array

The non-obvious problem. `array.from(xs)` must drop the caller
sentinel, but the only Array constructor is the `array_make[T](n,
init)` op, which demands a reified `init: T` even at `n == 0`. The
non-empty case is trivial — seed `array_make` with the head element
and overwrite. The empty case has no element of type `a` to supply,
and `a` is polymorphic-abstract: no `panic`/`axiom`/`list.head` can
fabricate a phantom seed (panic evaluates eagerly and aborts; axiom is
body-less).

Verified empirically that `kai_array_make(0, init)` never dereferences
`init` (the fill loop does not run at len 0) — so the runtime is
willing; the blocker is purely the typer demanding the *expression*.
There is no pure-stdlib escape: an ML-family empty container needs a
nullary runtime primitive, not a derivation of `make(n, seed)`.

Resolution: add `array_empty[T]() : Array[T]` as a prelude prim,
`kai_array_make(0, NULL)`. **Placed outside `Mutable`** (a length-0
array writes no observable slot — putting it in `Mutable` would taint
every `array.from([])` spuriously, against the #251/#252
observable-effects rule). `array_make` itself is already effect-free
in the typer (`fn_ty`, not `fn_ty_eff`); the `Mutable` on `array.from`
non-empty comes from the `array_set` writes, which the masking pass
drops because the Array is local. So `array.from([])` is pure and
`array.from([h,...])` is maskable-to-pure — both end effect-free.

This is the one runtime touch in an otherwise stdlib-only lane. The
brief said "probably no runtime"; soundness overrode that. The prim is
minimal and self-hosts byte-identically (the compiler bundle never
calls it).

## Structural surprises the brief did not anticipate

- **Import cycles are rejected** (verified by spike: `kaic: import
  cycle detected`). The `.to_X` conversions cross two modules each.
  UFCS `m.to_hashmap()` resolves the method by the *receiver's* type,
  but the function may live in *any* imported module (also spiked:
  a fn in module B taking a type from module A resolves under
  `x.f()`). First attempt put both directions of each pair inside the
  mutable module (`hashmap` / `hashset`) to keep the immutable side
  import-free.
- **That first attempt regressed a pre-existing fixture, caught only
  by the serial backend-parity gate.** Adding `import collections.map`
  to `hashmap.kai` shadowed `hashmap`'s OWN `pub fn map` pipe export —
  the module name `map` rebinds over the local function — and corrupted
  `HashMap` construction (`array_get: not an array` at runtime; a Bus
  error under parity). This is exactly the "module name must not
  collide with a same-named local binding" trap. `import … as imap`
  did not save it: in the concatenated bundle the alias did not bring
  the module into the function bodies' scope (`cannot find imap`), and
  the type `Map` stopped resolving. The plain-stdlib smoke + tier0 +
  selfhost all stayed green through this — only `BACKEND_PARITY_JOBS=1`
  surfaced it, vindicating the "parity serial is the real gate" rule.
- **Resolution: a neutral `collections/convert.kai`** that imports all
  four collections and holds the four `.to_X` methods. It defines no
  `map`/`set`/`from`, so importing the collection modules collides with
  nothing, and no two collection modules import each other (no cycle).
  Users `import collections.convert`; UFCS resolves `m.to_hashmap()`
  to `convert.to_hashmap` by the receiver type.
- **`from` is a valid identifier**, not a keyword — confirmed it was
  already used as a `let` binding name in fixtures. The rename needed
  no parser change.

## Fixtures added

- `array_from_no_sentinel` — `array.from` on multi/singleton/empty,
  plus a String element type (no numeric default to fall back on) and
  an empty String-typed array. Guards the `array_empty` path.
- `collections_from_uniform` — each `X.from` with duplicate-key /
  dedup semantics.
- `collections_to_conversions` — Map<->HashMap and Set<->HashSet
  round-trips: opened handle is genuinely mutable (`put`/`add` after
  the crossing), frozen result keeps originals + the new entry, and
  the immutable source is untouched.

All three are auto-globbed by `test-stdlib` / `test-stdlib-survey` and
the backend-parity harness (no Makefile wiring needed). Each verified
identical on native and C, parity serial.

Coverage gap: no negative fixture (the surface adds no new rejection —
`.to_X` and `from` are ordinary functions; the `m[k] := v`-on-immutable
rejection that the issue mentions is pre-existing, not L1's).

## Gates

- selfhost byte-id (native) — OK before and after rebase onto main.
- tier0 — OK.
- `test-stdlib` (C, strict) + `test-pipes-collections` — all in-scope
  fixtures green.
- backend parity, serial (`BACKEND_PARITY_JOBS=1`) — the real native
  gate; parallel false-greens per the runtime note.

## Follow-ups for next lanes

- **L2** desugars `%{…}` → `map.from` / `map.empty` and `%[…]` →
  `set.from` / `set.empty`; the verbs are now stable to target.
- **L3** writes the `kai info syntax` Collections section (deliberately
  not touched here — only the catalog docs were updated to stop them
  lying about the verb).
