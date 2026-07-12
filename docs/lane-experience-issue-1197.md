# Lane experience — issue #1197: region RC/arena divergence (native vs C)

## Scope as planned vs as shipped

**Planned:** #1197 reported that native and C disagree on the RC/arena model for
a region-allocated `Tree<r>`, in *opposite directions* depending on the insert's
control-flow shape. Fixture A (guard `k<v/k>v`): native paid `incref≈N`, C was
zero. Fixture B (right-spine, no guard): the roles inverted. The integrator's
diagnostic localised the cause — reuse-in-place (the `_arm_ru` token / dual-path
`KConReuse`) colliding with arena fresh-alloc, each backend deciding
reuse-vs-arena independently — and pointed the fix at gating the reuse machinery
on `!in_region` in both backends.

**Shipped:** exactly that, but the native side was deeper than "gate one
predicate." The lane touched three coordinated layers:

1. **Frontend reuse recognisers (native, `kir_lower_walk.kai`).** Gated
   `arm_body_uses_reuse` (via a new `arm_reuses(body, st)`), `arm_is_top_reuse`,
   and `lower_reuse` on `not st.in_region`. Under a region every arm now takes
   the plain alias-dup path and `lower_reuse` emits a fresh arena ctor
   (`lower_reuse_fresh`).
2. **Match-arm reuse recognisers (C, `emit_c.kai`).** Gated the three
   `try_reuse_*` recognisers via `reuse_*_here(pat, body, cx)` wrappers so a
   region drops every arm to the plain ctor path (which `emit_variant_ctor` /
   `emit_cons_ctor` already route to the arena).
3. **The native TRMC spine step (`emit_native_trmc.kai`) — the real gap.**
   Suppressing the arm-top token was not enough on native: the variant TRMC step
   builds its spine cell via `kaix_variant_at_argv_masked` with the token in
   `slots[0]`, and *when the token is null it falls through to a fresh
   `kai_variant_u` on the RC heap* — not the arena. So even with the token
   always null under a region (an arena scrutinee's immortal rc blocks the
   steal), the fresh path leaked out of the arena and paid per-node RC.

## The structural surprise the brief did not fully anticipate

The brief framed the fix as "gate the reuse token in `!in_region` in both
backends." That is correct for C and for the native *frontend*, but native has a
**second, independent fresh-alloc site** the frontend gate does not reach: the
TRMC spine step's own cell build in the LLVM backend. The C oracle already
handled this — `trmc_fresh_alloc` under `cx.in_region` routes to
`kai_arena_variant` — but native's `ntrmc_variant_step` had no `in_region` at
all. This is why Fixture A (which lowers to a TRMC descent) stayed at
`arena_alloc=100` on native even after the frontend gate: the spine cell was
fresh-allocated, just on the wrong heap.

The fix threaded `in_region` down to the backend by adding a `Bool` field to
`KTrmcStep` (paralleling how C carries `cx.in_region`), and split out
`ntrmc_variant_build_arena` / `ntrmc_store_arena_slots` — a token-free arena
build over `kaix_arena_variant_masked`, no `slots[0]` token offset. The cons
step likewise routes to `kaix_arena_cons` under a region.

Lesson: **native has more fresh-alloc sites than C's per-ctor `emit_variant_ctor`
funnel.** A region-routing predicate that lives only at the KCon lowering misses
the TRMC spine, which builds its cell through a bespoke runtime entry. Any future
"route ctor X to the arena" work must audit the TRMC path separately.

## Why reuse-in-place collides with the arena

The arena's contract is alloc-fresh + free-wholesale, with arena cells carrying
the immortal sentinel rc (`INT32_MAX`) so RC ops no-op on them. Reuse-in-place
is the opposite bet: a uniquely-owned cell is recycled and its surviving children
are `incref`'d. The two are individually correct but their *observable trace*
diverges — reuse gives `arena_alloc` low + `incref≈N`, arena gives `arena_alloc`
high + `incref=0` — and which one a backend picks depended on control-flow shape
(guard → TRMC arm-top; spine → dual-path or the other backend's TRMC). Inside a
region the answer is unambiguous: **a ctor under a live region never takes a
reuse token.** The old node dies with the arena; recycling it defeats the whole
point and reintroduces the per-node RC the region exists to eliminate.

The scrutinee drop stays sound for free: an arena cell's decref is a no-op
(immortal rc), so the match-exit drop of the old node neither double-frees nor
leaks — `arena_live=0` on every fixture confirms it.

## The CI gap that let this ship

No gate compared the *per-region RC trace between backends*. #1193 shipped a gate
that grepped the emitted C for `kai_arena_variant(` and checked the native object
references `kaix_arena_variant` — both necessary, neither sufficient: they prove
the arena ctor is *reachable*, not that *every* node routes there and that the
two backends *agree on the count*. A backend that arena-routes the base-case
`Leaf→Node` ctor but reuses the spine passes the #1193 grep while paying
`incref≈N`.

`test-region-1197-parity` closes it: build `region_tree_1123` (guard) and
`region_spine_reuse_1197` (spine) on both backends, assert `incref_total==0` on
each, and assert `incref` + `arena_alloc` agree C↔native. It rides tier1-native
(the native leg needs LLVM), not TEST_LIGHT.

## Fixtures added

- `examples/perceus/region_spine_reuse_1197.kai` (+ `.out.expected`) — the
  right-spine insert, the shape that most tempts reuse-in-place and the half of
  #1197 that inverted the roles.
- The parity gate reuses the existing `region_tree_1123` as its guard-insert
  half, so both control-flow shapes are covered by one gate.

## Coverage gaps left

- The gate checks `incref_total` and `arena_alloc`; it does not diff full
  per-callsite RC traces. A regression that keeps the totals equal but shifts
  *where* the increfs happen would pass. Totals are the load-bearing signal for
  #1197 (the divergence was in the totals), so this is acceptable, but a
  callsite-level region trace is a natural follow-up if a subtler divergence
  surfaces.
- Reuse OUTSIDE a region is verified intact (rb-tree bench: `reuse_in_place`
  identical C↔native, `incref` unchanged; the #1053/#1104/#1025/#995 fixtures
  stay green) but not wired into the new gate — the existing per-fixture gates
  already cover it.

## Verification summary

- 4 traces (A+B × C+native) converge to `val=5050, incref_total=0,
  arena_alloc=5050`.
- 13 `region_*` fixtures: identical output C↔native, RC traces (`incref`,
  `arena_alloc`) match between backends.
- rb-tree bench (non-region reuse): `reuse_in_place=6301528`, `incref=2000006`
  identical on both backends — reuse-in-place outside a region untouched.
- tier0, selfhost byte-id (C + modular), arena gate (plain + ASAN) green.
