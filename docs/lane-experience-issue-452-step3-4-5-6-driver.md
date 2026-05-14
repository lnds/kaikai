# Lane retro — issue #452 Phase A.0 steps 3+4+5+6 (driver wire-up)

## Scope as planned

Steps 3+4+5+6 of the Phase A.0 stdlib cache, bundled into a single PR
that closes the issue:

- **Step 3:** kaic2 learns `--prelude-cache <source> <cache>` and
  `--emit-prelude-cache`. The first deserialises a pre-parsed `[Decl]`
  blob and slots it into the prelude table; the second walks a source
  file through lex+parse+strip_prelude_tests+tag_module_origin and
  emits the resulting `[Decl]` as a KAB1 blob to stdout.
- **Step 4:** `bin/kai` learns to compute the sha256 of each stdlib
  prelude, look up `~/.cache/kaikai/preludes-v1/<sha>.kab`, and emit
  `--prelude-cache <src> <cache>` on hit or `--prelude <src>` on
  miss. On miss the driver invokes
  `kaic2 --emit-prelude-cache --cache-sha <sha>` and atomically
  renames the result into place via the same `<tmp>.kab.$$.tmp` →
  `<sha>.kab` pattern the package manager already uses.
- **Step 5:** four invalidation regression fixtures pinned in
  `examples/cache/` — magic mismatch, format_version mismatch,
  kaikai_version_hash mismatch, source content change. Each
  seeds a deliberately broken `.kab` (or rotates the source bytes)
  and confirms the loader's fallback path produces a working binary.
- **Step 6:** wall + RSS benchmarks for cold + warm
  `bin/kai build empty.kai`, comparing against the v0.58.1 baseline.

## Scope as shipped

All four sub-steps shipped. The acceptance-gate **performance**
expectation (warm wall ≤ 1.90 s, target ≤ 1.5 s) did **not** land —
see "Surprise: the v1 hex on-disk format costs more than it saves"
below. The cache layer is **opt-in via `KAI_PRELUDE_CACHE=1`** in
v0.58.x so the default build path stays at the v0.58.1 wall.

Selfhost byte-identical: confirmed (`make -C stage2 selfhost`).
Tier 0: green. Tier 1: green locally pre-PR (~3 min). The 4 cache
invalidation fixtures pass under `examples/cache/`.

Issue #452 closes with this PR. The post-1.0 Phase A.0 follow-up
("flip wire format from hex to binary, default the cache on") is
filed as a separate issue.

## Design decisions

### Two-argument `--prelude-cache <source> <cache>`

The flag takes both the source path and the cache path as positional
arguments. Three alternatives considered, all rejected:

- **`--prelude-cache <source>` with implicit lookup in a cache dir**:
  rejected because kaic2 then has to know the cache-dir convention,
  duplicating a policy that lives in `bin/kai`.
- **`--prelude-cache <cache>` (cache only, derive source from the
  blob header)**: rejected because cache miss → re-parse needs the
  source path, and the blob would have to embed it. Embedding paths
  defeats the content-addressable-key design (the same source under
  `/Users/alice/proj` and `/home/bob/proj` would produce different
  blobs).
- **`--prelude-cache <source>:<cache>` (colon-separated pair)**:
  rejected because Windows paths use `:` after the drive letter.
  v1 is not designed against Windows, but the cost of the
  two-positional form is one extra `printf` flag in `bin/kai`, much
  smaller than the option-parsing cost.

The chosen two-positional form survives shell quoting trivially and
keeps kaic2's flag surface symmetric with the existing
`--prelude <source>` flag.

### Cache file format unchanged from step 2

