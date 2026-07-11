# Lane experience — issue #1193: region arena routing on the native backend

## Scope

`kind Region` (shipped in v0.99.5, issue #1123) routes arena allocation
**only on the C backend**. On the native backend — the default since #851 —
a `Tree<r>` built inside `region { }` allocated every `Node` on the RC heap
(`kai_alloc` + incref/decref per node) instead of the bump arena. Result was
semantically correct (same output) but delivered none of the performance
benefit the feature exists for.

The integrator had already diagnosed the gap with evidence (`nm` of both
binaries, root cause in the source, runtime confirmed ready). This lane
verified the diagnosis and shipped the native codegen.

- **Planned:** propagate the region datum into the native lowering, route
  region ctors to `kaix_arena_*`, bracket the region block with push/pop +
  deep-copy-out, add the missing masked forwarder, add a regression fixture,
  clean the contingent/lying comments.
- **Shipped:** exactly that, plus a shared brand-detection module so both
  backends route off one verdict.

## Why the runtime was ready but the codegen was not

`stage0/runtime_llvm.c` already defined `kaix_arena_push/pop/variant/cons/
record` and `kaix_deep_copy_out` (forwarders to the C `kai_arena_*` helpers)
— they linked, but nothing emitted a call to them (`grep kaix_arena` over the
native emitters = 0). A comment even *claimed* the LLVM backend "routes every
constructor through `kaix_arena_alloc`" — false. The forwarders were staged
ahead of the codegen and then the codegen never landed, so the runtime and a
lying comment outlived the missing emit path.

One forwarder was genuinely absent: `kaix_arena_variant_masked` (the rb-tree's
kind-1-Int-slot path). The masked native ctor passes an `[n x i64]` word buffer
aliased as `KaiVarSlot*`; the arena mirror reads the tag's startup-registered
mask (`kai_slot_mask_of`) and forwards to `kai_arena_variant(tag,name,n,mask,
slots)`, exactly like `kaix_variant_masked` but into the arena.

## How the C routes vs how the native had to

The two backends decide "is this ctor in a region?" from different data,
because they run on different IRs:

- **C** emits from the AST (`Expr`) and flips an `EmitCtx.in_region` flag by
  THREE independent paths: (1) lexically inside the `region { }` block body,
  (2) **by signature brand** — a fn whose param/return carries a region-branded
  aggregate (`Tree<r>`) runs its whole body in-region, and (3) by the ctor's
  own resolved `.ty`. Path (2) is what makes the cross-fn case work: `tree_
  insert`'s `Node(...)` sits *outside* the `region { }` lexically, and only the
  `Tree<r>` signature marks it.
- **native** emits from the KIR, which had LOST both signals: the `region { }`
  block is unwrapped to a bare block in the rewrite pass (so no lexical marker
  survives), and `KCon` never carried the resolved type. The dead `region_id:
  Option[Int]` slot on `KFn`/`KBlock` was always `None` — a slot nobody filled.

The fix threads the same signature-brand predicate the C reads
(`fn_has_region_brand`, ported to `compiler.region` and shared) into `lower_fn`,
seeds a `LowerSt.in_region` flag for the body's extent, and stamps each `KCon`/
`KRecord` with a 5th `Bool` field the native emitter consumes. The lexical
`region { }` block re-derives its extent from the side table (keyed by source
position, same as the C's `EmitCtx.regions`) in `lower_block`.

## Design decisions

1. **Brand-in-signature is primary; lexical is secondary.** The brand is the
   only mechanism that composes across fn boundaries, so it carries the
   canonical `region_tree_1123` case. The lexical path covers only ctors
   written directly inside `region { }` (`region { [1,2,3] }`). Both are needed
   — several fixtures build lists directly in the block. (asu review confirmed
   this split; same model as Koka's `alloc<h>`.)
2. **A `Bool` on `KCon`, not the `region_id` slot.** `region_id` models region
   *identity* (which of N); the runtime has ONE arena stack, so routing needs a
   binary predicate, not an id. The flag is decided early in `lower_fn`,
   materialised on the node, consumed dumbly in `nemit_con`, and rides through
   inline cloning pass-through. `KConReuse` was already 5-ary, so a 5th field on
   `KCon` is not new territory.
3. **push/copy-out/pop are three statements in the stream, at the join.** A
   basic-block IR expresses ordering through statement order. The copy-out + pop
   are emitted at the point `lower_expr(tail)` returns to — which post-dominates
   any `if`/`match` fork inside the tail — so an early fork cannot skip the pop
   or free the value before the copy. Copy-before-pop is load-bearing: the arena
   free would dangle an escaping value otherwise.
4. **Shared module over duplication.** Porting the brand helpers to
   `compiler.region` and deleting the C copies avoided a bundle name collision
   (`te_is_region_branded` defined twice) AND made both backends route off one
   verdict — no drift risk.

## Structural surprises the brief did not anticipate

- **`region_id` was a dead slot, not a live channel.** The brief suggested
  "propagate `region_id` onto the `KCon`". But `region_id` was never populated
  ANYWHERE — it was `None` at all 6 construction sites. Poblating it would have
  been indirect (the emitter would consult "am I in a branded KFn?"); a direct
  `Bool` on the ctor node was cleaner.
- **Void runtime calls need a dedicated path.** `kaix_arena_push`/`pop` return
  `void`; the generic `KCall` native path assumes a `ptr` return. They are
  intercepted in `nemit_op_fx` and emitted with a void signature (mirror of
  `kaix_assert_check`).
- **The cons spine is `KPrim`, not `KCon`.** List literals lower to
  `KPrim("kai_cons", …)`, a separate path from user-ctor `KCon`. In-region it
  becomes `KPrim("kai_arena_cons", …)`, which the native `kai_*`→`kaix_*`
  prefix rename maps to `kaix_arena_cons` with no emitter change.
- **kaic1 codegen trap.** A `match`-arm body that starts with `let` and
  continues with more statements must be wrapped in explicit `{ }`, or the
  stage1 bootstrap compiler mis-parses it ("expected expression").

## The brand cross-fn trap

The load-bearing subtlety: `build_tree` calls `tree_insert` cross-fn, and BOTH
must route their `Node(...)` to the arena. This works because the brand travels
through the SIGNATURE — `tree_insert[r](t: Tree<r>) : Tree<r>` — which survives
monomorphisation (mono rewrites the mangle, not the written `TypeExpr`). The
native reads the same post-mono `params`/`ret_ty` that the C's
`params_have_region` reads, so the verdict is identical. Verified the C routes
(3 `kai_arena_variant` sites in the emitted `.c`) before trusting the brand
survives mono.

## Fixtures + the CI gap that let this ship

The gap that let #1193 ship silently: `test-region-1123` grepped the emitted
**C** for `kai_arena_variant(` — it never checked the native binary. A
region feature that routes on C but not native passed the gate.

This lane adds `test-region-1193-native`: it builds the canonical fixture with
`--backend=native` and asserts the native binary's `nm` shows the arena symbols
(`kaix_arena_variant`, `kaix_arena_push`, ...) that the C binary shows — the
exact check whose absence let the gap through. Both region fixtures also run
under serial backend parity (C vs native identical output).

## Follow-ups left for next lanes

- The generational / precise-escape path (zero-copy escape via
  brand-in-signature + region-lifetime proof) remains a follow-up, same as on
  the C backend — deep-copy-out is the v1 escape on both.
- Per-node RC-count assertion inside the region is verified via `nm` (arena
  symbols present ⇒ ctors bypass `kaix_variant`/RC); a finer RC-traffic counter
  fixture could pin it more tightly if a future regression is suspected.
