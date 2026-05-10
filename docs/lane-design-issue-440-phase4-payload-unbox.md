# Lane design — issue #440 (Phase 4 Option A: variant payload unboxing)

Status: **design pinned 2026-05-10 by Linus + Eric joint review**. The
pre-implementation gate this doc was drafted as has been resolved.
Implementation is unblocked under the decisions recorded in §
*Decision* below.

The brief on #440 frames the lane as a 10–12-day codegen change. After
mapping the runtime + emitter surface (stage2/compiler.kai = 50 387
lines, runtime in stage0/runtime.h), this doc surfaced a runtime
dispatch problem the brief did not address and put three coexistence
strategies to Linus + Eric for arbitration. The decision pin is
recorded below.

## What the issue asks for

> Inline primitive payload slots (`Int`, `Bool`, `Char`, `Real`) into
> variant cell structs. Subtree pointers stay `KaiValue *`. RB-tree
> drops from 16× C to ≤ 10× C. ≥ 50 % RC traffic reduction.

Acceptance numbers are validated by
`docs/benchmarks/rb_tree_breakdown_2026-05-09.md` (post-#439): RB-tree
wall is split alloc 32 % / free 37 % / RC 21 % / other 10 %. Phase 4
attacks all three.

## Today's runtime layout

`KaiValue.as.var` is uniform across every variant in the running
program:

```c
struct {
    int32_t      variant_tag;     /* index into the sum type */
    const char  *variant_name;    /* static string */
    int32_t      n_args;
    KaiValue   **args;            /* array of n_args boxed children */
} var;
```

Codegen entrypoints today (file:line in `stage2/compiler.kai`):

| Concern | Site | Shape |
| --- | --- | --- |
| Construct | `emit_variant_call` (11779) | `kai_variant(0, "Tag", n, (KaiValue*[]){...})` |
| Pattern test | `emit_pat_test_variant` (12533) | `scr->tag == KAI_VARIANT && strcmp(scr->as.var.variant_name, "Tag") == 0` |
| Pattern bind | `emit_pat_binds_variant` (12668) | `scr->as.var.args[i]` per slot |
| Free cascade | `kai_free_value`, `runtime.h` 1626 | `for (i ; i < n_args ; ++i) kai_decref(args[i])` |
| Equality | `kai_op_eq`, `runtime.h` 2418 | walk `args[]` recursively |
| Show | `kai_show_*`, `runtime.h` 2481 | walk `args[]` |
| Reuse-in-place v1 | `kai_reuse_or_alloc_variant`, `runtime.h` ~2244 | overwrite `args[i]` in place |
| LLVM emit | `llvm_emit_variant_ctor` (40289) | mirrors C path |

Both the **constructor** and every **consumer** dispatches by
`variant_name` (string compare!). There is no compiler-side global table
of variants, no per-variant runtime layout descriptor, no per-tag
free/eq/show vtable — the runtime is intentionally polymorphic by string.

## What "inline primitive into variant cell" actually requires

Three layers, each with cost:

### Layer A — emitter generates new layout

For each variant constructor whose payload mixes primitive + pointer
fields, emit:

- A C struct definition (`struct kai_variant_Node { KaiHeader; int8_t color; KaiValue *l; int64_t k; int64_t v; KaiValue *r; }`).
- A constructor `kai_variant_alloc_Node(int8_t, KaiValue*, int64_t, int64_t, KaiValue*)`.
- Per-slot extractors (or a layout descriptor — see Layer B).

This is the easy half. ~500–800 LOC in the emitter with careful naming
to disambiguate variants whose tag names collide across different sum
types.

### Layer B — runtime must know how to walk an unboxed cell

`kai_free_value`, `kai_op_eq`, `kai_op_show`, GC tracing,
`kai_reuse_or_alloc_variant` all currently iterate `args[]` of
`KaiValue *`. Once a variant cell holds raw `int64_t` interleaved with
`KaiValue *`, those loops stop type-checking and can't dispatch by
slot.

**Three coexistence strategies, in increasing emitter complexity but
decreasing runtime complexity:**

#### Strategy 1 — Two KaiValue tag flavors (A1 in lane brief)

Add `KAI_VARIANT_INLINE` next to `KAI_VARIANT`. Every consumer in
runtime gets a duplicated arm. Old (all-pointer) variants keep
emitting `KAI_VARIANT`; new (mixed-payload) emit `KAI_VARIANT_INLINE`.

- **Runtime cost**: each of `free_value`/`op_eq`/`op_show`/`reuse`/GC
  gets a parallel arm. ~6 functions × ~30 LOC each = ~180 LOC of
  duplicated runtime, plus instrumentation (KAI_TRACE_RC, profile_rc)
  needs to count both bands.
- **Emitter cost**: lowest — emitter just picks the alloc/extract
  helper based on whether the variant has primitive slots.
- **Bootstrap risk**: stage 0/1 only emit `KAI_VARIANT` for their own
  internal variants (Result/Option/etc. live in `runtime.h` directly
  as `kai_variant` calls); zero collision with the new tag. Safest.
- **Reuse-in-place coordination**: `kai_reuse_or_alloc_variant` only
  fires on `KAI_VARIANT` cells; extending to `KAI_VARIANT_INLINE`
  means a per-layout reuse helper (or a per-tag dispatch). Out of
  scope for this lane per the brief (#118 + #209 are layout-aware on
  old layout).

#### Strategy 2 — Layout descriptor on a single tag (A2)

Keep `KAI_VARIANT`. Augment the union member to carry a pointer to a
static-emitted `KaiVariantLayout` descriptor:

```c
typedef struct {
    int32_t       n_slots;
    const char   *tag_name;        /* still string-dispatched */
    /* per-slot offset + kind (primitive Int/Bool/Char/Real, or pointer) */
    const struct { uint32_t offset; uint8_t kind; } *slots;
} KaiVariantLayout;
```

Cell payload becomes a `void *raw` blob whose size + interpretation is
determined by `layout`. Runtime walks via the descriptor.

- **Runtime cost**: one set of arms. ~80 LOC of new descriptor logic
  plus rewrites of the 6 consumers.
- **Emitter cost**: must emit static-storage descriptor for each
  variant: ~10–20 LOC per variant + a 30 LOC layout-inference pass.
- **Bootstrap risk**: `kai_variant()` legacy entrypoint must keep
  working for stage 0/1 internal use. Adding a layout pointer to the
  union means the legacy path uses a sentinel `&kai_layout_uniform`
  descriptor.
- **Cleanest long-term**: future Phase-4 extensions (drop spec, reuse
  with new layout) all hang off the descriptor naturally.

#### Strategy 3 — Per-variant C struct cast from KaiValue * (A3, brief's implicit assumption)

`kai_variant_Node` is a C struct whose layout starts with a `KaiHeader`
matching `KaiValue` so it can be cast back. This is C aliasing through
a common prefix — legal in C99 §6.5.2.3.

**Problem**: `kai_free_value` is called with a `KaiValue *` of unknown
underlying struct. Without per-tag dispatch (which doesn't exist —
runtime dispatches by `variant_name` string), free can't tell whether
to walk 5 children or 2 + raw scalars.

To make A3 work you'd need either: (a) a new `tag` discriminator that
includes the layout id (basically Strategy 1 with worse ergonomics),
or (b) a per-name string-keyed vtable lookup at every free site
(strcmp on the hot path — exactly what `KAI_TRACE_RC` already
identifies as expensive).

**Strategy 3 is what the brief textually proposes but the runtime
dispatch problem is unaddressed.** Implementing A3 as written would
need additional design that the brief does not include.

### Layer C — RC discipline selective on pointer fields

Perceus passes (`pcs_rewrite_estr_span`, `__perceus_dup`/`__perceus_drop`
insertion) currently treat every variant arg as a boxed value worth
ref-counting. After Phase 4 they need to:

- Suppress dup/drop for binds typed `Int`/`Bool`/`Char`/`Real`.
- Continue dup/drop for binds typed pointer.
- Per-variant `drop_<Tag>` walks only the pointer slots.

This is contained — Perceus already classifies binds; the new check
is "is this slot's type primitive?" The classifier needs the
typed-AST info to be threaded into `pcs_rewrite_*` (it largely is,
post-#383 unboxing).

## Decision (pinned 2026-05-10)

Reviewed by Linus + Eric (joint, 2026-05-10). Both converged
independently on the same answer.

**Strategy 2** — single `KAI_VARIANT` tag, augmented with a
`const KaiVariantLayout *layout` descriptor pointer in the `as.var`
union. Static-emitted layout tables per concrete variant. Legacy
`kai_variant()` calls write a sentinel `&kai_layout_uniform` so stage
0/1 internal cells continue to be walked correctly without an emitter
change.

**Three sub-PRs**, in order:

1. **Sub-PR 1 (~3–4 days)** — runtime descriptor + sentinel.
   No emitter changes. ASAN-clean, selfhost byte-identical.
   **Auto-merge OFF** (Linus: "this is a runtime ABI change, someone
   should read it").
2. **Sub-PR 2 (~4–5 days)** — emitter generates per-variant struct +
   layout descriptor. Switches construction + pat-test + pat-bind
   sites for variants whose payload has ≥ 1 primitive field.
   Auto-merge eligible after CI green.
3. **Sub-PR 3 (~3 days)** — Perceus selective RC + benchmarks +
   fixtures + doc updates. Closes #440. Auto-merge eligible.

**Strategy 1 is rejected.** Eric: "leaky abstraction — promotes a
codegen detail (are all fields boxed?) into a first-class type-tag
distinction." Linus: "permanently bifurcating the runtime; every
future lane that touches `kai_free_value`/`kai_op_eq`/`kai_op_show`/
`kai_reuse_or_alloc_variant` reasons about two arms forever. Surface
area is O(features × consumers) vs Strategy 2's O(features +
consumers)."

**Strategy 3 is structurally dead.** The brief's textual proposal
(per-variant C struct cast from `KaiValue *`) leaves the runtime
dispatch problem unresolved; there is no Option D hiding in the
design space. Salvaging A3 collapses to either Strategy 1 (tag
discriminator) or a string-keyed runtime vtable (strcmp on the free
path — exactly what KAI_TRACE_RC says is expensive).

### Critical risks the design doc missed (raised in review)

**Risk 1 — cons-list regression (Linus).** The descriptor walk adds a
per-slot branch that does not exist today. Cons-list free path is
~33 ns/call (vs RB-tree's ~107 ns/call); a 5–10 ns overhead per slot
on 3 M frees is a 15–30 ms regression on a 73 ms baseline (20–40 %).
The cons-list target is ≤ 1.5× C and the lane must not regress it.

**Mitigation (mandatory in Sub-PR 1)**: inline the uniform-layout
fast path. `kai_free_value` checks `layout == &kai_layout_uniform`
first and falls into the existing `args[]` loop directly. Descriptor
walk runs only for non-uniform layouts. **Acceptance check**:
measure cons-list after Sub-PR 1 lands (no emitter change yet) —
must be byte-identical behavior and ≤ 5 % wall regression. If not,
the fast path is broken.

**Risk 2 — stage 1 ABI rebuild (Eric).** Stage 1 produces C that is
compiled and linked against the *current* `runtime.h`. After Sub-PR 1
the `as.var` union has a new field. If `make tier0` rebuilds stage 1
binaries against the new header, no problem. If it relies on a cached
stage 1 binary linked against the old header, the union layout
divergence is undefined behavior.

**Mitigation (mandatory in Sub-PR 1)**: verify `make tier0` rebuilds
stage 1 against the new header as part of its CI flow. If it does
not, add an explicit `clean stage1` step before the rebuild. The
selfhost gate must compile the entire `stage0 → stage1 → stage2`
chain against the new runtime, not link cached binaries.

## Recommended scope re-shape

The brief assumes Strategy 3 implicitly and frames the lane as a
single 10–12-day push. Based on this exploration **the lane should
ship in three sub-PRs**:

### Sub-PR 1 — runtime descriptor + KAI_VARIANT_INLINE (or layout pointer)

- Pick Strategy 1 or 2 explicitly, write to `docs/effects-impl.md`
  (or new `docs/variant-layout.md`) as a pinned decision.
- Land the runtime change that lets a variant cell carry a layout
  descriptor without changing any emitter call site (legacy
  `kai_variant()` calls go through the uniform descriptor).
- ASAN clean, selfhost byte-identical (no emitter change).
- ~3–4 days.

### Sub-PR 2 — emitter generates per-variant struct + layout descriptor

- Layout-decision rule: variant has ≥ 1 PRIMITIVE field → new layout.
- Emit struct, alloc, extractors, layout descriptor.
- Switch construction + pat-test + pat-bind sites to the new helpers
  for variants with the new layout.
- Selfhost byte-identical (no stdlib variant in stage1 has primitive
  fields — verify this empirically: `Result[T,E]`, `Option[T]`,
  `LamInfo`, `EFn`, `Var`, `TypeBody` etc. are all-pointer payloads;
  if anything has a primitive field the bootstrap chain needs care).
- ~4–5 days.

### Sub-PR 3 — Perceus selective RC + benchmark + close

- `pcs_rewrite_*` skips dup/drop on primitive-typed slots.
- New fixtures `examples/perceus/phase4_unbox_payload.kai` +
  `phase4_payload_rc.kai`.
- Re-measure RB-tree (target ≤ 10× C, hopefully ≤ 2× C per breakdown
  prediction). Cons-list ≤ 1.5× C. Compute ≤ 2× C.
- Update `docs/perceus-honesty-targets.md`, write
  `docs/benchmarks/rb_tree_post_phase4_2026-05-XX.md`.
- ~3 days.

## Bootstrap-chain reality check

The brief says "Stage 1 / 0 do NOT need to handle the new layout in
their own emitters". This is **half-true**: stage 0 (C handwritten)
and stage 1 (kai → C) emit `KAI_VARIANT` cells via `kai_variant(...)`
calls, and those cells are consumed by code that stage 2 also emits.
As long as `kai_variant()` continues to produce a uniform-layout cell
that the *new* runtime can still walk (e.g. via a `&kai_layout_uniform`
descriptor in Strategy 2, or by keeping `KAI_VARIANT` as the legacy
tag in Strategy 1), bootstrap is safe. Strategy 3 fails this without
extra runtime work.

stdlib variants (`Result[T,E]`, `Option[T]`, `Map[K,V]`, etc.) are
parametric over type variables — type-variable slots are POINTER per
the layout rule — so they keep emitting all-pointer payloads. **The
new layout fires only on monomorphised variants whose ground types
are primitive**. RB-tree's `RBNode(Color, RBTree, Int, Int, RBTree)`
is the canonical case.

## What this lane should NOT include

The brief's exclusions stand:

- No region brands.
- No cross-fiber unboxed messages.
- No drop specialisation.
- No reuse-in-place coordination with new layout (#118 + #209 stay
  on old layout).

Add to that exclusion list, based on this exploration:

- **No type-variable monomorphisation** — `Map[Int, Int]` does NOT
  trigger a new layout for the `Map` variant cells. A specialiser
  could do this; it's a separate lane.
- **No drop spec on the new layout** — even if the lane wanted to,
  per-tag inlined decref chains coordinate with both reuse-in-place
  and the new layout. That's a 3-way intersection that should land
  one direction at a time.

## Open questions resolved by the 2026-05-10 review

1. ~~Strategy 1, 2, or 3?~~ → **Strategy 2** (Linus + Eric).
2. ~~Sub-PR cadence?~~ → **Three sub-PRs**, in the order above.
3. ~~Auto-merge scope?~~ → **OFF for Sub-PR 1** (runtime ABI),
   **ON for Sub-PRs 2 and 3** after CI + ASAN green.
4. **Stage 1 mirroring** — still open as a long-term followup. Not
   blocking for #440. Stage 1 keeps emitting old layout via the
   sentinel descriptor; mirroring becomes worthwhile once Phase 4
   numbers prove the approach and a separate lane decides whether
   the bootstrap chain itself benefits from the new layout. Tracked
   as a followup, not a #440 deliverable.

## What this session did NOT do

- No code changes. Working tree clean.
- No fixtures. No benchmarks.
- No commit on the branch other than this design doc.

The branch (`issue-440-phase4-variant-unbox-payload`) is rebased onto
`origin/main` and ready for the next session to pick up **Sub-PR 1**:
runtime descriptor + `&kai_layout_uniform` sentinel, with the
mandatory cons-list and stage-1-rebuild checks above.
