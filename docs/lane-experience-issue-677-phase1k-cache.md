# Lane experience — issue #677 Phase 1k (re-scope): extract cache codec into compiler/cache.kai

## Scope as planned vs as shipped

**Planned (original Phase 1k):** extract the `# cli + driver` block of
`stage2/main.kai` into `compiler/driver.kai`.

**Re-scoped (this lane):** the driver-extract lane (PR #687) shipped
*analysis only* and concluded the driver is the pipeline apex — it calls
~143 head-defined symbols (typer, resolver, emitter, every desugar,
perceus/unbox, protocol lowering, validators, dumps), so extracting it
would force ~140 `pub` exports and invert the dependency arrow. The
analysis (`docs/lane-driver-cross-section-analysis.md`) identified the
*one* clean cut inside that block: the KAB2 cache codec + AST serde,
which sits *beside* the pipeline (a pure AST↔bytes function with zero
upward calls). The integrator re-scoped Phase 1k to that cut. This lane
executes it.

**Shipped:** `stage2/compiler/cache.kai` (3373 LOC), 4-symbol public
surface, 254 private internals, `test_cache.kai` with 16 unit tests +
3 property checks. main.kai drops from 57481 → 54155 LOC (−3326).

## What moved, what stayed

- **Moved:** the whole codec — `cache_format_version` / `cache_magic`
  through the per-node `cache_*_to_hex` / `cache_hex_to_*` walkers, all
  `Cache*Pos` position types, plus the three public entry points
  (`cache_serialize_module`, `cache_deserialize_module`,
  `cache_roundtrip_self_test`) and the `CacheDeserialized` result type.
- **Stayed in main.kai:** `emit_prelude_cache_for` (mixes serde with the
  prelude helpers `prelude_module_name` / `strip_prelude_tests` /
  `tag_decls_module_origin` that live in the driver layer) and the three
  call sites (`load_prelude_cached`, the `run` dispatch arms).

## Decision: `cache_roundtrip_self_test` moved with the codec

The analysis flagged this as the lane's call. **Moved it.** It tests the
codec and only the codec — it builds a synthetic `[Decl]` in memory,
serialises, deserialises, re-serialises, and compares blobs. It has no
prelude dependency, so leaving it behind would have meant a fourth
main.kai bridge for no reason. Its sole caller (the `MCacheRoundtrip`
arm in `run`, main.kai:54080) resolves it through `import compiler.cache`
exactly like the other two entry points. The brief listed it among the
"4 bridge fns that stay," but the brief also (point 6) listed
`cache_roundtrip_self_test` as a candidate public symbol — moving it
resolves that tension in favour of cohesion.

## Public surface

4 public symbols (target was 2–5):

- `pub type CacheDeserialized` (constructor `CDs` exported with it)
- `pub fn cache_serialize_module`
- `pub fn cache_deserialize_module`
- `pub fn cache_roundtrip_self_test`

Each carries a single-line `#[doc("…")]`. The un-pub audit was trivial:
the codec was extracted verbatim (no `pub` markers in the source range),
so all 254 internals are private by construction; only the 4 entry
symbols got `pub` added. Sealing verification showed exactly 3 external
callers (`load_prelude_cached` @50860 for deserialize, the `run` arm
@54079 for roundtrip, `emit_prelude_cache_for` @57468 for serialize) and
zero hits for any of the 254 internals outside the range — a textbook
clean cut, as the analysis predicted.

## Structural surprises the brief did not anticipate

1. **Modules live in `stage2/compiler/`, not `compiler/`.** The brief
   said "new compiler/cache.kai" and "template: compiler/fmt.kai". The
   actual path is `stage2/compiler/cache.kai`; tests live in
   `stage2/tests/`. No functional impact, but the brief's paths were one
   level short.

2. **No caller rewrite was needed.** The brief's Method step 7 said
   "rewrite the 4 bridge fns to call `cache.cache_serialize_module(...)`".
   In kaikai, `import compiler.cache` brings the module's `pub` symbols
   into the namespace *unqualified*, so the existing call sites
   (`cache_serialize_module(...)`, `cache_deserialize_module(...)`,
   `cache_roundtrip_self_test()`) and the `CDs(...)` pattern match resolve
   through the import with no edit. The only main.kai change beyond the
   deletion was adding `import compiler.cache`. (Both qualified
   `cache.fn` and unqualified `fn` styles work; `test_cache.kai` uses the
   qualified style for clarity, mirroring `test_fmt.kai`.)

3. **KAB2 is raw binary, not hex.** The brief's "Sharp edges" said "the
   codec uses hex strings — preserve the exact format." Reality
   post-merge: KAB2 (issue #592) replaced the KAB1 hex ASCII format with
   *raw packed binary* via the `int_to_byte_string` runtime primitive.
   The `_to_hex` / `hex_to_` names are legacy KAB1 carryover; every helper
   is now a binary encoder/decoder. The header comment in cache.kai
   documents this so the next reader is not misled. Preservation was
   still exact (verbatim move), so the discrepancy was documentation-only.

4. **Three imports, not two.** The brief expected `compiler.ast` (+
   maybe `compiler.util`). The codec also serialises `Diagnostic` /
   `Severity` / `RelatedInfo`, so `compiler.diag` is required too. Final:
   `compiler.util` (concat_all), `compiler.diag`, `compiler.ast`.

## Fixtures added, coverage

`stage2/tests/test_cache.kai`:
- 16 unit tests: round-trip per ExprKind body (EInt, EReal, EBool,
  EChar, EStr, EVar, ECall, EBinop), per Decl shape (record DType, sum
  DType, DImport, empty module, multi-decl module), header invariants
  (sha echo, decl count, self-test OK).
- 3 property checks: EInt body identity for any literal, EBool body
  identity for either boolean, EVar body identity + decl-count
  preservation for a generated name. All 100 iterations green.

The technique is structural blob-equality: serialise → deserialise →
re-serialise, assert the two blobs are byte-identical. A dropped slot or
misordered field on any covered node breaks equality and names the node.
The internal walkers are covered transitively here and exhaustively on
every selfhost (the prelude cache rides this exact codec).

Coverage gap: the tests exercise the *public* round-trip path, not the
on-disk header-mismatch / truncation cache-miss branches of
`cache_deserialize_module` (those return `None`). Those are exercised by
the live prelude load path during every build but lack a dedicated
negative fixture. Left for a follow-up if the codec gets a dedicated
corruption-resilience lane.

## Acceptance gate

- `make kaic2` — compiles clean (extraction resolves, callers find the
  imported symbols).
- `make selfhost` — **kaic2b.c == kaic2c.c**, the critical gate: the
  prelude cache serialises/deserialises through the extracted codec, and
  the selfhost reloads it; byte-identical output proves the codec is
  intact after the move.
- `kai test stage2/tests/test_cache.kai` — 16/16.
- `kai check stage2/tests/test_cache.kai` — 3/3.
- `make tier1` — green end-to-end (run locally before push).

## Real cost vs estimate

Small lane, as the analysis promised ("a genuinely easy extraction with
a small blast radius"). The bulk of the work was *verification* (sealing
audit over 258 symbols, import discovery, format-discrepancy check), not
mechanical moving. The verbatim-move-then-mark-pub approach kept the diff
honest and the un-pub audit trivial. No compiler edits, no AST changes.

## Follow-ups for future pipeline lanes (modules / desugar / infer / protocols / emit_c / emit_llvm)

- **The driver is still the apex and still last.** This lane did not
  touch the driver; it only removed the codec that happened to live in
  the driver's line range. The remaining `# cli + driver` block still
  calls ~143 head symbols. The sequencing in the analysis stands: extract
  the pipeline stages *beneath* the driver first (typer, emitter,
  desugars, perceus/unbox, protocol lowering, validators), then the
  driver last, when it imports modules instead of bodies.
- **`BuildMode` and `PreludeSegment` want to move to `compiler.ast`.**
  The analysis flagged these two types as defined deep in the driver but
  consumed by the emitter and prelude-mangling passes up in main.kai. A
  future emitter lane should lift them to `ast` (or a shared types
  module) to shrink the driver's eventual pub surface.
- **The codec is AST-shaped and brittle to AST changes.** Any new variant
  in `Expr` / `Decl` / `Ty` / `Pattern` / `Stmt` in `compiler/ast.kai`
  must be mirrored in cache.kai or the prelude cache silently misses
  (deserialise returns `None`, falls back to re-parse — correct but
  slow, and selfhost still passes because re-parse produces the same
  AST). A future ast lane that adds a variant must touch cache.kai in the
  same commit; the test_cache.kai round-trips will catch a *broken*
  encoding but not a *missing* one (a new variant the encoder doesn't
  emit just won't appear in a test until someone writes a fixture for it).
