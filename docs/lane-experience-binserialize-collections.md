# Lane retro — BinSerialize collections (pre-blocker for #452)

## Branch / scope

Branch: `lane-binserialize-collections` (worktree path still named
`kaikai.lane-phase-a0-stdlib-cache` from the original briefing — the
branch was renamed mid-lane).

Original brief: implement Phase A.0 of #452 (precompile stdlib
preludes into `.kab` cache files). Mid-lane discovery forced a hard
pivot to a pre-blocker: extend `#derive(BinSerialize)` so it covers
collections and `Char`, which the AST types the cache lane needs to
serialise depend on. Phase A.0 proper is the next lane.

Closes: pre-blocker for #452. No issue filed for the pre-blocker —
filed in this retro instead.

## Scope as planned

The brief assumed:

- PR #471 had landed `BinSerialize` *and* the derive sites for the
  AST tree.
- `bin/kai` driver + content-addressable `.kab` files + atomic writes
  + selfhost byte-identical was 2–3 days of work.
- The compiler did not need any non-driver changes.

All three assumptions were wrong. The first three hours of the lane
were spent verifying the assumptions and falsifying them.

## Scope as shipped

1. New combinator functions in `stdlib/protocols.kai`:
   - `bin_char_to_bytes`, `bin_char_from_bytes` (4-byte LE).
   - `bin_list_to_bytes[T]`, `bin_list_from_bytes[T]` (4-byte length
     prefix + element-wise encode/decode via closure).
   - `bin_option_to_bytes[T]`, `bin_option_from_bytes[T]` (1-byte
     tag + payload via closure).
2. Extensions to `derive_binserialize_impl` in `stage2/compiler.kai`:
   - `derive_binser_encode_expr_at` / `derive_binser_decode_call_at`
     inspect the field's `TypeExpr` and emit combinator calls with
     inline closures for `TyList(elem)` / `TyName("Option", [elem])`
     / `TyName("Char", _)`.
   - `derive_binser_encode_mangled` parallel to the existing
     `derive_binser_decode_mangled` so non-collection fields route
     to the per-type symbol directly. Critical for closure bodies
     where the monomorpher cannot propagate type info into the
     dispatcher.
   - Field-index and variant-index column offsets so multiple
     lambdas synthesised at the same source position get distinct
     `(line, col)` keys for the lambda lifter.
3. Validator update (`validate_derive_binserialize`) to recursively
   unwrap `List` / `Option` and report the inner element type as the
   missing impl, matching the user's mental model.
4. Six fixtures under `examples/stdlib/`:
   - `binserialize_derive_list.kai` — record with `[Int]`.
   - `binserialize_derive_option.kai` — record with `Option[Int]` /
     `Option[String]`.
   - `binserialize_derive_char.kai` — record with `Char`.
   - `binserialize_derive_nested.kai` — `[[Int]]` and
     `Option[[String]]`.
   - `binserialize_derive_sum_collections.kai` — sum with `[Int]`
     and `(Option[Int], [String])` variant payloads.
   - `binserialize_derive_list_of_unknown.kai` + `.err.expected` —
     negative case for `[Color]` where `Color` has no impl.
5. Design doc: `docs/binserialize-collections-design.md`.

## Design decisions

### Why combinators, not impls

The single-dispatch protocol resolves on the head type
(`proto_type_name`). `proto_type_name(TyList(_))` is `"List"`, and
`proto_type_name(TyName("Option", _))` is `"Option"`. Both the buffer
(`[Byte]` = head `"List"`) and any payload list (`[T]`) collapse to
the same dispatch slot. A global `impl BinSerialize for List` is not
viable:

- `from_bytes(buf, pos) : Result[String, BinCursor[Self]]` does not
  carry the element type `T`. There is nowhere to thread the
  per-element decoder through the protocol surface.
- The buffer and the payload would compete for the same dispatch
  entry; the runtime cannot tell them apart.

The combinator approach — free functions that take the element
encoder/decoder as a closure argument — sidesteps the dispatcher
entirely. The derive knows the element type at compile time and
emits a closure literal `{ x -> <encode element> }` inline. No new
protocol surface. No churn on existing impls. ~80 LOC of stdlib +
~250 LOC of derive extension.

### Why a per-type mangled call inside closures

