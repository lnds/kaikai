# Lane experience — native i64-inline variant slots (closes #1083)

## Scope as planned vs as shipped

**Planned (brief):** the native backend stored variant `Int` slots BOXED while the
C backend stored them raw `i64` INLINE. Make native mirror C, reusing the existing
`variant_slot_mask`/`variant_slot_kind` machinery — "copy, don't invent". Four
pieces: (1) pass the real mask in construction, (2) write raw `i64` instead of
boxed, (3) read `i64` in `KProj`, (4) drop skips `i64`.

**Shipped:** three of four — construction (raw write), mask registration, and drop
(skip i64) are live; the **raw read (piece 3) is deferred**. The layout is
i64-inline like C (`--emit=kir` shows `i64:` inits + `m<mask>` on `con`/`trmc-step`)
and the READ stays boxed via `kaix_variant_arg`, which consults the tag's
registered mask and re-boxes a raw i64 slot correctly (`kai_variant_slot_box`). So
construction/drop are i64-inline (one box per read, not per read+write). Byte-id C
self-host + rb-tree 1M `height:29` on both backends. Plus four structural surprises
the brief did not anticipate (see below), of which the raw read is the one that
did not make it.

**Why the raw read is deferred.** Reading a kind-1 slot raw (`SInt64` binder →
`kaix_variant_arg_i64`) passed rb-tree byte-id, C-selfhost byte-id, tier1-asan, and
rc-detector — but **regressed the native self-host gate** (SIGSEGV, `address=0x10` =
a slot Int read as a pointer and dereferenced). It reproduced only when the
native-built compiler compiles ITSELF (thousands of AST/KIR variants with Int
slots), not on the rb-tree. A `SInt64` binder propagated to a consumer the emitter's
boxed↔raw border did not cover. Isolated by bisection: forcing the read binder back
to `SBoxed` (build i64, read boxed) makes the self-host gate green again. The raw
read is a follow-up once that propagation path is audited; the construction/mask/
drop half — the actual #1083 layout — ships now.

## The four pieces, and where each landed

- **Construction (raw write).** `lower_ctor_call` was rewritten to `lower_ctor_args`:
  each arg lowers AT ITS SLOT — a kind-1 Int slot via `lower_expr_raw_at(SInt64)`
  (no trailing `int.box`), else boxed. This mirrors `lower_tcrec_args`. The KIR
  emits `SIInt64(v)` for Int slots. The `KConReuse` sites (5 of them) and the TRMC
  spine step were switched the same way.
- **Read (raw read).** `bind_slot_pattern` now types a match-arm `PBind` binder
  `SInt64` when the ctor's slot is kind-1 (`bind_slot_kslot`), and the native
  `KProj` at an `SInt64` slot emits `kaix_variant_arg_i64` (`.i64`) instead of the
  boxed `kaix_variant_arg`. The emitter's existing bidirectional border
  (`nemit_load_reg_boxed` re-boxes an `SInt64` register at a boxed use) makes this
  safe without touching consumers.
- **Drop skips i64.** The generic drop walker already reads `kai_slot_mask_of(tag)`
  and skips non-PTR slots — registering the real mask at startup (tarea C) is what
  makes it skip the Int slots. No drop-walker change.
- **Mask registration.** `nproto_register_payload_ctors` stamps the real
  `variant_i64_mask` (Int-only) at startup, eager, before any construction.

## Structural surprises the brief did not anticipate

1. **No `ptrtoint` prim existed.** The `[n x i64]` word buffer needs a boxed
   `KaiValue *` slot as one word (`ptrtoint(ptr)`), but the emitter exposed only
   `sext`/`zext`/`trunc`/`fpcast`. Had to add `llvm_build_ptrtoint`/`inttoptr` as
   new prims in FOUR places (stage2/runtime.h real+stub, native_prims.kai,
   stage1/compiler.kai, runtime_llvm_native_shim.c) — the "new prim needs stage1
   rebuild" trap. Low risk, mechanical.

