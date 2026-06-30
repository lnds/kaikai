# Lane experience — issue #1012: share RC free-list pools across c-modular TUs

## Scope as planned

`stage2/runtime.h` declares the RC free-list pools as file-local `static`
arrays (~159 MB total). `runtime.h` is `#include`d by every modular TU, so the
~86-TU compiler self-compile replicates them into every object → multi-GB
`.bss` → arm64 small-code-model ADRP overflow at link time. Plan (settled in
the issue): macro-guard the pools so the default single-TU build is
byte-identical and the modular build shares one copy. Pass
`-DKAI_SEPARATE_COMPILATION=1` to all module compiles and `-DKAI_RUNTIME_OWNER=1`
to exactly one TU. Wire a CI fixture proving the full compiler links modularly.

## Scope as shipped

- **`stage2/runtime.h`** — three pools guarded with the 3-way switch
  (`KAI_SEPARATE_COMPILATION` → `extern` in every TU, `+ KAI_RUNTIME_OWNER` →
  one definition; `#else` → original `static` verbatim):
  `kai_cell_pool` (8 MB), `kai_slot_pool` (75.5 MB), `kai_var_block_pool`
  (75.5 MB). **Each pool's index counter (`*_pool_n`) moves with its array** —
  see Surprise 1.
- **`bin/kai`** — the `KAI_MODULAR` branch passes `-DKAI_SEPARATE_COMPILATION=1`
  to every module compile and `-DKAI_RUNTIME_OWNER=1` to the `__root__.c` TU
  (fallback: the first `.c`, so there is always exactly one owner). The modular
  defines fold into the `.o` content-hash cache key (shared part) and the
  per-file owner def folds into the per-file key, so a stale cross-variant `.o`
  can never be reused. Also: the modular link now carries the same
  `-Wl,-stack_size,0x8000000` the single-TU compiler link uses on Darwin (see
  Surprise 3).
- **Fixture** — `test-modular-selfhost` (stage2/Makefile, delegated from the
  root Makefile), wired into `tier1` and `tier1-shard-1` with the coverage
  invariant updated. Builds `stage2/main.kai` through `bin/kai`'s KAI_MODULAR
  path and smokes `--version`.

## Verification

- **Byte-identity (the safety net):** `make -C stage2 selfhost` →
  `kaic2b.c == kaic2c.c`. The default path takes the `#else` branch, which is
  the original `static` lines token-for-token (verified directly with
  `cc -E -P` on the guard in all three macro states). runtime.h does not appear
  in emitted C, so selfhost is structurally immune anyway — but the gate is
  green, confirming the default build is unchanged.
- **Before:** `KAI_MODULAR=1 ./bin/kai build stage2/main.kai` → `ld: ... ADRP
  out of range ... __bss size=0x1d3a89ba8` (~7.3 GiB).
- **After:** same command links; `__bss` is ~842 MB (one shared pool copy +
  the per-TU non-pool statics that still duplicate, harmlessly for size);
  `--version` prints `kaic2 stage 2 (self-hosted)`.
- tier0 green except `test-heap-limit`, which fails only because macOS lacks
  `timeout` (known environment gap, not a regression).

## Design decisions / alternatives

- **Array + counter as an indivisible unit.** The issue sketch guarded only the
  arrays and called the smaller mutable statics "duplicate harmlessly." That is
  true for the RC stats counters but **false for the `*_pool_n` index
  counters** — see Surprise 1. They move with their arrays.
