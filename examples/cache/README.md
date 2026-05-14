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

    for f in examples/cache/*.sh; do sh "$f" || exit 1; done
