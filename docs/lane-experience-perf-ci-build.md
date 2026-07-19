# Lane experience — perf: faster local build + release cache

## Scope as planned vs shipped

Planned a five-lever perf pass (CI shard rebalance, `.o` cache
granularity, build trim, release cache, local `-O0` build) toward a
≤15-min CI target. Shipped three of the five — the three that are
verifiable locally or in isolation:

- **L4 `kaic2-dev` (-O0):** a dev-loop target compiling the bundle at
  `-O0` (~5 s cc vs ~70 s), emit byte-identical to `kaic2`. One-module
  rebuild ~107 s → ~37 s. `kaic2` stays the `-O2` trust path.
- **L3 release libLLVM cache:** factored the LLVM static prep into
  `mk/llvm.mk` (included by the root Makefile) so the release cache key
  hashes that file alone, not the whole Makefile — a test-target edit no
  longer invalidates the ~25-min cold build. Added `restore-keys` (same
  LLVM version → warm reuse) and a scheduled `warm-llvm-cache` workflow
  so a run of failed releases no longer leaves the cache empty.
- **L2 build trim:** shallow checkout + guarded clang install. Marginal
  (~15 s) — the build job is ~90 % irreducible kaic2 bootstrap.

## What did NOT ship, and why

- **L1 (`.o` cache granularity of the modular self-host):** the ~10-min
  `test-modular-selfhost` is the real CI bottleneck; its `.o` cache
  barely reuses across PRs. The invalidation is NOT a YAML key — the
  content hash lives in `bin/kai` and keys each `.o` by the emitted C,
  which is not hermetic per module (global tag/symbol numbering means an
  edit to one module perturbs another's emitted C). Fixing it is compiler
  work (make per-module C emission hermetic), not a cache-key tweak.
- **L5 (shard rebalance + `kaic2-fast` Abort-trap-6):** the shard split
  is only measurable in real CI runs, and the `kaic2-fast` crash is a
  heap/emit bug in the same c-modular full-compiler path as L1.

Both belong in a follow-up issue: they are the levers that actually reach
15 min, but they require compiler changes + CI iteration, not YAML.

## Structural surprise

The build job is not the lever the earlier analysis assumed. Measured:
336 of 372 s is the kaic2 `-O2` bootstrap, which §3 of
`docs/ci-time-analysis.md` already proved must stay `-O2`. So "build
6m → 4m" is not available; only the ~15 s overhead is.

## Fixtures / verification

L4 verified locally (byte-identical emit over `portfolio.kai`, runs
correctly). L3 verified structurally (include resolves, `make llvm-info`
works, YAML valid, keys byte-identical across the two workflows) but the
cache behavior only proves out on the next real release. L2 is YAML-only.

## Follow-ups

- Issue for L1 + L5: hermetic per-module C emission → effective `.o`
  cache → `test-modular-selfhost` 10 min → ~1 min; then cost-weighted
  shard rebalance; fix the `kaic2-fast` Abort-trap-6.
