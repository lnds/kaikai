# Lane experience — issue #999: parallel cc + .o content-hash cache for c-modular

## Scope as planned

The backend incremental lever. Compiling the stage-2 compiler through
`--emit=c-modular` partitions into ~86 translation units; the wrapper
(`bin/kai`, `KAI_MODULAR=1` branch) compiled each `.c` to its own `.o` in a
**sequential** for-loop, then linked. Two changes were asked for:

1. Fan the per-module `cc -c` out in **parallel** (one `cc` per core).
2. Cache each `.o` by **content hash** so editing one module recompiles that
   one `.o`, not all 86.

The default single-TU C path must stay byte-identical (selfhost gate); the
frontend stays whole-program (no `.kaii`, out of scope — that is #988).

## Scope as shipped — and the surprise the brief did not anticipate

The brief's first deliverable was "verify #990's c-modular link works." It does
**not**, on this machine (macOS arm64), and would not on x86-64 Linux either —
but for a reason #990 never touched. #990 fixed the 6 undefined `void *`
cross-module symbols (linkage *correctness*). The compiler's modular self-compile
still fails the link with a *size* error:

```
ld: ADRP out of range … ('__kai_default_node_stdout')
    __bss size=0x1d3a89ba8   (~7.3 GiB)
```

Root cause (filed as **#1012**): `stage2/runtime.h` defines the RC free-list
pools as file-local `static` arrays —
`kai_var_block_pool[9][1048576]` (75.5 MB), `kai_slot_pool` (75.5 MB),
`kai_cell_pool` (8 MB). `runtime.h` is `#include`d by every modular TU, so the
single-TU build has one copy (~159 MB, links) but the 86-TU split has **86
copies ≈ 13.6 GB of `.bss`**, overflowing the arm64 small-code-model ADRP reach.
Confirmed from the linker map: `_kai_var_block_pool` appears once per object,
each `0x04800000`, at distinct addresses (internal linkage → one per TU). The
#999 measurements never hit this because they timed `cc -c`, never the link.

Per the lane-discipline call (one worktree fixes one thing; blockers get their
own PR, as #990 did), this was **separated**: #1012 carries the `runtime.h` fix
(macro-guard the pools so the default path is verbatim-`static` and the modular
build shares one owner-TU copy). This lane ships the parallel-cc + cache lever,
validated end-to-end on the multi-module fixtures that link and measured on the
real 86-TU compiler **compile** phase.

## Design decisions

**Cache key — content hash, two levels.** A per-build `common` digest folds the
compiler, its flags, `uname -sm`, and every shared header in the split dir
(`kai_types.h`, `kai_decls.h`, `runtime.h`). Each module's key is
`hash(common + module.c)`. So a shared-header edit (any `pub` signature change,
which lands a new prototype in `kai_decls.h`) correctly invalidates **every**
`.o`; a body-only edit (same signature) changes one `.c` and recompiles one
`.o`. This header-awareness is a correctness property, verified in the fixture.

**Why content hash, not mtime.** The cache must survive `$tmp` (a fresh
`mktemp -d` per build) and must not recompile when a regenerated `.c` is
byte-identical. Timestamps satisfy neither; a `make`-style timestamp graph in
`$tmp` would re-fire every build. Content keying also dedups two modules whose
generated `.c` is identical (observed: 16 TUs → 15 cache entries; the second
identical TU reuses the first's just-stored `.o` within the same build).

**Parallelism — `xargs -P N -n 1`, vars via env.** One `cc` per file, fanned
out across `min`-free cores. The worker reads `CC`/`CFLAGS`/includes/cache from
exported `KAI_MC_*` env so the command template carries no filename placeholder
— `xargs -I {}` would textually substitute inside the script and `-I` collides
with substrings like `CFLAGS`. The trailing input file lands as `$1`. A miss
stores atomically (`cp` to a pid-suffixed temp, then `mv`); a store failure is
non-fatal (next build recompiles).

**Counting — derive hits, count only misses.** First cut counted hits and misses
with per-worker marker files; a fast all-hit burst on the CI Linux/dash runner
dropped a few of the 20 concurrent marker writes (16/20), so the warm assertion
flaked there but not on macOS. Switching markers → atomic stdout lines did not
help (same 16/20), which ruled out the filesystem and pointed at the bursty
concurrent writes themselves. The fix is to stop counting the fast path at all:
a miss (slow, `cc`-gated, spaced) prints one `M`; **hits are derived as
`TUs − misses`**, exact by construction. The fixture then asserts on `compiled`
(the reliable metric and the real cache-win signal: a no-change rebuild
recompiles zero), never on the hit tally.

**Cache location + knobs.** Default `…/Caches/kai/cmodular` (mirrors
`resolve_cache_root`), content-keyed so it is safe to share across projects.
Knobs: `KAI_MODULAR_JOBS` (parallelism), `KAI_MODULAR_CACHE_DIR` (location),
`KAI_MODULAR_NO_CACHE=1` (off, still parallel), `KAI_MODULAR_STATS=1` (one-line
hits/compiled summary — also the fixture's assertion source).

**Hasher detection.** `sha256sum` → `shasum -a 256` → `cksum`, first field is
the digest; absent → cache disabled (still parallel). Covers macOS + Linux.

## Measured (this machine: arm64, 16 cores, `-O2`, 86 compiler TUs)

| scenario | wall |
|---|---|
| sequential `cc -c` ×86 | 51.06s |
| parallel `-j16`, cold cache | 10.65s — **4.8×** |
| warm rebuild (no change) | **0.18s** (all hits, pure `cp`) |
| incremental: small leaf edit (`option.c`) | 0.34s |
| incremental: `infer.c` edit (the `-j` floor) | 9.24s |

Ratio matches the issue's prediction (88.8s → 20.1s, 4.4×, on a different
machine). The `-j` floor is the largest TU: `infer.c` (19.4k lines) gates the
parallel wall, so editing *it* costs ~9s while a typical leaf edit is sub-second.

## Fixtures

`stage2/Makefile: test-modular-cache` drives the `bin/kai` `KAI_MODULAR` path
(not the Makefile's own split loop) and pins three cache states from the
`KAI_MODULAR_STATS` summary, each diffed against the single-TU golden so a
cache-hit `.o` is proven link-correct:

- **cold** — empty cache → `compiled + hits == total`, `compiled > 0`.
- **warm** — no change → `compiled == 0`, `hits == total` (the cache win).
- **incr** — body-only edit (`square` `= n*n` → `{ let r = n*n; r }`, same
  signature, same output) → recompiles strictly fewer than cold.

Wired into `TEST_LIGHT_TARGETS` (tier1 light fan-out, auto-sharded) next to
`test-modular-build`, `KAI_MODULAR_JOBS=4` so the fan-out does not oversubscribe.

Coverage gap: the fixture is 16 TUs, not the 86-TU compiler — the compiler-scale
**link** is gated on #1012, so the headline self-compile is measured (compile
phase) but not yet a green link in CI. That fixture lands with #1012.

## Real cost vs estimate

Most of the lane was the unbudgeted root-cause hunt for the link failure (linker
map → `size`/`nm` → the `static` pool). The actual feature — the helper trio, the
worker, the fixture — was small and went in cleanly. The default-path
byte-identity invariant cost nothing: every change is confined to the
`KAI_MODULAR` branch + new helpers only it calls; `runtime.h` is untouched, so
selfhost is unaffected by construction.

## Follow-ups

- **#1012** — share the runtime pools so the compiler's modular link succeeds;
  then add the 86-TU self-compile link to CI and retire this lane's coverage gap.
- **Raise the `-j` floor.** `infer.c`/`emit_c.c` (19.4k/13.3k lines) gate the
  parallel wall at ~10s; splitting `infer.kai`/`emit_c.kai` would lower it.
  Worth its own issue.
- **Smaller mutable statics.** The RC stats/profiling counters in `runtime.h`
  also duplicate per-TU under modular (debug-only state, harmless to the link);
  fold them into the same owner/extern switch as #1012 for semantic cleanliness.