The first draft kept `to_bytes(self.field)` as the encoder body
even when the field type was atomic. The post-typing AST showed the
right call shape, but the monomorpher emitted `__proto_to_bytes`
(the runtime dispatcher that panics) rather than the per-type
mangled name. Inside a lambda body, the monomorpher does not
propagate the lambda parameter's instantiated type back into the
free-call resolution table — the closure stays generic.

Fix: the derive now calls `proto_mangled_fn` directly for every
non-collection / non-Char field, producing
`__pimpl_BinSerialize_<Head>_to_bytes(self.field)`. Mirrors the
existing decoder behaviour. Closure bodies and top-level encoder
bodies both resolve statically.

### Why per-(field, depth, direction) column offsets

The lambda lifter (`find_lam`, `stage2/compiler.kai:13359`) keys
each lambda by `(line, col)`. Synthesised lambdas at the derive
site have no real source position, so the derive originally stamped
all of them with the type declaration's `(line, col)`. Result: the
lifter found the same `LamInfo` for distinct lambdas, two call
sites referenced one C symbol, and the emitted code passed a
2-argument decoder into a position expecting a 1-argument encoder
— bus error at runtime.

Fix layers:

- Per field index in records: each field gets a 16-column slot
  (`col + idx * 16`).
- Per variant index in sums: 256-column window per variant.
- Per direction: encoder lambdas use `+1` offset within the slot,
  decoder lambdas use `+5`.
- Per nesting depth: each level adds `depth * 8` so nested
  encoder/decoder lambdas in `[[Int]]` / `Option[[String]]` do not
  collide either with the outer level or with each other.

The numbers are chosen so encoder and decoder offsets at any depth
remain disjoint: encoder slots `{1, 9, 17, …}`, decoder slots
`{5, 13, 21, …}`. The lifter is unchanged.

### Alternatives rejected

- **Single global tag for collection element types.** Would require
  global mutable state for the registry and would not solve the
  "from_bytes takes no `T`" issue.
