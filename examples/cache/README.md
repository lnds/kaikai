# Cache invalidation fixtures

Shell fixtures that pin the contracts of kaikai's compile caches. Each
script is self-contained: it builds a throwaway project and cache
directory under a temp dir, leaves nothing behind in the user's caches,
and exits 0 on success / 1 on regression. A stale-serving cache is a
correctness bug worse than no cache, so these are hard gates, not smoke
tests.

Two targets run them, both in tier 1:

    make -C stage2 test-user-cache   # userb_*.sh, typedc_*.sh
    make -C stage2 test-core-cache   # corec_*, emitc_*, wrapper_*, ncoreobj_*, nmodular_*

Run one alone with `sh examples/cache/<name>.sh`.

## User parse cache (`userb_*.sh`)

The per-project cache (`<project>/.kai-cache/<toolchain-id>/`, written by
`stage2/compiler/user_cache.kai`). `bin/kai` passes `--user-cache` when
`KAI_CACHE` is `1`, or `auto` (the default) in a package; the fixtures
set `KAI_CACHE=1`.

Negative — an edit must not serve stale decls:

- `userb_edit_source.sh` — edit the file's own bytes (content hash).
- `userb_edit_import.sh` — edit a direct import (consumer dep hash).
- `userb_edit_transitive.sh` — edit a two-hop import (cascade).
- `userb_chain_edit_isolated.sh` — an edit deep in an import chain
  re-parses that file and nothing else.
- `userb_home_and_edition.sh` — whatever else a parse reads is in the
  key or shown not to matter.
- `userb_version_bump.sh` — flip the blob's kaikai-version byte.
- `userb_corrupt_blob.sh` — truncate the blob past the header.

Positive — the emitted C is byte-identical across cold-cache, warm-cache
(hit path) and no-cache (oracle) builds. Self-host byte-identity does not
cover the cache, so this differential is what proves a hit reconstructs
exactly the decls a fresh parse would:

- `userb_cold_warm_identical.sh`
- `userb_kind_decl_module.sh` — the same for a module declaring a `kind`.

## Typed cut (`typedc_*.sh`)

Restoring a module's typed blob instead of re-inferring it: the interface
hash that decides whether importers must be re-inferred
(`typedc_iface_hash_gates.sh`), the symbol table
(`typedc_symtab_gates.sh`), and output byte-identity with the cut active
on the C path, the native path, a wide package, and the shared
core-cache directory (`typedc_cut_*.sh`).

## Core cache (`corec_*.sh`, `emitc_*.sh`)

The auto-loaded core, cached post-parse (KAB2 blobs) and, on the
c-modular backend, as emitted C translation units
(`core-emit-<key>.kct`, written by `stage2/compiler/emit_cache.kai`).
The emit key covers the core sources, the toolchain id, the edition and
the program's name-level surface, so a body-only edit hits while a
surface change misses.

- `corec_cold_warm_identical.sh`, `emitc_cold_warm_identical.sh` — the
  positive differentials: no-cache, cold and warm builds are
  byte-identical.
- `corec_edit_core_file.sh` — editing a core file forces a re-parse.
- `corec_version_bump.sh`, `corec_format_version_bump.sh`,
  `corec_corrupt_blob.sh` — a blob from another version, an older payload
  layout, or a truncated write is rejected, never decoded.
- `corec_derive_roundtrip.sh` — the derived AST codec round-trips real
  sources byte for byte (`kaic2 --cache-roundtrip-test <file>`).
- `corec_cspec_span_free_key.sh` — a body edit adding a lambda call hits
  the warm emit cache; a new top-level fn misses.
- `corec_monolithic_hit_stats.sh` — the monolithic path hits the shared
  `--core-cache-dir`.
- `corec_core_prune_flags.sh` — core pruning versus every cache the
  compiler can write.

## Wrapper and native objects (`wrapper_*`, `ncoreobj_*`, `nmodular_*`)

What `bin/kai` routes and caches around the compiler: core-cache hit
stats through the wrapper, c-modular routing and shared-cache soundness,
ThinLTO under `--release`, read-only caches, the cached native core
object (build mode, closure specialisations, the inline-bitcode
fallback), and native-modular object caching (toolchain identity, the
C compiler's identity, shared test roots, dotted-import routing).

`stress_nmodular_cold_pool.sh` builds cold native-modular projects under
CPU load; it runs from `make stress-nmodular-pool`, not tier 1.
