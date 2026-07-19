# Compiler cache — invalidation design

Shared design doc for Phase A (stdlib cache, #452) and Phase B
(user-file incremental cache, #455). Both issues cite this doc.
Reads: any lane touching compiler caching, future lanes adding
new cache layers (Phase C compilation server, etc).

This is a **scope doc**, not implementation guide. It pins WHAT
must hold for correctness; HOW each phase wires it is the lane's
choice.

> **Status (measured 2026-07-19).** What ships today is **post-parse
> only**: A.0 wire format (#592) plus the post-parse `[Decl]` caches for
> core (#825) and user files (#455). **A.1 (post-typecheck) and A.2
> (post-perceus) were never built** — no `feat(cache)` commit exists for
> either; the typer, monomorph, and perceus run in full on every build
> (verified: cold vs warm emitted C is byte-identical). #461 (A.1/A.2)
> closed as obsolete, not blocked: its `lower_protocols` boundary blocker
> was real but fixed in #597, and a re-measure showed the whole `kaic2`
> compile had fallen so far that a post-perceus cache "cannot move the
> total wall while `cc` dominates." The typer is now ~4% of an
> end-to-end build. **Do not revive A.2.** The A.1/A.2 sections below are
> preserved as the original scope but describe levers whose premise has
> evaporated; the live successor is a per-module typed-interface cache
> (#1298), sequenced after the header-slicing work (#1296) and pursued
> only if a re-measure shows the typer became the front-end bottleneck.

## Goal

Move kaikai from "re-parse stdlib + user code on every `kai build`"
(today: 1.5 s tiny program) to "deserialise pre-typed modules on
every build, re-parse only what changed" (target: ≤ 300 ms cold,
≤ 150 ms incremental — DoD #6).

## Empirical breakdown (2026-05-14, M2 Pro, arm64)

Phase-by-phase wall of `bin/kai build empty.kai`, n=5 runs each,
median reported. See `tools/bench-phases.sh` (run with `-r` for RSS).
Anchor: post-#578 (typer per-module fold) + 32 core modules auto-loaded
by `bin/kai`. Re-measured 2026-05-14 to refresh the 2026-05-13 numbers
after #574 / #578 / #570 / #571 / #572 / #573 landed.

| Stage | Cumulative wall | Delta | RSS peak | Share |
|---|---|---|---|---|
| `--tokens` (lex core modules) | 0.26 s | 0.26 s |  73 MB | 11% |
| `--ast` (parse core modules) | 0.41 s | 0.15 s | 151 MB |  7% |
| `--check` (typecheck) | 1.00 s | 0.59 s | 196 MB | 26% |
| `--infer` (typecheck) | 0.97 s | -0.03 s | 196 MB |  0% |
| default (emit C) | 1.52 s | 0.55 s | 306 MB | 24% |
| `kai build` total (kaic2 + cc + shell, median 5 runs) | 2.31 s | 0.79 s | ~448 MB | 34% |

The 2.31 s baseline is ~17% better than the 2.79 s recorded on
2026-05-13. None of the intervening lanes touched the cache layer —
the improvement comes from #574/#578 (typer fold simplification),
#573 (LLVM lambda lift cleanup), and a handful of pre-typer cascade
simplifications that landed in v0.56.4–0.56.6. The shape of the
breakdown is unchanged; only absolute numbers shift.

### What the breakdown means

A second control (2026-05-13): `kaic2` over `empty.kai` with **no
core modules loaded at all** runs in ~3 ms. The full pipeline
(parse → cascade → typecheck → monomorph → lower_protocols →
perceus → emit) on a 1-line user file is essentially free. The
2.06 s of `kaic2` wall today is **100% core processing** at every
pipeline stage, not just lex+parse.

That changes what each cache layer must skip to be effective. Walls
below are projected savings on the 2.79 s 2026-05-13 baseline:

- **A.0 (cache `[Decl]` post-parse)** — skips lex+parse of core modules
  only. Saves ~0.41 s. Wall → ~1.90 s. Mechanism: the 15+ pre-typer
  passes + the typer + codegen all still run on the merged
  core++user `[Decl]`.
- **A.1 (cache typed `[Decl]` + per-module env deltas)** — skips
  the pre-typer cascade and the typer on the core. Saves an
  additional ~0.59 s (parse+typecheck → from cache). Wall → ~1.31 s.
  Mechanism: requires the typer-modularisation refactor's
  **semantic step** AND a driver-side multi-segment refactor. See
  "What #574 unblocked and the lower_protocols boundary it did not"
  below.
- **A.2 (cache through perceus + dead-code-emit on core)** —
  skips lower_protocols, perceus, and emit on core DFns that the
  user does not reach. Saves the remaining ~0.55 s. Wall → ~0.76 s.
  Mechanism: emit walks only the user's reachability closure into
  the core; un-reached core DFns are skipped at emit. This
  reuses the dead-code elimination the compiler already performs
  (verified: `kaic2` over empty.kai with the full stdlib in scope
  emits ~235 lines of out.c with ~39 kai_* symbols, none of them
  core map/filter/fold). Today emit *walks* every core DFn
  even though it skips the unreached ones; A.2 caches the walked-and-
  decided state.

**Reaching DoD #6 (≤ 300 ms) requires A.0 + A.1 + A.2 cumulatively,
plus closing the ~0.73 s shell + cc + remaining shared overhead.**
The shell+cc tail is its own engineering surface (a compilation
daemon or direct LLVM emission); no cache layer touches it. Each
A.x layer is independently shippable and independently useful, but
none of them on its own reaches DoD #6.

### What #574 unblocked and the lower_protocols boundary it did not

PR #578 (issue #574, merged 2026-05-14) shipped the **semantic** half
that #568 deferred. `typecheck_module(file, mod, inherited, …)` now
actually consumes its `inherited: ModuleEnvDelta` parameter, and
`typecheck_program` folds left-to-right across `[ModuleDecls]`
threading the accumulating delta. The typer API is therefore ready
for a cache loader to call as:

```
typecheck_program(file, [core_segment, user_segment], proto_impls, verbose)
```

where `core_segment` carries decls reconstructed from the cache
and the typer reuses the core delta on `inherited` instead of
recomputing it. Single-segment callers (every legacy driver call)
keep the byte-identical baseline because the fold reduces to
`typecheck_module(file, mod, empty_delta, …)`.

**What is still missing for A.1**: the driver call site itself.
`compile_source` (`stage2/compiler.kai:58150`) runs the 30+
pre-typer passes — `qualtype_decls`, `rqc_decls`,
`lower_pattern_narrow_decls`, `lower_consts`, `lower_axioms`,
`inject_builtin_effects`, `expand_aliases_in_decls`, `expand_ta_decls`,
`desugar_pos_records_decls`, `desugar_index_decls`,
`desugar_var_decls`, `desugar_use_decls`, **`lower_protocols`**,
`desugar_interp_decls`, `rename_proto_calls_decls`,
`desugar_const_refs_decls`, `rewrite_nursery_caps_decls` — over
`merged_raw = list_append(qualified_core, qualified_decls)`. The
boundary between core and user decls is positional at line 58271
and survives the per-element walkers, but `lower_protocols` itself
ends with (`stage2/compiler.kai:52818`):

```
let final_decls = list_append(user_renamed,
                              list_append(impl_renamed, dispatchers))
```

`impl_renamed` and `dispatchers` are freshly-synthesised decls
that interleave core- and user-side protocol material; the
positional boundary between core- and user-origin decls is
destroyed here. Any cache loader handing the typer two
`ModuleDecls` segments needs either:

- a refactor of `lower_protocols` (and the desugar passes that
  follow it) so each output decl carries a `module_origin` tag
  the loader can partition on; or
- a different cache payload boundary — e.g. cache **post-pre-typer-
  cascade**, after every walker has run, accept that the cache no
  longer skips parse, and ship A.1 as "skip everything between
  parse and typer for the core". That bumps the saving from
  0.59 s (A.1's typer-only target) to ~0.76 s (parse + cascade +
  typer), but moves the cache payload past 15 more passes that all
  change shape across kaikai versions — every walker addition or
  removal is a payload bump.

The cleaner path is to tag the synthesised decls. The estimate is
~300-500 LOC across `lower_protocols`, `desugar_interp_decls`, and
`rename_proto_calls_decls`. The selfhost byte-identical gate is
the typer caring about origin tags — today it does not, so origin
is purely tracking metadata.

Neither path is a one-line driver tweak. The cache lane that ships
A.1 owns this refactor (or chooses the payload-boundary alternative
above and pays for it in format-version churn).

### What #460 did and did not deliver

PR #568 (issue #460, merged 2026-05-14) introduced `ModuleEnvDelta`,
`typecheck_module(file, mod, inherited, proto_impls, verbose)`, and
`typecheck_program(file, modules, proto_impls, verbose)` in
`stage2/compiler.kai`. That landed the **API surface** the A.1
payload needs.

What it deliberately deferred (marked "sub-step 3d-future" in the
source comments at `stage2/compiler.kai:36649`):

- `typecheck_program` still flattens `modules` to a single segment
  via `flatten_module_decls` and runs `typecheck_module` once on
  the concatenation, so the byte-identical selfhost baseline holds.
- `typecheck_module` ignores its `inherited: ModuleEnvDelta`
  parameter. `collect_program_data` rebuilds the env from scratch
  over the full decl list every call. Composing deltas across
  modules requires re-threading `build_ty_env`, the alias
  resolver, `collect_records`, `collect_sums`, `collect_op_to_eff`
  and the seven pre-typer passes that today run globally.

The A.1 cache lane therefore cannot just "deserialise a delta and
slot it into `inherited`" — there is no consumer for it yet. The
sub-step 3d-future is the load-bearing piece, and at the cost
estimate quoted in #460 (2 500–4 000 LOC, selfhost-byte-identical
non-trivial), it is a lane of its own scale comparable to #568.

The Phase A.1 cache lane is therefore **two lanes**: one that
delivers the semantic per-module typecheck (consumes `inherited`),
and one that serialises + ships the cache itself. They can land
in either order; the cache lane is safer to ship second because the
semantic refactor will likely change the delta's shape.

## Two layers, one rule

| Layer | What it caches | Where it lives | When it invalidates |
|---|---|---|---|
| **Phase A** | Stdlib core modules (immutable from user POV) | `~/.cache/kaikai/core modules-v<N>/<sha>.kab` | stdlib file changes OR kaikai version bump OR cache format bump |
| **Phase B** | User-side files (mutable) | `<project>/.kai-cache/<content_hash>-<dep_hash>.kab` | source changes OR any transitive import changes OR kaikai version bump OR cache format bump |

Both use the same on-disk format (KAB1, defined below). What
varies across the three sub-phases is the *payload schema*:

- **A.0 payload** — post-parse `[Decl]` only. Requires no typer
  refactor. Selfhost risk is low because cached and uncached
  pipelines diverge only at the parse boundary, after which both
  paths run the same cascade + typer + codegen.
- **A.1 payload** — typed `[Decl]` + per-module env delta (recs,
  sums, op_eff_arities, type_aliases, op_to_eff, proto_impls,
  TyEnv slice). Requires `build_ty_env` (`stage2/compiler.kai:25273`)
  and `infer_program` (`stage2/compiler.kai:33278`) to produce
  these deltas per module rather than over the merged core++user
  list. The `core_len: Int` field today is a positional index for
  diagnostics, not a modularity boundary — the refactor builds the
  boundary. Est. 2500–4000 LOC, selfhost-byte-identical risk
  non-trivial. Tracked separately.
- **A.2 payload** — A.1 plus an emit-time reachability index: the
  cached core knows which DFns are *candidates* for emit; the
  user's reachability closure picks the subset. Requires emit to
  consume a cache + reachability seed. Depends on A.1.

  Note: A.2's saving is the largest individual chunk (~0.55 s of
  emit-on-core on the 2026-05-14 baseline) and is what brings
  the wall under ~1 s. A.0 + A.1 alone leave the codegen pass running
  over the full core DFn set.

### Serialisation primitive

The kaikai stdlib defines `protocol Serialize { to_string,
from_string }` (`stdlib/protocols.kai:327`) for atomic values
(Int/Real/String/Bool, all four impls shipped) plus `#derive`
support for Show/Eq/Hash/Ord but **not yet for Serialize**
(`stage2/compiler.kai:45590` dispatcher table). Two gaps block any
A.x lane from using Serialize today:

1. **No derive for Serialize.** Recursive AST types (Decl/Expr/…)
   would need ~30 hand-written impls today; with a derive_serialize
   pass analogous to derive_show, that drops to 30 one-line
   annotations.
2. **`from_string : String → Result[Self, String]` is whole-string,
   not cursor-based.** A list of records can't be parsed by calling
   `from_string` on a prefix and getting `(Self, rest)` back; the
   protocol consumes the full string or fails. AST serialisation
   needs a cursor (`from_bytes : ([Byte], Int) → Result[(Self, Int), E]`
   or similar).

Both gaps are pre-blockers for the A.0 lane. The cache lane that
ships A.0 must either:

- Extend `Serialize` to a cursor-aware shape (changes a public
  stdlib protocol — coordinate with #258 Default consumers); or
- Add a sibling `protocol BinSerialize` specialised for binary
  serde with cursor semantics, leaving `Serialize` alone for
  configuration-style values.

The choice is the A.0 lane's. Either way it is **not a one-line
change** and should be its own design pass before any cache code
ships.

## Cache key

The cache key is what identifies a single entry. **Content-addressable**,
not mtime-based — two machines with the same content must hit the
same cache key (enables CI cache sharing, cross-workstation builds).

### Phase A key

```
sha256(source_bytes) + kaikai_version_hash + cache_format_version
```

- `source_bytes`: the file's raw content. NOT path, NOT mtime.
- `kaikai_version_hash`: bumped on each release tag. A new kaikai
  binary cannot reuse caches from the previous version even if the
  AST format is unchanged — the compiler may infer types
  differently.
- `cache_format_version`: a u32 in the cache file header. Bumped
  on every change to the serialised-AST binary layout.
- **Cache schema variant**: encoded in the high byte of
  `format_version`. `0x00` = A.0 (post-parse `[Decl]`); `0x01` =
  A.1 (typed `[Decl]` + env deltas). A.0 caches and A.1 caches
  coexist on disk; the loader rejects the wrong variant for the
  current compiler load path.

### Phase B key

```
sha256(source_bytes) +
  sha256(concat(sha256(d) for d in transitive_imports)) +
  kaikai_version_hash +
  cache_format_version
```

The transitive-imports hash is what makes invalidation cascade
correctly. If `core/list.kai` changes, every user file that
transitively imports it has a different `concat` hash, so all of
them miss the cache and rebuild.

**Critical**: the dependency closure must be computed *before*
deciding cache hit/miss. The compiler walks `import` statements
to find direct deps, then recurses. This is cheap — the
imports section is in the first ~5% of any source file.

## On-disk format

```
struct KaiAstBlob {
    char     magic[4];          // "KAB1" - magic + format generation
    uint32_t format_version;    // monotonically increasing
    uint32_t kaikai_version;    // matches binary's release tag hash
    uint64_t payload_len;       // bytes after header
    uint64_t checksum;          // sha256 truncated, of payload
    uint8_t  payload[];         // serialised AST (A.0: [Decl]; A.1: typed + env deltas)
};
```

Header is 32 bytes. Constant-time validation at load:

1. magic matches "KAB1" → if not, cache miss (corrupt or wrong tool).
2. format_version matches current compiler's expectation → if not,
   cache miss (format drift).
3. kaikai_version matches current binary → if not, cache miss
   (compiler upgraded).
4. checksum matches recomputed sha256(payload) → if not, cache miss
   (corrupt).

Only after all four pass is the payload deserialised.

## Invalidation cases (concrete)

### User edits their own code

```
foo.kai sha changes
→ Phase B: composite key changes
→ miss on foo.kai entry
→ re-typecheck foo.kai
→ Phase A entries reused (stdlib sha unchanged)
→ write new Phase B entry
→ target: ≤ 150 ms incremental
```

### User edits a stdlib file (dev mode, KAI_STDLIB pointed at checkout)

```
stdlib/core/list.kai sha changes
→ Phase A: miss on list.kai entry
→ re-parse + re-typecheck list.kai
→ write new Phase A entry
→ Phase B: every user file with core.list (direct or transitive)
   has different composite key → miss
→ re-typecheck each
→ target: full rebuild, ~5 s for a 50K LOC project
```

### User upgrades kaikai (brew upgrade)

```
kaikai_version_hash changes
→ ALL caches (Phase A + Phase B) miss on version check
→ re-build everything from scratch
→ first build after upgrade is slow, subsequent fast again
```

### Bug: cache corrupted (disk error, killed mid-write, etc)

```
magic/checksum/version mismatch in header
→ treat as miss (do not try to recover)
→ re-build, overwrite
```

### User runs `kai clean`

Subcommand explicit:

- `kai clean` — purges `<project>/.kai-cache/` (Phase B).
- `kai clean --global` — also purges `~/.cache/kaikai/core modules-v*/`
  (Phase A).
- `kai clean --older-than 30d` — optional, post-1.0, garbage-collect
  by atime.

## Non-invalidation cases

These should NOT invalidate the cache:

- **Changes to `stage0/runtime.h`**: the cache stores typed AST, not
  C output. Runtime changes affect the final binary (cc compiles
  fresh every time) but not the typed AST.
- **Changes to `bin/kai` driver shell**: doesn't touch typed AST.
- **Changes to `stage0/parser.c` / `stage0/main.c`**: only affects
  the bootstrap chain. User caches are unaffected unless the
  bootstrap rebuild changes the kaikai_version_hash (which it does
  on every release — but not on dev-mode edits to stage0).
- **Whitespace/comment-only changes to user code** that don't
  change the AST: today these DO invalidate (sha changes). A
  smarter cache could normalise — out of scope for v1. Acceptable
  cost.

## Atomic writes

To avoid race conditions in CI (parallel `kai build` invocations
on the same project, parallel test runners) and to avoid
half-written cache files after a SIGKILL:

```
write payload to <tmp>.kab.tmp
fsync the file
rename atomically to <sha>.kab
```

POSIX `rename(2)` is atomic on same filesystem. Both Phase A and
Phase B must follow this pattern.

## Span path normalization

The serialised TypedModule contains source spans pointing to file
paths. **Paths must be relative to the project root, not absolute**.
Otherwise:

- Cache built on `/Users/alice/proj` doesn't hit on `/home/bob/proj`.
- CI cache built in `/tmp/runner-xyz/` is useless on developer
  machines.

Phase A path normalization:

- Source paths in stdlib are relative to `$KAIKAI_ROOT/stdlib/` (the
  installed stdlib root). Cached spans use these relative paths.
- At deserialization, the compiler resolves them against
  `$KAIKAI_ROOT/stdlib/` from the current install.

Phase B path normalization:

- Source paths are relative to the project root (the dir containing
  `kai.toml`, or cwd if none). Cached spans use these.
- At deserialization, paths are resolved against the current
  project root.

## What the format version means

The `format_version` u32 in the header is bumped **on every change
to the TypedModule binary layout**, no exceptions. Examples:

- Add a new field to a node type → bump.
- Change the encoding of `Type` from variant tags to integers → bump.
- Add a new node type → bump (old caches don't know how to read it).
- Reorder serialization of an existing struct's fields → bump.

The cost of a bump is "all caches re-build on next invocation". This
is acceptable. The cost of NOT bumping when needed is "cache
deserializes garbage and the compiler emits silently wrong code" —
much worse.

There is no migration story. If you change the format, you bump,
old caches die, users rebuild once. Move on.

## What this design does NOT do

- **No distributed cache.** A team-wide shared cache (Bazel-style,
  Nix-style) is out of scope for Phase A/B. Each developer + each
  CI runner has its own local cache.
- **No incremental within a single file.** If `foo.kai` changes by
  one character, the whole file re-typechecks. Function-level
  incremental is out of scope.
- **No cross-package cache invalidation through `kai.toml`
  dependencies.** That depends on the package manager's resolution;
  out of scope here.
- **No on-disk garbage collection.** Cache directories grow
  unbounded until `kai clean` is run. Acceptable for v1.

## Acceptance per phase

Numbers below are expected walls after each layer ships, on a
2026-05-14 M2 Pro baseline of 2.31 s for `kai build empty.kai`
(32 core modules, post-#578). Tier 0 + Tier 1 + Tier 1-ASAN green
and selfhost per-compiler determinism (`kaic2b.c == kaic2c.c`;
stage 1's output is not required to match — see
`docs/decisions/bootstrap-relax-byte-identical-2026-05-22.md`)
are gates for every sub-phase.

### Phase A.0 — post-parse cache — **SHIPPED 2026-05-14 (#452) + KAB2 2026-05-15 (#592)**

- Warm wall: ≤ 1.90 s (cache-hit on all 32 core modules). **Empirically
  delivered** by #452 (hex KAB1) and #592 (binary KAB2). Post-KAB2
  baseline ~2.31 s overall (see §"Empirical breakdown").
- Saves: 0.41 s lex+parse.
- Cache invalidates on: source sha change, kaikai_version_hash
  change, header corruption (magic / checksum / version
  mismatch). Four negative fixtures verify.
- Atomic write: temp + fsync + rename.
- **Pre-blocker landed (#471):** the kaikai-native `BinSerialize`
  protocol with cursor semantics (`from_bytes(buf, pos)`) plus
  `#derive(BinSerialize)` for records and sums (including
  `[T]` / `Option[T]` collection payloads). Annotating the AST
  variants (Decl / Expr / ExprKind / Pattern / PatKind / TypeExpr /
  TyKind / RowExpr / Stmt / Param / Variant / FieldDecl /
  TypeBody / ImportKind / HClause / HReturn / UnitExprT plus
  ~10 record wrappers) remains, plus a serialiser per AST node and
  a deserialiser that reconstructs the same tree. The handwritten
  combinator surface is ~1500–2500 LOC depending on whether
  `#derive(BinSerialize)` covers every variant or some get
  hand-rolled impls (parametric Result is currently not derived;
  the dispatcher's whitelist is `binser_collection_head = List or
  Option`).

### Phase B — user-file post-parse cache — **SHIPPED 2026-06-11 (#455)**

The user-file analogue of A.0: a per-project, content-addressable cache
of post-parse `[Decl]` for user modules, with correct transitive
invalidation. New module `stage2/compiler/user_cache.kai`; the driver's
import resolver consults it before lexing each imported module; opt-in
via `KAI_CACHE=1` (the `bin/kai` wrapper creates `<project>/.kai-cache/`
and passes `--user-cache`).

- **Composite key materialised as the blob filename**
  (`<content_hex>-<dep_hex>.kab`). Invalidation is a property of the
  key, not a sweep: an edit anywhere in a module's transitive closure
  changes its dep hash, so its filename changes and the old blob is
  never read. Hashing uses the `string_hash` runtime prim (FNV-1a 64),
  not sha256 — content-change detection, not cryptography.
- **Dep discovery by token scan, not parse**, so computing the key does
  not defeat the cache.
- **Atomicity + mkdir live in `bin/kai`** — the stage1 bootstrap
  compiler that builds the kaic2 bundle lacks `file_rename` /
  `dir_create_dir` (and `bit_*`); a content-addressed name makes a torn
  write self-correcting (next build recomputes the key, rejects the
  header-invalid blob, overwrites).
- Invalidation gated by **six fixtures** (`examples/cache/userb_*.sh`,
  `make test-user-cache`): edit source / edit import / edit transitive
  import / version bump / corrupt blob, plus a **positive differential**
  (cold-cache == warm-cache == no-cache oracle, byte-identical C). The
  differential is the correctness gate selfhost byte-id cannot give —
  the compiler is built with the cache off, so selfhost never exercises
  a hit.

> **Honesty note (2026-06-11):** Phase B does **not** meet the issue's
> ≤ 150 ms cold-trivial target, and A.0-on-user-files cannot. On a
> trivial user file ~100 % of the wall is the core
> (lex+parse+**typecheck**), which is loaded with `caches=[]` and
> re-processed every build; A.0 skips only the user-file *parse*, which
> is ~0 ms. Measured: 8 trivial modules 0.21 s warm vs 0.21 s no-cache;
> one 2000-LOC module 1.28 s warm vs 1.22 s no-cache (deserialize ≈
> parse for kaikai modules, the same wash that killed the token-cache
> variant above). **The Phase B deliverable is correct transitive-
> invalidation infrastructure** — the durable piece #447 (LSP) builds
> on and A.1 reuses — **not** a wall-time win. The ≤ 150 ms target lives
> in A.1 (cache the core *post-typecheck*); see below. Phase A's
> core cache is also currently dead on the `kai build` path (loaded
> with `caches=[]`); wiring it in or retiring `--emit-core-cache` is
> a separate follow-up, deliberately not coupled to #455.

### Phase A.1 — post-typecheck cache

- Warm wall: ≤ 1.31 s.
- Saves: an additional ~0.59 s (typecheck on core). Note:
  caching post-typer alone does NOT save the ~0.20 s of pre-typer
  cascade — those passes run before the typer call site, so the
  cache loader bypasses them only if it hands the typer core
  decls that already went through the cascade. See "What #574
  unblocked and the lower_protocols boundary it did not" above.
- **Pre-blockers:**
  - **#574 (closed by #578, 2026-05-14)** — typer per-module
    semantics. `typecheck_module` now consumes `inherited:
    ModuleEnvDelta` and `typecheck_program` folds across segments.
    A multi-segment cache loader can drive the typer today.
  - ~~**#597 (open) — `lower_protocols` boundary destruction.**~~
    **#597 closed 2026-05-15** — `module_origin` propagation through
    `lower_protocols` + the desugar passes that follow it shipped;
    synthesised decls now carry the core-vs-user tag the cache
    loader needs. The driver call site that mixed core- and
    user-origin decls after `lower_protocols`'s `[user_renamed,
    impl_renamed, dispatchers]` concat is no longer the blocker.
    Mitigation paths documented in §"What #574 unblocked and the
    lower_protocols boundary it did not" — the boundary-tagging
    option (a) is what shipped. Lane retros:
    `docs/lane-experience-issue-461-phase-a1-a2.md` (pre-fix audit),
    and the #597 closing PR for the tagging refactor. **The A.1
    cache lane is now blocked only on the typer-side semantic step
    (sub-step 3d-future of #460), since closed by #578.**
  - The A.1 payload extends the A.0 payload with the
    `ModuleEnvDelta` struct (`ty_entries`, `unions`,
    `op_eff_arities`, `recs`, `sums`, `op_to_eff`,
    `unit_aliases`). The boundary-tagging refactor will likely
    add a `module_origin` field to every Decl, which the format
    needs to carry — payload schema cannot be frozen until the
    refactor lands.
- Builds on A.0 wire-up; payload schema variant byte changes
  (0x00 → 0x01).

### Phase A.2 — post-perceus + emit-only-user

- Warm wall: ≤ 0.76 s. The remaining ~0.43 s is shell + cc + the
  pieces of emit that scale with the user file rather than the
  core.
- Saves: the ~0.55 s of codegen passes (lower_protocols + perceus +
  emit walking) on the core.
- Pre-blocker: A.1.
- Mechanism: emit takes the cached core as input + the user's
  reachability closure as seed; walks only reached core DFns.

### DoD #6 (≤ 300 ms)

A.0 + A.1 + A.2 cumulatively reach ~0.76 s wall on the current
baseline. Closing the remaining gap to 300 ms requires either:

- A compilation daemon (Phase C) that amortises the fixed cc + shell
  overhead over a long-running process; or
- Direct LLVM IR emission (skip the cc invocation) — out of scope
  until post-1.0 per `docs/roadmap.md`.

DoD #6 in its current form (≤ 300 ms cold, ≤ 150 ms incremental)
is therefore the **endpoint** of the Phase A roadmap, not a
single-lane gate. The lane that closes the final gap can be Phase C
or LLVM-direct; either choice belongs in its own design doc.

### Variants that look attractive but do not pay off

- **Token cache.** Cacheing `[Token]` post-lex skips the ~0.33 s of
  lex. But `Token` carries `start: Int, length: Int` into the source
  buffer; the parser still needs the source bytes in memory and the
  deserialiser pays a per-token decode + allocation cost
  proportional to the token count (~35K tokens across 32 core modules).
  On a clean re-bench (2026-05-13) the cache load was within ~50 ms
  of the cost it replaced, with no slack to overcome cache-key
  hashing + file IO. The A.0 boundary (`[Decl]`) is the smallest
  payload that delivers a net win, because the parse pass is the
  one that turns `O(source-bytes)` work into `O(decl-count)` work.

## Related

- #452 — Phase A stdlib cache (epic; A.0 / A.1 / A.2 sub-lanes).
  **A.0 closed 2026-05-14** (hex KAB1 baseline); KAB2 binary format
  closed 2026-05-15 via #592. A.1 / A.2 remain open as separate
  lanes per the sub-phase plan above.
- #455 — Phase B user-file incremental cache.
- #459 — `BinSerialize` protocol with cursor + `#derive` (closed
  by #471; the kaikai-native serde primitive A.0/A.1 consume).
- #460 — typer modularisation API (closed by #568; API only —
  semantics deferred to #574).
- #461 — Phase A.2 — post-perceus cache + emit-only-user.
- #574 — typer per-module semantics (sub-step 3d-future of #460;
  A.1 pre-blocker, closed by #578).
- ~~#597 — `lower_protocols` + synth desugar boundary tagging (the
  remaining A.1 pre-blocker after #574 closed).~~ **#597 closed
  2026-05-15** — `module_origin` propagation through
  `lower_protocols` and the downstream desugar passes shipped; A.1
  payload schema can now carry the per-module boundary.
- #592 — KAB2 binary on-disk cache format (closed 2026-05-15);
  flipped A.0 from hex KAB1 to packed binary, delivered the
  projected 0.41 s wall save.
- #454 — Compiler library mode (defines the TypedModule that gets
  serialised).
- #447 — LSP v1 (consumes #454's query surface; benefits from this
  doc indirectly via faster didSave re-typecheck).
- `docs/roadmap.md` DoD #6 — compile-time performance gate.
- `docs/benchmarks/cross_lang_2026-05-10.md` — empirical motivation.