- **Slab bump-allocator left per-TU.** `kai_slab_*` (a few pointers/ints, not a
  size problem) stays `static`. Correct under a shared block pool: a block from
  any TU's slab is valid memory once pooled; the free path never `free()`s an
  individual slab-interior pointer (spills are dropped, slabs reclaimed at each
  TU's own `atexit` teardown), so the shared pool holding pointers into several
  TUs' slabs is sound. Sharing it would have been a larger, unnecessary diff.
- **Owner = `__root__.c`.** The emitter always names the root TU `__root__.c`
  (`emit_c.kai`), and the linker error already pointed at `__root__.o`. The
  fallback-to-first-`.c` guarantees exactly one owner even if that ever
  changes — zero owners would fail loudly at link (undefined pool symbols),
  never silently.
- **Defines in the cache key.** Folding `mod_defs` into the shared key and the
  per-file owner def into the per-file key keeps the #999 `.o` cache correct
  across the macro change; the new runtime.h content already invalidates every
  stale entry, but the keying is now explicit rather than incidental.

## Structural surprises

1. **The `*_pool_n` counters are load-bearing, not stats.** The pool helpers
   (copied into every TU as `static`) do `pool[n][--counter]` / `pool[n][counter++]`.
   Share the array but leave the counter per-TU and TU-A's counter indexes the
   shared array while TU-B's (different value) indexes the same store →
   double-allocation / aliasing → corruption. Proven by reasoning, then the
   implementation keeps array+counter together. The "duplicate harmlessly"
   note in the issue applies to the RC stats counters, not these.

2. **The link fix is necessary but NOT sufficient — a second, independent bug.**
   With the pools shared the full compiler links and `--version` runs, but
   compiling *any* program SIGSEGVs in `kai_free_value` during `load_prelude`'s
   name-collision validation. Isolated to: modular-only (single-TU is fine),
   full-compiler-only (small modular fixtures incl. a higher-order one are
   fine), and **independent of the pools** (crashes with `-DKAI_NO_CELL_POOL=1`,
   pure malloc) **and of the stack** (crashes at 128 MB). It is an RC/codegen
   divergence the modular split introduces, surfacing only at the self-compile's
   scale — masked until now because the link never succeeded. Filed as **#1016**
   with the repro, ruled-out list, crash site, and a hypothesis. The fixture is
   scoped to what #1012 actually delivers (link + `--version`), not a full
   program compile, precisely because of #1016.

3. **The modular link forgot the big stack.** The single-TU compiler links with
   `-Wl,-stack_size,0x8000000` (the compiler is deeply recursive; 8 MB is not
   enough for deep compiles on macOS, where the main-thread stack is fixed at
   link time). `bin/kai`'s modular link did not. Added it (Darwin-guarded,
   mirroring the Makefile's `LDFLAGS_STACK`). It does not fix #1016 — but once
   #1016 is fixed the modular compiler needs it to run real compiles, so it
   belongs with "the modular link produces a working binary."

## Fixtures added / coverage gaps

- **Added:** `test-modular-selfhost` — full-compiler modular link + `--version`
  smoke; in `tier1` / `tier1-shard-1`. Without the shared-pool guard the build
  fails at LINK, so a regression of this fix trips the gate.
- **Gap (tracked by #1016):** no fixture yet that the modular-built compiler
  *compiles a program and diffs* — blocked by the runtime crash. When #1016
  lands, extend `test-modular-selfhost` to compile a hello fixture and diff
  against the single-TU golden.

## Cost vs estimate

The mechanical fix (runtime.h guard + `bin/kai` defines) was ~30 min as
estimated. The bulk of the lane went to **diagnosing Surprise 2**: building
no-pool and stack-flag variants, single-TU controls, and a minimal higher-order
modular fixture to prove the crash is neither the pools, nor the stack, nor a
generic modular-codegen issue. Worth it — it converts "the fixture should
compile + diff" (which would have failed CI) into a correctly-scoped gate plus a
well-characterized follow-up.

## Follow-ups

- **#1016** — the modular self-compile runtime crash (the real next blocker for
  #999's end-to-end measurement).
- Move the RC stats / profiling counters (and optionally the slab state) to the
  same owner/extern switch for semantic cleanliness — harmless duplication
  today, noted in the issue as a follow-up.
- When #1016 closes, upgrade `test-modular-selfhost` to compile + diff a fixture.
