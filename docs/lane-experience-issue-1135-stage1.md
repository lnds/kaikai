# Lane experience — issue #1135 stage 1: Vec[T] representation + runtime ops

Advances #1135 (does NOT close it — this is the first stage of an
explicitly multi-lane plan). Scope of this lane: the runtime
representation, the pure-value op set, uniqueness in-place / CoW, and
unboxed monomorphized storage (the #1121 deliverable, executed here).

## Scope as planned vs as shipped

Planned (the stage-1 brief): flat-buffer representation with one RC
header; unboxed storage for `Int`/`Real`/small records with boxed
fallback; `make`/`get`/`set`/`push`/`length`/`map`/`foldl` as pure
ops; rc==1 in-place with CoW on sharing; fixtures for uniqueness,
purity, and storage kinds. All shipped. Additions beyond the letter of
the brief, all small: `vec.empty`/`is_empty`/`from_list`/`to_list`
(needed by fixtures and any realistic use), structural `kai_op_eq` and
`kai_deep_copy_out` arms (soundness sweep obligations of a new tag),
and the `kai info vec` page.

Out of scope, untouched as instructed: literal minting, `[h, ...t]`
seamless slices, fusion integration (#1140), the list-default flip.

## The central design decision — new tag KAI_VEC vs reusing Array machinery

Decided: **new tag `KAI_VEC`, new buffer layout; nothing shared with
`KAI_ARRAY` beyond the node-header shape.** The evidence that forced
it: Array's buffer is `KaiValue **items` — boxed pointers, per-element
heap cells — while Vec's whole point is *unboxed* element storage (raw
scalars, inline records). There is no physical buffer to share; only
the `{len, cap, ptr}` node shape repeats, and that costs nothing to
repeat (the union member is the same 24 bytes). Semantics diverge in
the same direction: Array writes are observable (identity, `Mutable`);
Vec writes are value-pure with the rc==1 check deciding in-place vs
copy. A shared representation would have meant per-element boxes for
Vec — exactly the ~84 B/elem cost this issue exists to remove.

Representation shipped:

- The KaiValue node carries `{int64 len, int64 cap, void *data}` —
  24 B, no union growth, no impact on any other tag.
- `data` is ONE heap block: `[KaiVecMeta][cap * stride bytes]`. The
  96-byte meta prefix (per vector, not per element) records the
  element kind; growth reallocs the one block.
- Element kinds: `RAW` (Int/Real/Bool/Char/Byte as raw 8-byte
  payloads), `REC` (record of ≤ 8 all-scalar fields, inlined at
  `n_fields * 8` B/elem, field names + per-field tags in the meta),
  `BOXED` (sound fallback: `KaiValue *` per slot, RC'd), `PENDING`
  (empty vector; the first push classifies).
- Classification happens at runtime from the first element's shape
  (`kai_vec_classify`), not from the compiler: monomorphization
  guarantees every element of one vector has one static type, so one
  classification is total. This kept the compiler surface to exactly
  three tables (prelude names, type schemes, EP symbol map) — no new
  pass, no unbox_pass interaction, no KIR change.

Uniqueness rides the existing machinery verbatim: `kai_check_unique`
(the Perceus reuse-in-place predicate) composes with the prelude
callee-consumes convention with no new analysis. A last-use caller
moves its ref in → rc == 1 at entry → in-place is unobservable. A
caller that keeps the vector alive was already dup'd by Perceus →
rc > 1 → copy-on-write. The soundness of in-place IS the soundness of
Perceus ownership; no vec-specific compiler logic exists.

Rejected alternatives, for the record:

- *Meta in the node header spare fields* (`variant_tag`/`var_n_args`):
  not enough room for the REC metadata (names, field tags, head tag).
- *Compiler-selected element kind* (monomorph rewrites `vec_make[Int]`
  → a specialized prim): more surface for zero stage-1 gain; runtime
  classification is total under monomorphized types. Revisit only if
  fusion (#1140) wants compile-time-known strides.
- *Separate meta allocation*: one block halves the allocs and keeps
  growth a single realloc.

## Measured — bytes/elem (fill + sum, N = 10M, macOS arm64, -O2)

| carrier                    | wall (C) | wall (native) | RSS      | B/elem |
|----------------------------|----------|---------------|----------|--------|
| `Vec[Point]` (inline REC)  | 0.88 s   | 1.09 s        | 162 MB   | ~16.2  |
| `Vec[Int]` (RAW)           | —        | 0.41 s        | 82 MB    | ~8.2   |
| `Array[Point]` boxed (issue baseline 2026-07-08) | 0.38 s | — | 885 MB | ~84 |

The storage deliverable lands exactly in the issue's "~160 MB class"
target (Rust: 154 MB). Wall on `Vec[Point]` is *slower* than the boxed
Array baseline today: every `vec_get` reconstructs a boxed record
(node + fields + two Int boxes) and every access crosses the prelude
boundary. That cost is the known shape stage 2+ removes (borrow reads,
fusion collecting into the unique buffer, raw projections); stage 1's
claim is the representation and the RC discipline, and those are the
measured 5.5× memory win with counter-verified zero-copy writes.

## Structural surprises

- `stage2/runtime.h` is NOT a copy of `stage0/runtime.h` — it is the
  production runtime with Koka-style tagged-immediate Ints. Every
  element-classification and payload read needed the `kai_is_value` /
  `kai_intf` idiom there, and the plain `->tag` idiom in stage0. The
  vec sections are deliberately parallel but not identical.
- kaic2-emitted C only compiles against stage2/runtime.h (`-I .` comes
  first in `CPPFLAGS_CORE`; the extra `-I ../stage0` in test recipes
  is vestigial for these fixtures). The ASAN validation therefore ran
  against the production runtime.
- The per-tag alloc histogram (`kai_rc_alloc_by_tag[16]`) already
  excludes `KAI_FOREIGN` (tag 16); `KAI_VEC` (17) inherits the same
  defensive skip. The gates use the dedicated `vec_inplace`/`vec_cow`
  counters instead, printed under `KAI_TRACE_RC` next to
  `reuse_in_place`.
- Record field order: the meta resolves incoming record fields BY NAME
  (pointer-compare fast path, strcmp fallback) rather than trusting
  positional order, so a permuted literal cannot silently transpose
  columns; a genuine shape mismatch traps loudly.

## Fixtures added (all wired; native variants only in tier1-native.yml)

- `examples/perceus/vec_unique_inplace_1135.kai` — 100 sets + 50
  pushes on a linearly-threaded vector; gate asserts
  `vec_inplace=150 vec_cow=0` (KAI_TRACE_RC golden) plus exact output.
- `examples/perceus/vec_shared_cow_1135.kai` — a write to a shared
  vector: original read back intact, gate asserts `vec_cow=1`.
- `examples/perceus/vec_unboxed_storage_1135.kai` — RAW Real, inline
  REC, boxed String / Option (shared write leaves original intact),
  and the PENDING empty→push path, round-tripped on both backends.
- `examples/effects/vec_pure_row_1135.kai` — every helper annotated
  with a PURE row; compiling is the purity assertion (the same shapes
  over Array are rejected without `/ Mutable`). Also exercises the
  stdlib module surface (`vec.map`/`foldl`/`from_list`/`to_list`).
- Makefile gates: `test-perceus-1135-vec-value` (C, in
  `TEST_LIGHT_TARGETS`) and `test-perceus-1135-vec-value-native`
  (tier1-native.yml step). ASAN run manually on all fixtures against
  the production runtime: clean.

Coverage gaps left deliberately: no `==` surface test (needs
`impl Eq for Vec`, stage 3 — the runtime structural arm exists), no
slice fixtures (stage 2), no minting fixtures (stage 3).

## Cost vs estimate

The runtime work itself was the planned size (one section per
runtime copy + sweep arms). The unplanned cost was discovering the
two-runtime divergence (tagged Ints) and validating both idioms — a
second pass over every scalar helper. The compiler side was smaller
than briefed: no stage1 rebuild was needed (the compiler bundle never
calls vec prims; kaic1 only needs the header to *compile*, which is
additive), and no unbox/KIR work at all.

## Follow-ups (stage 2+ of #1135, noted not filed — staging decided at launch)

1. **Seamless slices** — `[h, ...t]` as O(1) views (offset + length +
   parent refcount). The meta block is ready for it; the slice form
   needs a decision on whether a slice is a `KAI_VEC` node with a
   parent pointer or a distinct tag.
2. **Literal minting** — `let a: Vec[Int] = [1, 2, 3]` via the #1091
   precedent; bare literal stays cons.
3. **Fusion integration (#1140)** — fused pipe chains collecting into
   a unique pre-sized buffer; this is where the wall-clock win lands.
4. **Borrow reads** — seed `vec_get`/`vec_length` into the Perceus
   bmap with `_borrow` runtime variants (the #1120 array move,
   verbatim); removes the per-access container dup/decref pair.
5. **Protocol impls** — `impl Eq/Show for Vec` (head tag 14 is
   registered; the runtime structural eq arm already exists).
6. **RAW scalar set extension** — stage2's nominal fixed-width tags
   (`KAI_INT32`/`UINT32`/`UINT64`) currently fall to boxed; adding
   them to the scalar set is mechanical once someone needs it.
7. **Show surface** — `#{v}` prints `<vec>` today, like Array; a
   value type deserves element rendering (stage 3 surface decision).
