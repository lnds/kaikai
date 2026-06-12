# Phase A.0 cache invalidation fixtures (#452)

Four shell-based regression fixtures that pin the rejection paths of the
KAB1 cache header (`stage2/compiler.kai` `cache_read_header`) and the
content-addressable filename in `bin/kai prelude_cache_lookup`.

Each script seeds the cache directory with a deliberately broken `.kab`
blob (or a file whose name no longer matches the source sha) and then
invokes `bin/kai build` to confirm:

1. The compile completes successfully (the loader falls back to
   `load_prelude` on header mismatch — no fatal error).
2. The driver still emits a working binary (`empty.kai` prints `hi`).
3. Where applicable, `bin/kai` rewrites a fresh cache entry on the next
   run.

The fixtures are deliberately **opt-in** (each invokes `bin/kai` with
`KAI_PRELUDE_CACHE=1`) because the cache layer is off-by-default in
v1 of Phase A.0 — see `docs/lane-experience-issue-452-step3-4-5-6-driver.md`
for the reasoning and follow-up plan.

Each fixture is self-contained: it creates its own throwaway cache
directory under `/tmp`, leaves nothing behind in the user's
`~/.cache/kaikai/`, and exits 0 on success / 1 on regression.

Run individually:

    sh examples/cache/magic_mismatch.sh
    sh examples/cache/format_version_mismatch.sh
    sh examples/cache/kaikai_version_hash_mismatch.sh
    sh examples/cache/source_content_change.sh

Or in a batch:

    for f in examples/cache/magic_mismatch.sh \
             examples/cache/format_version_mismatch.sh \
             examples/cache/kaikai_version_hash_mismatch.sh \
             examples/cache/source_content_change.sh; do sh "$f" || exit 1; done

# Phase B user-file incremental cache fixtures (#455)

Six `userb_*.sh` fixtures pin the invalidation contract of the per-
project user cache (`<project>/.kai-cache/<content>-<dep>.kab`, written
by `stage2/compiler/user_cache.kai`). Unlike the Phase A fixtures, the
Phase B cache is wired end-to-end: each fixture builds a throwaway
multi-module project under `/tmp` with `KAI_CACHE=1` (the opt-in the
`bin/kai` wrapper reads to create the cache dir and pass
`--user-cache`).

Five are negative — they edit something and assert the build does NOT
serve stale decls:

- `userb_edit_source.sh` — edit the file's own bytes (content hash).
- `userb_edit_import.sh` — edit a direct import (consumer dep hash).
- `userb_edit_transitive.sh` — edit a two-hop import (cascade).
- `userb_version_bump.sh` — flip the blob's kaikai-version byte.
- `userb_corrupt_blob.sh` — truncate the blob past the header.

One is positive and is the load-bearing correctness gate:

- `userb_cold_warm_identical.sh` — the emitted C must be byte-identical
  across cold-cache, warm-cache (hit path), and no-cache (oracle)
  builds. selfhost byte-identity does not cover the cache (the compiler
  is built with it off), so this differential is what proves a cache
  hit reconstructs exactly the decls a fresh parse would.

These run in CI via `make test` (the `test-user-cache` target, also in
`test-fast`). Run them alone with:

    make -C stage2 test-user-cache
    # or: for f in examples/cache/userb_*.sh; do sh "$f" || exit 1; done
