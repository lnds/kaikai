# Lane experience — issue #1123: kind Region (arenas as a kind)

The last piece of the initial kind catalog. `Region` joins
`Type`/`Effect`/`Measure`/`Currency` over a new theory, `Structural`
(identity). The live case, post-`Vec`, is what `Vec` cannot do: bulk
**linked** structures in an arena (`Tree<r>`), lifetime batching, and
the escaping path.

## Scope: planned vs shipped

Planned (brief): theory Structural + kind Region; `region { r -> }`
block; `Tree<r>` on user heads; ctor-routing by type to the arena;
wholesale-free of the no-escape case; escape handled safely (reject or
generational). Closes #1123.

Shipped: all of the above except the **generational** escaping path,
which stays a follow-up. The escaping case is nonetheless **safe**: a
`Tree<r>` that crosses the brace is deep-copied onto the RC heap (the
shipped `region {}` discipline, confirmed by Eduardo), not a UAF. The
CORE is sound on its own — no escape is unsafe — so the cut is
legitimate per the brief's explicit allowance.

## Design decisions (and alternatives considered)

1. **`theory Structural = builtin`, not a property set.** Region
   identity ("two regions unify iff the same skolem") is exactly what the
   dimension unifier already does (symbol equality in `unify_abelian`),
   so no `unify_structural` engine is written — Module was the template
   (a kind whose habitants are atoms served by the abelian solver). A
   property set like `{ identity }` would have implied operations Region
   does not have.

2. **Surface `region { r -> }`, the nursery rhyme.** Ruled out
   `with_arena(fn[r](a) ...)` (rank-2, which HM must not grow). The block
   skolemizes `r` directly. The parser reuses the bounded `Ident ->`
   lookahead; the marker `ECall(EVar("region"), [ELambda([r], block)])`
   carries the name, and the pre-typer region pass reads it off the
   binder and unwraps — the `ELambda` never reaches the typer.

3. **`Tree<r>` = `TyDimT(TyCon("Tree"), USym(r))`** — reuse the
   dimensioned node (Money precedent), not a new `TyBranded` arm (which
   would force a new case at ~dozens of exhaustive matchers). Opened only
   the `parse_type_base` native-heads gate to accept `<habitant>` on a
   user head; structural unification of the head is untouched and the
   brand rides along.

4. **Routing by the SIGNATURE type, not the tparam list nor the per-ctor
   `.ty`.** This was the hard-won correction — see Surprises.

## Structural surprises the brief did not anticipate

The routing (the "half a new piece" the issue named) fought three
compiler realities at once:

- **The brand does not survive on every ctor's `.ty`.** Measure stamps
  its brand at the literal *syntax* (`1.5<m>`); Region wanted it to flow
  from the annotated context (`: Tree<r>`) into synthesized ctors. It
  doesn't — `ctor_check_or_synth` drops the expected `TyDimT` on the
  floor, and a rigid `USym(r)` won't unify a bare `TyCon` into a brand
  (it collapses to `UOne`). Measured: 30 of 32 ctors reached emit as
  `TyCon(Tree)`, brand-free. A stamp-the-brand fix double-branded and
  covered only 1 ctor. **Resolution:** route by the *param/return type*
  (`params_have_region`), which survives monomorphization (mono changes
  the mangle, not the written `ptype`), where the tparam list does not
  (mono empties it before tcrec runs).

- **Three ctor emission paths, not one.** Direct
  (`emit_variant_call_typed`), Perceus reuse (`emit_reuse_variant_body`),
  and TRMC (`emit_trmc_variant_step`). A recursive `insert` is
  tail-recursive-modulo-cons, so its Nodes are built by the TRMC spine
  step — which ignored `in_region`. Disabling TRMC for region fns broke
  (non-exhaustive match); the sound move (asu) was to keep TRMC and route
  only its fresh-alloc brick to `kai_arena_variant`. The arena sentinel
  rc already makes reuse a no-op at runtime, so nothing else changes.

- **Two latent runtime bugs this feature is the first to hit.**
  (a) `kai_arena_variant` sized the cell at `sizeof(KaiValue)` and wrote
  slots into a detached `kai_arena_raw` block that `kai_var_slots`
  (`&v->as`) never reads — a variant-with-payload built in an arena read
  back garbage (tag corruption → "non-exhaustive match"). Fixed to
  reserve `kai_var_block_size(n)` inline. (b) `kai_deep_copy_out` copied
  every variant slot as a pointer and recursed, dereferencing a raw
  `Int` slot as an address → segfault when a `Tree<r>` escaped. Fixed to
  read the per-slot kind. Both were dead until an arena variant carried a
  primitive slot, which the shipped lexical `region {}` never produced.

## Fixtures added, coverage gaps

- `examples/perceus/region_tree_1123.kai` (+ `.out.expected`): the
  showcase — build a 100-key tree in an arena across region-poly helpers,
  fold to Int, r does not escape. `arena_alloc=5050 arena_free=5050
  arena_live=0`, `incref=0` (zero per-node RC), vs 107 RC allocs + 99
  increfs on the plain-`Tree` baseline.
- `examples/perceus/region_tree_escape_1123.kai`: the escaping tree,
  deep-copied out — the `kai_deep_copy_out` slot-mask regression guard.
- `make -C stage2 test-region-1123` runs both on the C backend, greps
  for `kai_arena_variant` (routing did not regress), and diffs output.
  Wired into `.PHONY`, `TEST_LIGHT_TARGETS`, and `test-fast`.

Gap: no native-backend region fixture yet (the C backend carries the
routing; native parity for arena variants wants its own guard). The
generational escaping path has no fixture because it is not shipped.

## Follow-ups left for next lanes

1. **Generational escaping arenas** — the real remaining case: a
   `Tree<r>` that escapes and is *used* by reference (not copied), backed
   by a generation tag + runtime check (Vale-style). This is what
   `region-passing` / second-order region polymorphism buys and the CORE
   deliberately omits. The CORE is stack-of-regions LIFO (Cyclone
   stack-scoped), only the top arena allocs.
2. **Nesting soundness pin.** `region { r1 -> region { r2 -> f[r1]() } }`
   would route `f`'s `Tree<r1>` ctors to `kai_arena_current()` = r2, not
   r1 — a brand≠current mismatch. This is the same deferred escape v1.x
   documents (a `T<r>` crossing a region border), not a regression this
   lane introduces; the deep-copy-out covers each block's own brace. A
   conservative reject or the generational check closes it.
3. **#1121 (unboxed value records)** likely shrinks: flat inline layout
   comes free with the arena for the linked case. Not closed here —
   noted for its owner.

## Cost vs estimate

The catalog + surface + type grammar (B1–B3) were fast and clean —
Module/nursery/Money precedents held. The cross-fn arena routing (B4) was
the entire cost: it is where the "half a new piece" lived, and it
bottomed out in two runtime bugs plus a three-path emission reality that
no single interception point covers. The escape path (B5) collapsed to a
2-line slot-mask fix once the runtime bug was seen — the escape was
already safe by design, only the copy-out was broken.
