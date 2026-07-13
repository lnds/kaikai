# Lane experience — issue #989 (reopened): route the native default through the per-module object cache

## Scope

Issue #989 was already *closed once* by PR #1050 ("per-module separate
compilation with content-addressed object cache"). That PR shipped the whole
machinery — `emit_native_modular.kai`, the `BkNativeModular` backend, the
`--emit=native-modular` flag, `home_tu`/`KFn.mo` partitioning, root/leaf roles,
the runtime-owner link model, and even a full `test-native-modular` Makefile
target with a four-axis fixture. But the issue was **reopened and re-scoped**
because the machinery sat behind an opt-in (`KAI_NATIVE_MODULAR=1`) and did not
deliver the user-facing win it existed for:

1. **Warm build did not drop.** With the flag on, a warm `demos/poker/main.kai`
   build stayed at ~1.08 s — no better than whole-program's ~1.02 s.
2. **The cache was not incremental.** Adding one unrelated fn to the user file
   re-emitted *every* partition, not just the changed one.
3. **The default was not routed.** `kai build` still ran whole-program native.

This lane made native-modular the default and closed all three gaps.

- **Planned:** flip the default, cache the runtime object, fix the key so the
  cache is genuinely incremental, verify the six gates, wire CI.
- **Shipped:** exactly that. One new module (`emit_native_modkey.kai`), a
  refined key, a cached runtime `.o` in `bin/kai`, the default flip with a
  debug/no-libLLVM fallback, and the existing `test-native-modular` target wired
  into `tier1-native.yml` (it was written but never run by CI).

## Root cause of the warm-build residue — measured, not guessed

Decomposing a warm build with `bash -x` + timestamps localised the residue to
the *link*, not codegen:

| phase | time |
|---|---|
| `kaic2 --emit=native-modular` (whole-program frontend + cache-hit probe) | 0.33 s |
| link (`cc -O2` 12 objects **+ runtime_llvm.c from source**) | **0.72 s** |
| wrapper startup | 0.01 s |

The link was recompiling the ~1.9 K-LOC `runtime_llvm.c` at `-O2` on **every**
build. The whole-program native path avoids this by merging a pre-compiled
`runtime_llvm.bc` in-process; the modular path had no equivalent. Caching the
runtime as a `.o` (keyed on the runtime source + CC + CFLAGS + arch) once per
toolchain drops the link to **0.02 s**. Warm build: **1.08 s → 0.41 s**.

The honest residue at 0.41 s is the 0.33 s whole-program frontend (parse /
infer / perceus / mono), which is whole-program by Model A design and out of
scope here (per-module typecheck is #988). We land ~0.01 s over the 0.4 s
target; the remaining lever is a frontend one this lane deliberately does not
touch.

## The incremental gap — the key hashed the whole program

`nmod_cache_key` folded `nmod_dump_sigs(kp.fns, "")` — the signatures of
**every** program fn — into each partition's key. So adding one fn anywhere
changed the extern block of *every* partition's key, and all N re-emitted. The
cache was content-addressed but not *selectively* addressed.

Fix (`emit_native_modkey.kai`): a partition's key hashes the fns it DEFINES
plus the signatures of only the cross-TU callees those fns actually invoke,
discovered by walking the defined fns' blocks for `KCall` / `KTailCall` /
`KClosure` symbols. Walking `kp.fns` in program order to emit the referenced
sigs keeps the text deterministic regardless of callee-discovery order.

This preserves the anti-ABI-skew property intact: a callee's signature/ownership
flip still changes its dumped sig, which still flips the key of every partition
that calls it. Adding an *un*referenced fn no longer touches unrelated keys.

Measured on the fixture: a body-only edit now re-emits **1** object (14 cache
hits of 15); a `pick` signature change re-emits **2** (producer + consumer).

## Structural surprise — inlining makes "body-only" recompile the caller, correctly

A body-only edit to a *small* callee re-emitted TWO objects, not one. That is
correct, not a bug: the KIR inliner copies the small callee's body into its
caller, so the caller's emitted text genuinely changed and its content hash
must change. The one-object result only holds for a callee the inliner does
NOT expand (verified with a recursive callee: 1 object, caller reused). The
hash-is-over-emitted-content discipline gets this right without any special
case — the anti-skew property and the inliner compose for free.

## The default-flip fallbacks

Two cases stay on the whole-program native path, both load-bearing:

- **`--debug`** — the whole-program path emits a `.dSYM`; the partitioned link
  does not. Debug builds are not the warm-iteration hot path, so this is a clean
  split, not a gap to close here.
- **A kaic2 built without libLLVM** — `--emit=native-modular` prints the same
  "not built into this compiler" sentinel as `--emit=native`. The flip falls
  through to the whole-program block, which already degrades to the C backend
  with a note. Any *other* modular failure surfaces its stderr and still falls
  through, so a real user error is reported by the whole-program compile rather
  than swallowed (the whole-program path recompiles and prints it verbatim).

## Gates

1. **Byte-parity (serial):** all 13 `examples/multi-module` fixtures byte-
   identical modular==whole-program; `test-native-modular` asserts it on the
   four-axis fixture (mono / effects / cross-module fiber / call-chain).
2. **Incremental:** body edit re-emits 1 object, rest are cache hits.
3. **Ownership-flip:** a callee signature change re-emits its callers (≥2).
4. **User number:** warm `demos/poker/main.kai` = 0.41 / 0.41 / 0.41 s (from
   1.08 s). Cold 1.63 s; incremental 0.53 s.
5. **Cross-piece identity:** the #1033 cross-module region + fiber fixtures
   (shared arena stack / shared scheduler across objects) pass; a nullary-enum
   equality-across-modules fixture passes. Runtime singletons live in the one
   root owner; leaves declare them extern.
6. **CI:** `test-native-modular` added to `tier1-native.yml` shard 2 (was never
   run before). native-selfhost-gate baseline unchanged.

## Follow-ups (not opened as issues)

- **Routing `--backend=c` single files through c-modular+cache:** not trivial —
  the c-modular path already caches per-module `.o` but is behind `KAI_MODULAR=1`
  and splits a marker-delimited stream; wiring it as the C default is its own
  lane. Left for a follow-up per the brief.
- **The 0.01 s over target** is entirely the whole-program frontend; closing it
  needs incremental frontend work (#988 territory), out of scope for an
  emission-partition lane.

## Real cost vs estimate

Most of the machinery pre-existed (PR #1050), so the lane was diagnosis-heavy,
not build-heavy: measure where the warm residue actually was (link, not
codegen), find why the cache was not incremental (whole-program sig hashing),
and fix each with the minimum surface — one 54-LOC A++ module, a `bin/kai`
runtime-object cache, and the default flip. The trap avoided was "assume the
existing implementation works because #989 was closed"; measuring first showed
it did not meet its own gates.