- **Newtype `ByteVec` wrapping `[Byte]`.** Disambiguates buffer from
  payload but forces every existing helper (`bin_byte_at`,
  `bin_have`, and the protocol surface) to switch types. The
  `u8 → Byte` rename (#476) already absorbed that level of churn
  once; another cascade for `ByteVec` was rejected as net-negative.
- **Cursor-aware protocol redesign.** Adding a third parameter to
  `from_bytes` for the element decoder breaks the protocol's
  `Self`-only signature contract, breaks every existing impl, and
  contradicts the "single-dispatch protocols with O(1) impl-table
  lookup" principle. Rejected.
- **Hand-written serialisers for every AST node.** ~1500–3000 LOC
  in `stage2/compiler.kai`, with no way to keep them aligned with
  AST evolution. The derive infrastructure is the correct vehicle.

## Structural surprises

The brief said "no compiler changes — cache is driver-side." Three
shapes of surprise made that wrong:

1. **`#derive(BinSerialize)` only existed for primitive-only records
   and sums.** No collections, no Option, no Char. The derive
   validator (`binser_builtin_impl`) was a five-element list. The
   AST tree has ~30 types and every one of them has lists or
   options somewhere.
2. **The dispatcher resolves on head type only.** `proto_type_name`
   collapses `[Int]` and `[Byte]` to `"List"`. The buffer and any
   payload share a dispatch slot. The derive's own internal note
   (`stage2/compiler.kai:46394-46398` in the pre-lane code) called
   this out explicitly: *"impl BinSerialize for List does not exist
   and never should"*. The cache lane could not avoid touching this.
3. **The lambda lifter de-dupes by `(line, col)`.** Two distinct
   lambdas at the same source position are treated as one. Not a
   bug for hand-written code (lambdas have distinct columns). For
   synthesised code, the derive has to assign unique positions
   itself. The column-offset arithmetic is ad-hoc but tractable.

The post-typing AST dump (`--dump-typed`) was correct throughout —
two distinct `lambda : (Int) -> [Byte]` nodes at the right
positions in the tree. The C output was wrong. That gap is what
made the bug confusing to track down: the typer saw two functions,
the lifter saw one.

## Fixtures + coverage

Tier 0 passed locally on first try after the column-offset fix.
Tier 1 was run before the PR push (see commit log).

Coverage:

- Positive: `[Int]`, `[String]`, `Option[Int]`, `Option[String]`,
  `Char`, nested `[[Int]]`, `Option[[String]]`, sum with `[T]` and
  `Option[T]` payloads.
- Negative: `[Color]` where `Color` has no impl. Validator names
  `Color` as the missing provider, not `List`.

Coverage gap:

- No fixture exercises `Option[Char]` or `[Char]` directly. The
  combinator code paths are exercised transitively (nested call
  the same `derive_binser_encode_expr_at` recursion) but a direct
  smoke test would catch a future regression in the `Char` axis
  faster.

## Cost vs. estimate

Brief estimate: 2–3 days for full Phase A.0.

Reality:

- Hour 0–1: Read the design doc, brief, and current state of #471.
- Hour 1–2: POC verification — `#derive(BinSerialize)` fails on
  `Option[T]` and `[T]`; design doc explicitly flags this as
  pre-blocker; user re-scoped to "extend BinSerialize first".
- Hour 2–3: Design doc for the combinator approach.
- Hour 3–5: Stdlib helpers (`bin_char_*`, `bin_list_*`,
  `bin_option_*`), POC with hand-rolled calls confirms they work.
- Hour 5–7: Derive extension (encode/decode emit + validator
  update). First end-to-end test (`[Int]` field) crashes at
  runtime.
- Hour 7–8: Debug session — the typed AST showed correct code, the
  C output passed wrong lambda. Discovered the `(line, col)`
  lifter key collision.
- Hour 8–9: Column-offset arithmetic + depth tracking. All
  positive fixtures pass.
- Hour 9–10: Validator recursion + negative fixture + golden
  outputs.
- Hour 10–11: tier0 (passed first try, selfhost byte-identical) +
  tier1 + retro + PR.

Roughly one full day for the pre-blocker. Phase A.0 proper (the
original lane scope) still needs ~5–7 days on top.

## Follow-ups

For the next lane (Phase A.0 — content-addressable stdlib cache,
#452):

- Apply `#derive(BinSerialize)` to the AST type tree (`Decl`,
  `Expr`, `Pattern`, `TypeExpr`, `Stmt`, `Variant`, `FieldDecl`,
  `FieldInit`, `Param`, `RowExpr`, `RowLabel`, `Arm`, `ListElem`,
  `PField`, `HClause`, `HReturn`, `UnitExprT`, `TypeBody`,
  `ValueMode`, `ExprKind`, `PatKind`, `TyKind`, `Stmt` variants
  with embedded positions). Each derive site is a one-line
  annotation now that the combinators cover collections.
- Wire `bin/kai` to compute `sha256(source) + kaikai_version_hash`
  per prelude, look up `~/.cache/kaikai/preludes-v<N>/<sha>.kab`,
  hit → deserialise; miss → parse + serialise + atomic write
  (tmp + fsync + rename).
- Wall-time gate: ~1.47 s → ~1.24 s on warm second build of
  `examples/minimal/empty.kai` (target from
  `docs/cache-design.md`). If the saved time falls short, profile
  before tuning serialiser shape.
- Selfhost byte-identical remains the load-bearing gate. The cache
  is additive — the no-cache path must produce identical kaic2
  output bytes.

Out of scope for this lane and the next one alike:

- Optimisation of `bin_list_to_bytes` (currently `list_append`-heavy
  with quadratic complexity for the encoder). Acceptable for v1;
  the cache lane writes once per stdlib edit and reads on every
  build, so the decoder hot path matters more.
- `BinSerialize for [T]` as an actual protocol impl. Out of scope
  until annotation-aware single-dispatch or an explicit cursor-
  carrying protocol redesign lands.

## What I'd do differently

- Verify the precursor lane's actual surface before trusting a
  brief that says "consume it as-is." The brief and the design doc
  contradicted each other on whether the pre-blocker had shipped;
  the briefing won on optimism, the design doc won on fact. Should
  have re-read both before estimating.
- The column-offset trick is functional but hacky. A cleaner fix
  would be to give the lambda lifter a stable monotonic counter
  that the derive can request. Filed mentally as a follow-up; not
  worth blocking this lane.
- The lane's branch name (`lane-phase-a0-stdlib-cache`) does not
  match what shipped (`lane-binserialize-collections`). The rename
  happened mid-lane; rebrief style would have caught the mismatch
  earlier.