2. **kaic1 handle-RC UAF — the time sink.** An LLVM `Value*` handle that is the
   result of an `ncall` (e.g. `kaix_int_field`) is on kaic1's RC regime as an owned
   local. Pushed into a C-opaque `llvm_buf` and read back later, kaic1 decref's it
   when its producing frame returns (the buffer is invisible to the RC), so the
   later read hits freed memory (ASAN: BUS on `0xbe...` poison). `nemit_atom` (a
   `llvm_build_load`/const, OFF the RC regime) survives the same buffer, which is
   why the boxed path never hit this. **Fix: store-immediate.** Materialise each
   slot word AND store it into the pre-allocated `[n x i64]` in the same statement
   (`nemit_store_slot_words`), so no ncall-result handle ever waits in a buffer
   across a frame return. Applied to all three construction routes.

3. **The 5th mask arg widened a reused CallInst → `<Invalid operator>`.** The TRMC
   spine step reuses the node CallInst handle twice (`slot_addr` + `extend_and_loop`).
   kaic1's RC pass dup's a reused handle and scribbles the CallInst. The 5-arg
   masked ctor made this worse. **Fix (asu-guided):** the mask is a property of the
   TAG, not the call-site — the runtime reads `kai_slot_mask_of(tag)` internally, so
   ALL THREE constructors (`kaix_variant_masked`, `kaix_variant_reuse_at_i64`,
   `kaix_variant_at_argv_i64`) dropped the mask arg. This is arch-cleaner (one mask
   source) AND narrows the CallInst. It was NOT enough on its own for TRMC: the
   node still had to be **spilled to an alloca** and reloaded per use, because the
   store-immediate build left other live handles in the caller frame that made
   kaic1 dup the reused node. Spill+reload gives each use a fresh off-RC load.

## The 3-routes-or-none invariant

The slot mask is per-TAG (a global table), not per-cell. A tag's cells cannot mix
word kinds. So all three routes that build a given ctor (fresh `KCon`, reuse
`KConReuse`, TRMC spine) had to lay i64-inline consistently, or none. Two runtime
forwarders were added (`kaix_variant_reuse_at_i64`, `kaix_variant_at_argv_i64`)
that thin-forward over the deep `kai_variant_reuse_at`/`kai_variant_at` (which
already accept a mask). RBNode reuse and TRMC now write the same `.i64` words a
fresh cell does — the runtime comment on `kaix_variant_at_masked` documents the
exact corruption (rb-tree height diverging) this invariant prevents.

## Scope kept narrow: kind-1 Int only

`variant_i64_mask` marks ONLY kind-1 Int slots raw; Real (kind-2, would need a
`bitcast f64`) and enum (kind-3, would need the tag-immediate emit) stay boxed in
the native i64 layout. RBNode is Int-only, so #1083 is fully covered; Real/enum
i64-inline is a follow-up if a bench demands it.

## Fixtures

The rb-tree bench (`examples/perceus/rb_tree_bench.kai`) exercises all three routes
with Int slots and is the byte-id + perf oracle. The `--emit=kir` goldens
(`examples/kir/control_flow.kir.expected`) were re-baselined to show the new `i64:`
inits + `m<mask>` — the legitimate KIR shape change. Existing perceus-reuse
fixtures (`test-perceus-1053-*`, `test-perceus-872-*`, `test-perceus-882-*`) cover
the reuse routes under ASAN in CI.

## Perf

Instructions dropped 6.31B → 6.01B (rb-tree 1M); wall ~1.32×→~1.25× vs C-backend.
The layout is now i64-inline identical to C. The residual gap is RC traffic
(dup/drop, comparison unboxing), documented as orthogonal to codegen — the
i64-inline lever is spent; closing the rest is a drop-spec / flat-layout lane.
"wall ≤ C" (1.0×) is not reachable by i64-inline alone.

## Follow-ups

- Real/enum i64-inline (kind-2/kind-3) if a bench needs it — needs `bitcast f64`
  and the enum tag-immediate emit.
- The TRMC node spill is a correctness necessity, not free; a kaic1 that did not
  dup reused handles would let it be dropped.
- The residual RC traffic (~2.1×) is the next perf lever, orthogonal to this lane.
