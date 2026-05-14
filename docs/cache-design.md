# Compiler cache — invalidation design

Shared design doc for Phase A (stdlib cache, #452) and Phase B
(user-file incremental cache, #455). Both issues cite this doc.
Reads: any lane touching compiler caching, future lanes adding
new cache layers (Phase C compilation server, etc).

This is a **scope doc**, not implementation guide. It pins WHAT
must hold for correctness; HOW each phase wires it is the lane's
choice.

## Goal

Move kaikai from "re-parse stdlib + user code on every `kai build`"
(today: 1.5 s tiny program) to "deserialise pre-typed modules on
every build, re-parse only what changed" (target: ≤ 300 ms cold,
≤ 150 ms incremental — DoD #6).

## Empirical breakdown (2026-05-13, M2 Pro, arm64)

Phase-by-phase wall of `bin/kai build empty.kai`, n=5 runs each,
median reported. See `tools/bench-phases.sh` (run with `-r` for RSS).
Anchor: post-#572 (self-import) + #573 (LLVM lambda lift) + 32
preludes auto-loaded by `bin/kai`. Re-measured 2026-05-13 because
the prior 2026-05-11 baseline (1.47 s wall) predates the recent
prelude growth (decimal, money, fx, uuid, regexp, path, crypto/*)
and the per-prelude lex+parse work scales linearly with file count.

| Stage | Cumulative wall | Delta | RSS peak | Share |
|---|---|---|---|---|
| `--tokens` (lex preludes) | 0.33 s | 0.33 s |  73 MB | 12% |
| `--ast` (parse preludes) | 0.54 s | 0.21 s | 151 MB |  7% |
| `--check` (typecheck) | 1.18 s | 0.64 s | 196 MB | 23% |
| `--infer` (typecheck) | 1.31 s | 0.13 s | 196 MB |  5% |
| default (emit C) | 2.06 s | 0.75 s | 306 MB | 26% |
| `kai build` total (kaic2 + cc + shell, median 5 runs) | 2.79 s | 0.73 s | ~478 MB | 26% |

### What the breakdown means

A second control (2026-05-13): `kaic2` over `empty.kai` with **no
preludes loaded at all** runs in ~3 ms. The full pipeline
(parse → cascade → typecheck → monomorph → lower_protocols →
perceus → emit) on a 1-line user file is essentially free. The
2.06 s of `kaic2` wall today is **100% prelude processing** at every
pipeline stage, not just lex+parse.

That changes what each cache layer must skip to be effective. Walls
below are projected savings on the 2.79 s 2026-05-13 baseline:

- **A.0 (cache `[Decl]` post-parse)** — skips lex+parse of preludes
  only. Saves ~0.54 s. Wall → ~2.25 s. Mechanism: the 15+ pre-typer
  passes + the typer + codegen all still run on the merged
  prelude++user `[Decl]`.
- **A.1 (cache typed `[Decl]` + per-module env deltas)** — skips
  the pre-typer cascade and the typer on the prelude. Saves an
  additional ~0.77 s (parse+typecheck → from cache). Wall → ~1.48 s.
  Mechanism: requires the typer-modularisation refactor's
  **semantic step**, not just its API. See "What #460 did and did
  not deliver" below.
- **A.2 (cache through perceus + dead-code-emit on prelude)** —
  skips lower_protocols, perceus, and emit on prelude DFns that the
  user does not reach. Saves the remaining ~0.75 s. Wall → ~0.73 s.
  Mechanism: emit walks only the user's reachability closure into
  the prelude; un-reached prelude DFns are skipped at emit. This
  reuses the dead-code elimination the compiler already performs
  (verified: `kaic2` over empty.kai with the full stdlib in scope
  emits ~235 lines of out.c with ~39 kai_* symbols, none of them
  prelude map/filter/fold). Today emit *walks* every prelude DFn
  even though it skips the unreached ones; A.2 caches the walked-and-
  decided state.

**Reaching DoD #6 (≤ 300 ms) requires A.0 + A.1 + A.2 cumulatively,
plus closing the ~0.73 s shell + cc + remaining shared overhead.**
The shell+cc tail is its own engineering surface (a compilation
daemon or direct LLVM emission); no cache layer touches it. Each
A.x layer is independently shippable and independently useful, but
none of them on its own reaches DoD #6.

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
| **Phase A** | Stdlib preludes (immutable from user POV) | `~/.cache/kaikai/preludes-v<N>/<sha>.kab` | stdlib file changes OR kaikai version bump OR cache format bump |
| **Phase B** | User-side files (mutable) | `<project>/.kai-cache/<sha>.kab` | source changes OR any transitive import changes OR kaikai version bump OR cache format bump |

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
  these deltas per module rather than over the merged prelude++user
  list. The `prelude_len: Int` field today is a positional index for
  diagnostics, not a modularity boundary — the refactor builds the
  boundary. Est. 2500–4000 LOC, selfhost-byte-identical risk
  non-trivial. Tracked separately.
- **A.2 payload** — A.1 plus an emit-time reachability index: the
  cached prelude knows which DFns are *candidates* for emit; the
  user's reachability closure picks the subset. Requires emit to
  consume a cache + reachability seed. Depends on A.1.

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
2. **`from_string : String → Result[String, Self]` is whole-string,
   not cursor-based.** A list of records can't be parsed by calling
   `from_string` on a prefix and getting `(Self, rest)` back; the
   protocol consumes the full string or fails. AST serialisation
   needs a cursor (`from_bytes : ([Byte], Int) → Result[E, (Self, Int)]`
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
- `kai clean --global` — also purges `~/.cache/kaikai/preludes-v*/`
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
2026-05-13 M2 Pro baseline of 2.79 s for `kai build empty.kai`
(32 preludes, post-#572/#573). Tier 0 + Tier 1 + Tier 1-ASAN green
and selfhost byte-identical are gates for every sub-phase.

### Phase A.0 — post-parse cache

- Warm wall: ≤ 2.25 s (cache-hit on all 32 preludes).
- Saves: 0.54 s lex+parse.
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

### Phase A.1 — post-typecheck cache

- Warm wall: ≤ 1.48 s.
- Saves: an additional ~0.77 s (typecheck + cascade on prelude).
- **Pre-blockers:**
  - **#574** — typer per-module semantics (sub-step 3d-future of
    #460). API landed in #568; `inherited: ModuleEnvDelta` is
    today an unread parameter. Re-threading `build_ty_env`, the
    alias resolver, and the record/sum/op-to-eff collectors per
    module; 2 500–4 000 LOC, selfhost-byte-identical risk
    non-trivial.
  - The A.1 payload extends the A.0 payload with the
    `ModuleEnvDelta` struct (`ty_entries`, `unions`,
    `op_eff_arities`, `recs`, `sums`, `op_to_eff`,
    `unit_aliases`). The semantic refactor will likely also
    introduce alias-resolution / proto-impl-table deltas, which
    the format needs to carry — payload schema cannot be frozen
    until the refactor lands.
- Builds on A.0 wire-up; payload schema variant byte changes
  (0x00 → 0x01).

### Phase A.2 — post-perceus + emit-only-user

- Warm wall: ≤ 0.73 s. The remaining ~0.43 s is shell + cc + the
  pieces of emit that scale with the user file rather than the
  prelude.
- Saves: the ~0.75 s of codegen passes (lower_protocols + perceus +
  emit walking) on the prelude.
- Pre-blocker: A.1.
- Mechanism: emit takes the cached prelude as input + the user's
  reachability closure as seed; walks only reached prelude DFns.

### DoD #6 (≤ 300 ms)

A.0 + A.1 + A.2 cumulatively reach ~0.73 s wall on the current
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
  proportional to the token count (~35K tokens across 32 preludes).
  On a clean re-bench (2026-05-13) the cache load was within ~50 ms
  of the cost it replaced, with no slack to overcome cache-key
  hashing + file IO. The A.0 boundary (`[Decl]`) is the smallest
  payload that delivers a net win, because the parse pass is the
  one that turns `O(source-bytes)` work into `O(decl-count)` work.

## Related

- #452 — Phase A stdlib cache (epic; A.0 / A.1 / A.2 sub-lanes).
- #455 — Phase B user-file incremental cache.
- #459 — `BinSerialize` protocol with cursor + `#derive` (closed
  by #471; the kaikai-native serde primitive A.0/A.1 consume).
- #460 — typer modularisation API (closed by #568; API only —
  semantics deferred to #574).
- #461 — Phase A.2 — post-perceus cache + emit-only-user.
- #574 — typer per-module semantics (sub-step 3d-future of #460;
  A.1 pre-blocker).
- #454 — Compiler library mode (defines the TypedModule that gets
  serialised).
- #447 — LSP v1 (consumes #454's query surface; benefits from this
  doc indirectly via faster didSave re-typecheck).
- `docs/roadmap.md` DoD #6 — compile-time performance gate.
- `docs/benchmarks/cross_lang_2026-05-10.md` — empirical motivation.
