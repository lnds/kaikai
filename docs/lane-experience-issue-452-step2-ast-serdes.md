# Lane experience report — issue #452 Phase A.0 step 2 (AST record serdes)

Continuation of PR #581 (step 1 — KAB1 header + hex codecs for the
primitive surface). Step 2 ships the AST-record serdes layer that
turns a `[Decl]` post-parse value into a portable hex blob and back.
The cache layer remains dormant at runtime — nothing calls
`cache_serialize_module` from a non-test code path yet. Step 3 will
wire `kaic2 --prelude-cache <path>` and `bin/kai`'s cache hit/miss
shell, which together let an empty-program build actually skip the
~0.41 s lex+parse cost on the 2026-05-14 baseline.

## Objective metrics

- Start: 2026-05-14 (post-v0.57.0 + #581 merge).
- End: ~1 session, conversation-driven.
- LOC added: 2 783 (`stage2/compiler.kai`), 0 deletions of pre-existing
  code (only refactors of step 1's O(n²) string concatenators —
  same external surface, see "Step 1 hot-fix" below).
- Build / test invocations:
  - `make -C stage2 kaic2`: ~8 (incremental build per error fix).
  - `make -C stage2 selfhost`: 3 (after each major edit batch).
  - `make tier0`: 1 (clean run).
  - `make tier1`: 1 (clean run).
  - `stage2/kaic2 --cache-roundtrip-test`: 1.
- Selfhost byte-identical: OK (verified after every edit batch).

## Scope as planned vs as shipped

| Planned (brief) | Shipped |
|---|---|
| `BinSerialize` impls (manual or via `#derive`) for each AST type | Shipped as free-function hex serdes — `#derive(BinSerialize)` is not usable from `stage2/compiler.kai` because the bootstrap path `stage0 → stage1` has no `impl`, `protocol`, `#derive`, or `Byte` (this was the pinned constraint from PR #581) |
| `cache_serialize([Decl]) -> [Byte]` | `cache_serialize_module(decls: [Decl], source_sha: String) -> String` — returns hex-encoded `String`, not `[Byte]`, for the same bootstrap reason |
| `cache_deserialize([Byte]) -> Result[[Decl]]` | `cache_deserialize_module(buf: String) -> Option[CacheDeserialized]` — `Option` instead of `Result` because the deserialiser only ever fails with "cache miss" (every variant collapses to `None`); rebuilding a parse error from inside hex decode is not useful to the caller |
| Round-trip test: serialize → deserialize of a module entero produce el mismo `[Decl]` | Round-trip test serialises a synthetic 4-decl `[Decl]`, deserialises, re-serialises, and compares hex strings byte-identically — `assert eq(decls, restored)` not viable because stage 1 has no `Eq` for AST types |
| Position fidelity: spans deserializados apuntan al source file original | Spans round-trip exactly: each `Expr` / `Pattern` / `TypeExpr` carries `(line, col)` inline, decoded back by `cache_hex_to_expr` / `cache_hex_to_pat` / `cache_hex_to_typeexpr`. The source file path is NOT in per-node payload — it is supplied out-of-band by the cache key (see "Design decisions" §3) |
| Tier 0 + Tier 1 green, selfhost byte-identical | All three OK |

The brief's literal "BinSerialize impls" framing was wrong in detail
but right in spirit: the wire format is conceptually a `BinSerialize`
trace, just written as free functions because the bootstrap chain
cannot host the protocol surface. Future stages (once stage 2 is the
ground truth and stage 1 retires) could collapse this to derived
impls.

## Design decisions

### 1. Step 1 hot-fix: every `*_to_hex` had to be O(n)

Step 1 used `string_concat(acc, chunk)` inside the byte-by-byte loop
of `cache_string_chars_to_hex`, `cache_int_to_hex_loop`, and
`cache_string_list_loop`. That is O(n²) per call — fine for a 4-line
header, broken for a 100 KB module payload (back-of-envelope: 5 MB of
hex × 12 billion char copies = many hours per build).

Fix: refactor each loop to accumulate `[String]` chunks (prepend O(1))
and emit a single `concat_all(list_reverse(acc))` at the end. The
runtime's `kai_string_concat_all_impl` is a two-pass O(n) allocator
(stage0/runtime.h:2555) — perfect for this pattern. External API
unchanged; downstream callers see the same `: String` return type.
Selfhost remains byte-identical after the refactor.

This refactor is bundled into this PR because every step-2 serde would
have been pinned by the same O(n²) problem if shipped on top of the
step-1 surface as-is.

### 2. Sum-type tag bytes start at `0x01`, never `0x00`

Every sum-type encoder emits a one-byte tag (two hex chars) starting
at `0x01`. The decoder rejects unknown tags with `None`. A stray
zero byte in a malformed blob therefore decodes as "unknown variant"
(loud) rather than "tag 0 = first variant" (silent corruption).
`Option[T]` is the one exception: it uses `00`/`01` for None/Some,
following the step 1 convention already shipped.

### 3. Spans carry `(line, col)` inline; the file path is out-of-band

Every `Expr`, `Pattern`, and `TypeExpr` carries `line: Int` and
`col: Int` inline in the wire format. The source file path is
**not** in per-node payload — the cache key (the source SHA) plus the
cache directory layout (`~/.cache/kaikai/preludes-v1/<sha>.kab`)
uniquely identifies which file a deserialised module came from, so
the loader supplies the file path to whatever consumer needs it
(currently nothing — step 3 wires it).

This saves ~16 bytes per AST node (a typical prelude has ~10K nodes
per module → ~160 KB savings per module → ~5 MB total across 32
preludes). It also avoids the "did the file path embed `/tmp/runner-xyz`
or `/Users/alice`?" problem that bit cache-design.md §"Span path
normalization" — by not carrying paths in payload, we sidestep the
normalisation question entirely. Cache spans always point at the
file the loader knows it loaded.

### 4. ValueMode is preserved; Ty is not

`Expr` has two typer-relevant fields: `ty: Option[Ty]` and
`mode: ValueMode`. `ty` is None at parse time (the typer fills it in
`infer_program`); `mode` is `MUnknown` until `unbox_pass` decides
between MBoxed and MUnboxed.

Step 2 caches **post-parse**, so `ty` is always `None` at write time
and is hard-coded to `None` at read time — there is no wire payload
for it. `mode` *is* serialised, partly because it is a public
surface field that the parser does set in some sugars, but mostly
because step 2's wire format is meant to be re-usable by step A.1
(post-typecheck cache) with the same `ValueMode` slot meaning.

### 5. Round-trip test compares hex blobs, not AST values

Stage 1 has no `Eq` impl for AST types — every record / sum cell
would require an explicit comparator. The round-trip test instead
serialises a known `[Decl]` to `blob1`, deserialises to `restored`,
re-serialises `restored` to `blob2`, and compares `blob1 == blob2`.

This is the standard "serde determinism" check: if the encoder is
deterministic (we control it) and the decoder is the inverse, then
`blob1 == blob2` is equivalent to `restored == decls` for any
equivalence that respects the encoder. Stage 2 future test work can
add a stage-2-side `decl_eq` once `impl` arrives there.

### 6. Mode dispatch via a CLI flag, not a test fixture

`stage2/kaic2 --cache-roundtrip-test` invokes the in-source
`cache_roundtrip_self_test()`, prints `OK ...` or `FAIL ...`, and
exits 0/1. This shape works for stage 1 (no `test "..."` runner
available there) and runs without parsing any file. Future steps
can extend the same flag to read a `.kab` file and round-trip it.

### 7. The if-chain shape over match-on-Int is a stage-1 quirk

`cache_hex_to_exprkind_body` dispatches on the tag byte with an
`if tag == 1 { ... } else if tag == 2 { ... }` chain rather than
`match tag { 1 -> ... 2 -> ... }`. The latter is what the brief
implied but stage 1's parser handles long `if/else if` cleanly while
its match-on-Int support has historically had edge cases (selfhost
panics on certain shapes). Adopting the if-chain shape from the
existing `cache_hex_to_typeexpr_at` helper kept this safe.

The decoder is split across 5 helpers (`cache_hex_to_exprkind_body`,
`cache_hex_to_typeexpr_at` and its `_fn` / `_dim` / `_refine`
neighbours, `cache_hex_to_handle_body`) to keep each function's
match-nesting depth manageable. Stage 1's brace-matcher tolerates
about 6 levels deep before becoming hostile to refactoring; the
split-helper shape stays at 4.

## Structural surprises

- **There is no "records-only" sub-step of step 2.** The brief
  proposed splitting Step 2 (records, ~600 LOC) vs Step 3 (recursive
  Expr/Decl, ~1000 LOC) per PR #581's framing. In practice every
  named record (Param, FieldDecl, Variant, FieldInit, PField,
  RowLabel, HClause, HReturn, DefaultBlock, ProtoOp, EffectOp,
  TypeBody) holds a `TypeExpr` or `Expr` somewhere — and `TypeExpr`
  is mutually recursive with `Expr` via `TyRefine(TypeExpr, Expr)`.
  The split would have left ~half the records unimplementable until
  Step 3 anyway. This lane shipped the unified SCC.

- **The `ImportKind` variant `IkHole` was added post-spec.** The
  brief listed `ImportKind` as IkPath / IkAlias / IkSelective, but
  `IkHole(String)` (m7f §7) ships in current main. The match was
  non-exhaustive until I added the fourth arm. This is the kind of
  drift that will keep biting until cache format-versioning is
  wired up.

- **Variable names `pub` and `variant` collide silently with C
  codegen.** `pub` is a kaikai keyword (so `Some(cbp) -> match cbp
  { CBP(pub, p1) -> ... }` parses but emits invalid C); `variant` is
  not reserved by the kaikai parser but the C emitter generates
  `kai_variant(...)` calls inline, and a `variant` local shadows
  that. Both produced the same opaque error from `cc`: "called
  object type 'KaiValue *' is not a function". The fix was a global
  rename — `pub` → `is_pub`, `variant` → `which`. Worth pinning as
  a memory: locals in `stage2/compiler.kai` must avoid `variant`,
  `pub`, and probably `args` (already known per
  `feedback_kaikai_param_args_shadow.md`).

- **Record literal construction needs the type prefix in stage 1.**
  `{ tkind: …, line: …, col: … }` does not parse as a TypeExpr
  literal; `TypeExpr { tkind: …, line: …, col: … }` does. Same for
  `Expr`, `Pattern`, `Param`. Pattern destructure as a match arm
  is also unsupported: `match t { { tkind, line, col } -> … }` —
  must use `t.tkind` / `t.line` / `t.col` instead. Both rules are
  scattered through the prior codebase but neither was in any
  written reference I could find when I started. Now they are.

## Fixtures added

- `cache_roundtrip_self_test()` in `stage2/compiler.kai` builds a
  synthetic `[Decl]` containing one `DType` sum (Color = Red |
  Green | Blue), one `DType` record (Pair[T]), one `DFn`
  (`fn answer() : Int = 42`), and one `DConst` (`PI : Real`).
  Output: `OK 4 decls round-trip; blob=802 hex chars`.

  This exercises every "shape" the decoder needs to handle:
  - sum-type decls (DType + TBSum + Variant + nullary args)
  - generic record decls (DType + TBRecord + FieldDecl + TyName(T,[]))
  - DFn body (Expr + EInt + line/col span)
  - DConst body (Expr + EReal + Real codec)

  What it does NOT exercise: protocol/effect/handler decls, every
  ExprKind variant (only EInt and EReal are covered), patterns,
  pipes, lambdas, imports. Step 3 should extend the round-trip to
  a real parsed prelude (`parse_prelude("stdlib/core/string.kai")`)
  once the loader is wired and a real prelude can be serialised
  through `cache_serialize_module`.

  No `examples/cache/` fixture file — the test is invoked via
  `stage2/kaic2 --cache-roundtrip-test`, no source file is read.

## Real cost vs estimate

| Activity | Brief estimate | Actual |
|---|---|---|
| Read brief + #581 + cache-design + verify state | not budgeted | ~20 min |
| Step-1 O(n²) hot-fix on three helpers | not budgeted | ~10 min |
| Codecs for Char + Real | included in 600 LOC | ~5 min |
| SCC of types (TypeExpr / RowExpr / UnitExprT) | included in 600 LOC | ~25 min |
| SCC of Expr / Pattern / Stmt (35 + 10 + 6 variants) | included in 1000 LOC | ~60 min |
| Decl serdes (15 variants) | included in 1000 LOC | ~20 min |
| Severity / Diagnostic / RelatedInfo | included | ~5 min |
| Entry points + CLI flag + dispatch | included in 200 LOC | ~10 min |
| Round-trip test | 50 LOC | ~5 min |
| Debug + brace-counting + name shadowing | not budgeted | ~25 min |
| Selfhost / tier0 / tier1 gates | not budgeted | ~10 min |
| Retro + PR body | not budgeted | this paragraph |
| Total | "1500-2500 LOC, 1-2 lanes" | 2 783 LOC, 1 lane |

The brief's LOC envelope was slightly high — the boilerplate of
match-nesting + sum-tag dispatch is denser than `#derive` would have
been. 2 783 LOC for ~28 AST types averages to ~100 LOC per type,
which is the going rate for hand-rolled cursor-based serdes in any
language. No surprises.

## Follow-ups

- **Step 3** — `kaic2 --prelude-cache <path>` flag. Calls
  `cache_deserialize_module` instead of running lex+parse on the
  named prelude. ~150 LOC.
- **Step 4** — `bin/kai` shell-level cache hit/miss: compute sha256
  of each prelude source (via `shasum -a 256`), look up
  `~/.cache/kaikai/preludes-v1/<sha>.kab`, on miss invoke kaic2 to
  write the blob, on hit pass `--prelude-cache` to kaic2. ~200 LOC.
- **Step 5** — Atomic write (tmp + fsync + rename) + 4 invalidation
  fixtures (header magic / format version / kaikai version / sha
  mismatch). ~100 LOC.
- **Step 6** — Bench harness pass over `kai build empty.kai` to
  confirm ~0.41 s wall savings vs the 2.31 s 2026-05-14 baseline,
  then close the issue's A.0 sub-phase. The remaining 1.90 s wall
  is the typer + codegen + cc + shell, which A.1 / A.2 attack
  separately.
- **Format version remains `1`.** Step 3 should bump to `2` ONLY if
  the wire format changes; the act of *adding callers* does not bump.
  The `cache_kaikai_version_hash() : Int = 1` placeholder also needs
  a real value before the cache is non-dormant — it must change on
  every kaikai release tag, otherwise upgrading the compiler will
  load stale prelude caches and emit wrong code. Step 4 / 5 owns
  that wire-up.
- **The `EVariantsOf` second-field encoding.** That field carries the
  resolved variant names which the parser leaves `[]`; the typer
  fills it in `synth_variants_of`. Step 2 serialises whatever is
  present at write time (always `[]` for post-parse cache). A.1
  serialisation should round-trip the filled-in list — no work in
  step 2 needed there, just a reminder for the A.1 lane.

## What this lane did NOT do

- No `cache_serialize_module` call site outside the round-trip test
  function. The cache remains dormant. Step 3 wires it.
- No `~/.cache/kaikai/...` directory layout. Step 4 wires it.
- No format-version bump policy doc. The header's
  `cache_format_version()` is still `1` from step 1 and step 2 did
  not change the layout in a way that requires a bump (it *added*
  payload semantics on top of the step-1 header).
- No `.kab` example file checked into the repo. Step 4 / 5 should
  add one alongside the invalidation fixtures.

## Limitations

- Round-trip test is synthetic — it does not exercise a real prelude.
  Until a real prelude flows through the encoder (Step 3), there
  is a real risk that some ExprKind / PatKind / TyKind variant
  has a subtle bug that the 4-decl fixture happens to miss. The
  shape of the decoder is uniform across variants, so the risk is
  low, but it is not zero.
- `cache_kaikai_version_hash() : Int = 1` is a placeholder that
  must be replaced with a build-time hash before the cache is
  enabled for users.
