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
(today: 1.5 s tiny program, of which ~1.3 s is stdlib re-parsing)
to "deserialise pre-typed modules on every build, re-parse only
what changed" (target: ≤ 300 ms cold, ≤ 150 ms incremental — DoD
#6).

## Two layers, one rule

| Layer | What it caches | Where it lives | When it invalidates |
|---|---|---|---|
| **Phase A** | Stdlib preludes (immutable from user POV) | `~/.cache/kaikai/preludes-v<N>/<sha>.kab` | stdlib file changes OR kaikai version bump OR cache format bump |
| **Phase B** | User-side files (mutable) | `<project>/.kai-cache/<sha>.kab` | source changes OR any transitive import changes OR kaikai version bump OR cache format bump |

Both use the same on-disk format (TypedModule serialised, defined
by Phase A's lane). Both use the same invalidation primitives.

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
  on every change to the TypedModule binary layout.

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
    uint8_t  payload[];         // serialised TypedModule
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

## Acceptance for Phase A + Phase B together

When both phases land:

- `bin/kai build empty.kai` cold (no cache): ≤ 300 ms wall.
- `bin/kai build empty.kai` warm (cache hit): ≤ 150 ms wall.
- Editing `empty.kai` and rebuilding: ≤ 150 ms wall.
- Editing a stdlib file and rebuilding empty.kai: re-build (≤ 1 s).
- Editing `stage2/compiler.kai` and self-compiling: same wall as
  today (~5-7 s), no improvement (self-compile already
  re-typechecks the whole file).
- Cache survives across `kai build` invocations.
- Cache is invalidated correctly per the cases above (5 negative
  fixtures verify each invalidation path).

## Related

- #452 — Phase A stdlib cache.
- #455 — Phase B user-file incremental cache.
- #454 — Compiler library mode (defines the TypedModule that gets
  serialised).
- #447 — LSP v1 (consumes #454's query surface; benefits from this
  doc indirectly via faster didSave re-typecheck).
- `docs/roadmap.md` DoD #6 — compile-time performance gate.
- `docs/benchmarks/cross_lang_2026-05-10.md` — empirical motivation.