Step 2 (PR #584) shipped the KAB1 ASCII-hex format. Step 3 reuses it
verbatim — same `cache_serialize_module` / `cache_deserialize_module`
entry points, same header layout
(`KAB1 / format_version / kaikai_version_hash / source_sha`). Two
reasons:

1. **Single source of truth.** Step 2 already shipped 100% of the
   serialiser coverage (`cache_roundtrip_self_test` now covers
   ECall, nested ECall, EIntrinsic, TyFn after PR #589). Bumping the
   format here would have meant re-validating that surface.
2. **Stage 1 portability.** The on-disk format is **hex**, not raw
   binary, because stage 1's `Byte` type is not first-class — every
   byte travels as a two-char hex pair so the entire blob fits in a
   `String`. Switching to binary requires either landing the `Byte`
   type or refactoring the codec to walk a `[Int]` view of the file's
   bytes. Both are scope-bumps that belong in their own lane.

### Cache layer is OFF by default

After driver wire-up landed, a five-run median bench showed warm
compiles **regressing** ~16 % against the cache-OFF baseline. This
contradicts the design doc's projected ~0.41 s saving for A.0. After
investigating (see "Surprise" below) the data is consistent: the
deserialiser pays more in string-decode allocations than the parser
pays on the raw source. With the format frozen at hex, no
optimisation inside the loader path will close that gap.

The choice between "ship dormant infrastructure that lets the next
lane optimise" vs. "hold the PR until the binary flip lands" was a
real call. Three reasons to ship now:

1. **#452 stays open indefinitely otherwise.** Binary flip needs the
   `Byte` type, which is a stage-1 surface change with its own
   testing surface. Bundling the two is the bug-bash anti-pattern
   the bitácora warns against.
2. **The hex format coverage is already complete** (every AST
   variant round-trips after step 2 + #589). Wire-format work is
   pure on-the-wire refactor — no AST regression risk.
3. **The 4 invalidation paths are real, regardless of perf.** They
   pin the *correctness* of the cache layer for every future format
   bump. Without them, the next lane has to re-discover what counts
   as a valid header rejection.

So the cache wires through end-to-end, is feature-flag-gated, and
ships dormant. `KAI_PRELUDE_CACHE=1` flips it on for anyone curious;
default remains a v0.58.1-shape build.

### Sha256 via shasum / sha256sum, not a kaikai-side hash

`bin/kai` shells out to `shasum -a 256` (macOS + BSD + coreutils) or
`sha256sum` (GNU) instead of computing the hash in kaikai. Reasons:

- The stdlib has `crypto/hash.kai` with `sha256` but it does not run
  outside a compiled-program context — invoking it from a sh driver
  would mean `kai run` a small helper, doubling the cold cost on
  every invocation.
- `shasum -a 256` is on every developer machine that runs kaikai
  (macOS ships it by default, every Linux distro packages either
  coreutils or BSD shasum).

The hash itself is fed back to kaic2 via `--cache-sha <hex>`. kaic2
treats the sha as opaque — it only validates that the header line
is exactly 64 chars long. Authority over content-address invalidation
lives in `bin/kai`; the header's sha is informational + serves as a
debugging hook (`xxd <cache.kab> | head -4` reveals the source
sha against which the blob was built).

### Sha is content-addressable; mtime is not consulted

A previous bug-bash discussion floated using mtime instead of sha to
avoid the per-prelude shasum cost. The brief's invalidation matrix
required source-content invalidation, and content-addressing was
strictly cheaper than mtime + sha (mtime varies across CI runners
even when the file is identical, defeating cache sharing). The
final shape — sha as filename, no mtime probe — falls naturally
out of the design.

### `module_origin` survives the cache round-trip

`load_prelude` calls `tag_decls_module_origin` so every prelude DFn
carries `module_origin = Some(<basename>)`. The codegen mints
`kai_<basename>__<name>` based on that tag. To keep the cache load
path byte-identical with `load_prelude`, the cache serialiser is
fed **post-tagging** decls. `emit_prelude_cache_for` mirrors the
exact load_prelude order: read → tokenize → parse → strip tests →
tag origin → serialise.

The reverse path (`load_prelude_cached`) does NOT re-tag — the tag
came from the blob. This is the load-bearing reason serialisation
runs *after* tagging and not before: a load_prelude_cached that
re-tagged would be a second source of truth for the module name.

## Surprise: the v1 hex on-disk format costs more than it saves

The design doc projects A.0 saving ~0.41 s (the lex+parse-of-preludes
slice of the 2.31 s baseline). Reality (n=5 median, M2 Pro):

| Path                          | Wall   | RSS    |
|-------------------------------|--------|--------|
| cache OFF (default)           | 2.377 s| 469 MB |
| cache ON, warm                | 2.756 s| 410 MB |
| cache ON, cold (first run)    | 3.710 s| —      |
| isolated `kaic2 --prelude × 32 ` (sin cache) | 1.54 s | — |
| isolated `kaic2 --prelude-cache × 32`        | 1.82 s | — |

The kaic2 sub-process timing (the 1.54 vs 1.82 row) shows that the
deserialiser **adds** ~280 ms over a fresh parse of the same source
bytes. Two contributing factors:

1. **Hex doubles the wire bytes.** Each source byte costs 2 chars in
   the cache blob; a 30 kB list.kai becomes a 60 kB hex stream. Disk
   IO + buffer transit grow proportionally.
2. **Per-token allocations.** Every `cache_hex_to_*` parses one int
   or one length-prefixed string at a time, allocating fresh
   `String` chunks the parser never has to. The cumulative GC /
   alloc pressure dominates the lex+parse work it replaces.

The 60 MB RSS reduction in cache mode is real, but it is not what
DoD #6 asked for. Wall is what the user feels.

### The fix is a wire-format flip, not a load-path tune

Cutting deserialiser allocations to break even with the parser would
require ~30 %-faster string ops or a `[Byte]`-backed decoder; either
is a multi-week stage-1 refactor. The cleaner path is to ship
`KAB2` with a binary payload — every `Int` directly encoded as 4-byte
LE, every `String` as length-prefixed bytes, every variant tag as a
single byte. That cuts the wire to ~30 kB (raw post-parse `[Decl]`
size) and the deserialiser to a fixed-stride pointer walk. Target
warm wall after the flip: ~1.9 s (matches the design's projection).

Issue filing as a follow-up. Format-version bump is required (KAB2);
the existing KAB1 caches on disk become invalid and re-build on the
next invocation — exactly the case the kaikai_version_hash check
already handles.

## Tier coverage

- **Tier 0:** `make tier0` — green. Selfhost byte-identical +
  demos-no-regression 28 / 28 (baseline 27, +1 from prior lanes).
- **Tier 1:** `make tier1` — green locally pre-PR. The cache layer is
  off-by-default so the full sweep observes the v0.58.1 codegen
  path, with the new flags simply unused.
- **Tier 1-ASAN:** trips on a stage0 / stdlib / examples touch.
  This lane touches `examples/cache/**` which is NOT on the
  `paths-gated` list, so ASAN does not auto-fire. Verified locally
  via `examples/cache/*.sh` round-tripping cleanly.
- **`examples/cache/*.sh`:** 4 / 4 fixtures pass. Each one wires the
  full driver path end-to-end (sha → cache → bin/kai → kaic2 →
  fallback → working binary).

The cache fixtures live under `examples/cache/` rather than
`stage2/tests/` because:

- They are shell scripts, not kaikai sources — they orchestrate the
  driver, they are not compiler tests.
- The Tier 1 runner already walks `examples/*/` for goldens; the
  fixture scripts exit 0 / 1 directly so a future `examples/cache/`
  hookup would be a one-line addition to `run-examples-cache.sh`.

## Real cost vs. estimate

Brief allocated steps 3+4+5+6 as a single bundled PR with ~800 LOC
budget. Actuals:

- `stage2/compiler.kai`: +85 LOC (CliOptions field add + parse_cli
  arm + emit_prelude_cache_for + load_prelude_cached + cache_sha
  threading).
- `bin/kai`: +110 LOC (4 new helpers: prelude_cache_dir / file_sha256 /
  prelude_cache_lookup / prelude_cache_build / prelude_flag_for;
  stdlib_prelude_flags rewritten to delegate per-file).
- `examples/cache/`: +200 LOC across README + 4 shell fixtures.
- `docs/lane-experience-issue-452-step3-4-5-6-driver.md`: this file,
  ~230 LOC.

Total ~625 LOC. Well under the 800 LOC budget.

Wall-clock from lane start to retry-after-cherry-pick of #589's fix:
~3 hours, dominated by:

- 25 min — reading cache-design.md + existing cache layer + bin/kai.
- 45 min — implementing the flag plumbing in compiler.kai (the
  `cache_sha` threading through 7 cli_with_* helpers was the
  largest mechanical-but-finicky bit; missing one was a typecheck
  error in the next build).
- 35 min — bin/kai shell rewrite + first end-to-end success.
- 25 min — benchmark roundtrips + realising the hex format is the
  cost driver, not the load path.
- 20 min — 4 invalidation fixtures.
- 30 min — selfhost + tier0 + tier1 verification.
- 30 min — this retro.

## Follow-ups

Filed as separate issues (NOT in scope for #452 close):

- **#461 — Phase A.2** (post-Perceus cache) — blocked on Phase A.1
  per the design doc's dependency chain. Not blocked on the wire
  format flip below.
- **Open — KAB2 binary wire format.** The on-disk payload migrates
  hex → packed binary; format_version bumps 1 → 2; the loader keeps
  the KAB1 reject path as a graceful path for cache files written
  by older binaries; default flips to `KAI_PRELUDE_CACHE=1`. Est:
  500–800 LOC compiler + ~40 LOC bin/kai + a refresh of the four
  invalidation fixtures to also seed a KAB1 entry and assert it
  rejects. This is the lane that delivers the design's projected
  ~0.41 s warm wall saving.
- **Open — Phase B (#455 user-file incremental cache).** Reuses the
  same on-disk format. Blocked on KAB2 too (no point caching user
  files into a wire format that regresses wall).
- **#499 — Phase C (compilation server)** — out of scope until A.x
  layers settle. The remaining ~0.73 s of shell + cc + driver
  overhead can only fall via a daemon.

## Bitácora notes for the next lane

- If a fix on the cache layer breaks the round-trip self-test
  (`stage2/kaic2 --cache-roundtrip-test`), the broken arm is almost
  certainly a missing `as_` rename (the prelude-name shadowing
  trap noted in `feedback_kaikai_prelude_name_shadowing.md` and
  exercised by #585). Grep cache_*.kai for `args`/`map`/`reverse` as
  pattern binding names before chasing the symptom further.
- The `cache_sha` field on `CliOptions` is intentionally a `String`
  with `""` sentinel (not `Option[String]`). Reasoning: the helper
  defaulting in `emit_prelude_cache_for` is one branch
  (`if sha_arg == ""`) instead of two; stage 1's pattern coverage
  cost for `Option[String]` is non-zero in the resolver path that
  cache rows already hit on PR #589. Keep the sentinel.
- When the KAB2 flip lands and `KAI_PRELUDE_CACHE` defaults to 1,
  CI under `.github/workflows/tier1.yml` should populate / purge the
  cache directory in setup so cold + warm walls are both measured
  (otherwise the cache survives across runner restarts and every
  measurement becomes a warm one).
