# Lane experience — issue #1202 (CI leg): route shard-1 self-hosts through the cached c-modular path

## Scope as planned

`tier1-shard-1` ("costly self-compiles + caches") had drifted to 19–24 min
against a 35-min timeout (raised from 25 the day before), with ~25% of recent
main runs cancelled at the ceiling. #1202's root-cause leg (compiler source grew
~10%, the monolith amplifies it) is a separate compiler-perf investigation. This
CI leg's job: get shard-1 comfortably under 15 min *without weakening any gate*,
by routing the targets whose purpose is "build a compiler, then test something
else" through the already-measured cached c-modular path (#999 / PR #1014), and
persisting that .o cache across runs with `actions/cache`.

## What shard-1 actually runs — the inventory that drove the design

Per-phase durations, read from a real main run's CI log (run 29213488167,
Linux, same infra as the timeouts):

| Phase | CI wall | Class | What it builds → verifies |
|---|---|---|---|
| `test-costly-parallel` | 208s | (b) | 5× kaic2 over `main.kai` (33-line stub) + minimal fixtures in 5 dump modes → tokens/ast/types/env dump + check |
| `test-heap-limit` | 9s | (b) | small fixture → KAI_MAX_HEAP abort |
| `test-user-cache` | 39s | (b) | user-file cache invalidation fixtures |
| `test-core-cache` | 18s | (b) | core cache invalidation fixtures |
| **`test-modular-selfhost`** | **464s** | already c-modular | whole compiler via c-modular → links + `--version` + emits byte-identical C to single-TU |
| **`test-perceus-1131-modular-escape`** | **293s** | already c-modular | whole compiler via c-modular (`KAI_BACKEND=c`) → compiles escape shape byte-identically to single-TU |
| `demos-no-regression` | 108s | — | demos baseline |
| non-light tail (fmt/bench/check/…/info/doc) | 403s | mixed | assorted |

Total ≈ 1542s (~25.7 min). The two full-compiler self-hosts are **757s (49%)**
of the shard, and both already emit through `--emit=c-modular` — but both forced
`KAI_MODULAR_NO_CACHE=1`, recompiling all ~121 TUs from scratch every run. That
is the lever: the .o content-hash cache elides the `cc` phase on unchanged
emitted C.

The dump-mode set (`test-costly-parallel`) is misnamed by an old comment
("~4 GB RSS, ~20 s, whole compiler") — `SRC := main.kai` is the 33-line stub, so
those five run kaic2 over tiny inputs. They are already cheap; left untouched.

## The classification, and why the cache does not weaken the two self-hosts

Both self-hosts verify **the product of the modular build** — that it links, and
that the resulting binary emits C byte-identical to the single-TU compiler. The
content-hash cache keys each `.o` by a fold of {compiler, CFLAGS, arch, shared
headers, per-module defs, the emitted `.c`}. A hit reproduces the exact object a
fresh `cc` would — so:

- The **link always runs** (the cache only touches the per-TU `cc`), so the
  shared-pool RC guard is exercised on every run.
- The resulting compiler binary is bit-identical with or without cache; the
  byte-identity checks verify the same product.
- A stale hit is impossible by construction: any compiler change changes the
  emitted C → changes the hash → recompiles. The cache only skips recompiling
  C that is byte-identical to what was compiled before — and recompiling
  already-identical generated C exercises `clang`, not kaikai.

Verified locally: the cache-built modular compiler emits `portfolio.kai`'s C
byte-identically to single-TU kaic2, and all three gates
(`test-modular-selfhost` cold, warm, `test-perceus-1131-modular-escape`) pass
with the shared cache routed in.

Adversarial review (asu) found exactly one real crack the persistent cache
introduces that `NO_CACHE` did not have: a runner-image `clang` bump under a
key that hashes only the `cc` *path*, not its version, could serve an object
built by the old clang. Closed at the actions/cache layer, not inside `bin/kai`
(kept out of the in-flight #1204 territory): the actions/cache key folds
`clang --version`, so an image bump lands a fresh cache and the internal content
hash never sees cross-clang objects.

## Design decisions

- **Route both self-hosts to one shared build-local cache dir**
  (`stage2/build/cmodular-selfhost-cache`, gitignored, so the working-tree-clean
  check stays green). Within a run, `test-modular-selfhost` populates it and
  `test-perceus-1131-modular-escape` — same 121 TUs — starts warm off it.
- **`actions/cache` persists that dir across runs.** Key folds a compiler-source
  fingerprint (`hashFiles` over `stage2/compiler/**`, `stage2/main.kai`,
  `stdlib/**`, runtime headers) and the clang identity; `restore-keys` warm-
  starts from the nearest prior cache and lets the internal content hash decide
  which objects survive. actions/cache only ferries objects — it cannot produce
  a false green, because the internal key gates validity.
- **Left `test-costly-parallel` and the non-light tail alone.** They are not
  dominated by a recompilable `cc` phase; routing them buys little and the tail
  mixes gates that verify their own products (fmt self-host, doc/info).
- **Kept `KAI_BACKEND=c` on the #1131 gate** (the known trap: without it the
  wrapper routes `main.kai` to single-TU native and the link lacks the libLLVM
  shim).

## Measured

Local (macOS, arm64, 14 jobs, 121 compiler TUs): cold `compiled=119`, warm
`cache hits=121 compiled=0` — the cache eliminates the `cc` phase entirely on an
unchanged rebuild. Local warm *wall* does **not** drop, because on this machine
the `kaic2 --emit=c-modular` emit phase (~75s) dominates and `cc` is comparatively
cheap; the cache attacks `cc`. On Linux CI the split is inverted —
`test-modular-selfhost` is 464s there, far above the ~75s emit — so `cc`
dominates and the cache's win lands. The authoritative delta is the shard-1 wall
on this PR's CI vs recent main runs; it is reported in the PR body, not
projected from this machine (per the local-vs-CI split above, mac wall would
mislead).

## Fixtures

No new fixture: `test-modular-cache` (from #999) already pins the cache's
cold/warm/incremental correctness (a hit `.o` is diffed link-correct against the
single-TU golden), and the two self-hosts themselves are the byte-identity gates
that catch any cache-induced product change. This lane changes *how* they build,
not *what* they verify.

## Follow-ups

- The old #1202 root cause (compiler-source growth amplified by the monolith)
  remains open — this leg buys headroom, it does not fix the growth. The self-
  compile wall-time ratchet floated in #1202's plan is a natural next gate.
- If shard-1 still runs hot after this, `test-costly-parallel`'s five dump-mode
  runs over `main.kai` are the next slice to weigh, but they are cheap today.
