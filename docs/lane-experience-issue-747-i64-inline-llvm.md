# Lane experience — #747 i64-inline + variant fast-path on the LLVM backend

**Issue:** #747 — rb-tree LLVM backend: parity vs Koka/C — Int leak (3.7 GB
RSS) + missing i64-inline.

**Scope as planned (issue body):**
- [ ] Emit decref for out-of-cache boxed `Int` in the LLVM backend (the
  3.7 GB leak).
- [ ] Port Lane B i64-inline (kind-1 raw binders) to the LLVM codegen so
  `Int` rides raw `i64` inside fibers, matching the C backend.
- [ ] Add a parity fixture: LLVM vs C rb-tree must agree on `size`/`height`
  and the LLVM RSS must land within a sane factor of the C RSS.

**Scope as shipped:**
- The Int leak was already closed before this lane (PASO 1 / PR #756, the
  unified runtime + RC-in-match port). At lane start the LLVM RSS was
  already in parity (55.4 vs 55.2 MB at N=1M); the issue body described the
  pre-#756 state.
- i64-inline ported in full: construction (`kaix_variant_masked` +
  `[n x i64]` slot buffer + int-only `slot_mask`), match-arm read
  (`kaix_variant_arg_i64`, direct `.i64` load), reuse-in-place
  (`kaix_variant_at_masked` in the TRMC step), and the literal-Int pattern
  TEST (`icmp eq i64` on the raw word, no re-box).
- Beyond the issue: routed the fast variant ctor path
  (`kai_variant_u_fast` + startup payload-ctor registration) and the fast
  nullary ctor path (`kaix_nullary_fast` array-load + startup nullary seed),
  because profiling showed the cold `kai_variant_u` preamble — not the box
  round-trip — dominated the residual gap once i64-inline landed.
- Two parity fixtures: `test-issue-747` extended to assert the raw
  representation (the IR must use `kaix_variant_arg_i64` + `kaix_variant_masked`
  and `balance_left` must carry no `kaix_to_int` box round-trip), and a new
  `test-issue-747-mixed-slots` (`Int+Real+enum+ptr` variant) proving the
  `k == 1` gate packs ONLY the Int slot raw.

**Result (callgrind, N=100K, Docker, deterministic):**

| stage | instructions | vs kaikai-c |
|---|---:|---:|
| LLVM baseline (lane start) | 1,331.93M | 7.23x |
| + i64-inline | 1,156.11M | 6.27x |
| + variant_u_fast + nullary_fast (production `-c`) | 897.15M | 4.87x |
| + the above, `llvm-link` merged module | 534.87M | 2.90x |
| kaikai-c (reference) | 184.27M | 1.00x |

−60% instructions (1,331.93M → 534.87M with a merged module; −33% in the
production separate-compile path). Wall at N=1M dropped 3.40x → 3.04x C.
Correctness exact (height 23 @ 100K, 29 @ 1M, == C on both backends), RSS
in parity, selfhost byte-id (both `kaic2` and `--emit=llvm` self-determinism).

**Design decisions and alternatives considered:**

1. **Option A (Int-only raw) vs Option B (Int+Real+enum raw).** Took A:
   ONLY kind-1 Int slots ride raw; Real (kind-2) and enum (kind-3) stay
   boxed exactly as the legacy path laid them out. An enum nullary slot is
   an interned immortal singleton — raw-packing it buys no measurable wall
   on the rb-tree (the Color slot is a pointer chase to a hot, immortal
   cell, not an allocation). B would have reopened the offset bug-class
   (#741/#747) for zero benefit on this benchmark. The single source of
   truth is `llvm_is_int_slot(t) = variant_slot_kind(t, []) == 1`, used by
   construction AND every read site, so they can never diverge — this
   mirrors the unbox pass's own criterion (`ctor_int_slots_of`).

2. **Homogeneous `[n x i64]` buffer + `ptrtoint`, not per-slot `bitcast
   %KaiValue** -> i64*`.** `KaiVarSlot` is one machine word; the i64 buffer
   aliases it exactly. The homogeneous buffer keeps the opaque-pointer IR
   from aliasing a cell interior across two element types (TBAA-safe under
   -O2). asu's call.

3. **`kai_variant_u_fast` + startup payload-ctor registration.** The first
   i64-inline cut only −13%; profiling revealed the cold `kai_variant_u`
   preamble (varname/slotmask register + nullary probe + immortal-args
   262144-bucket hash scan) was 34.8% of all instructions. Routing the
   masked ctor to `kai_variant_u_fast` (alloc + stores only) and stamping
   tag→name/mask once at startup (`_kai_proto_init_llvm`) was the bigger
   win.

4. **`kaix_nullary_fast` (array load) for Red/Black/RBLeaf.** The cold
   `kai_variant_u` nullary path was another 23%; mirror of the C backend's
   `kai_nullary_fast` (read the interned singleton from `kai_enum_by_tag`,
   seeded at startup).

5. **NOT broken: IR opacity.** Emitting cell-interior GEP+load directly in
   the IR (to inline the shim ops) would close more of the gap but reopen
   the runtime-mint-vs-emitter-read offset bug-class. Vetoed; the residual
   shim-boundary cost is the price of "one runtime, two ABIs, opaque IR".

6. **NOT shipped: `always_inline` / `-flto` / `llvm-link` in production.**
   The C99 non-static `inline` model is a minefield for the
   external-symbol-the-`.ll`-calls contract; merging the shim module into
   production (`-flto`) does not move the wall on Mac (cache/RC-bound) even
   though it cuts −33% instructions. That is a build-chain lane, measured on
   Linux, not part of the i64-inline codegen port.

**Structural surprises the brief did not anticipate:**
- The leak was already fixed; the live gap was pure wall/instruction.
- The dominant cost after i64-inline was NOT the box round-trip but the
  cold-path constructor preamble. The issue body framed i64-inline as the
  whole gap; it was ~13% of it.
- `LlvmEmit` had no `enums` field (the C backend's `EmitCtx` does). Without
  it, kind-3 enum slots misclassify and the `slot_mask` diverges intra-LLVM.
  Threading `enums` (one field, mirrored on `regions`) was the P0 prereq —
  about 60 mechanical edits across the record-literal reconstructions and
  the body-emit threading chain.
- The residual gap is structural: the LLVM `insert_loop` body costs ~196M
  instructions @ 100K — more than the entire C program (184M). The shim
  call boundary, not the value representation, is the remaining wall.

**Fixtures added:**
- `test-issue-747` (extended): asserts raw representation in the IR.
- `test-issue-747-mixed-slots` + `examples/perceus/llvm_mixed_int_real_slot.kai`
  + `.out.expected`: the coherence gate for the `k == 1` predicate.

**Coverage gaps / follow-ups left for next lanes:**
- Production link should merge the shim module (`-flto` / `llvm-link`) to
  realize the −33% the merged-module path already shows; measure on Linux
  where wall is not cache-masked. Build-chain lane.
- The `insert_loop` body codegen is structurally heavier than the C
  backend's even fully inlined; closing that needs either shim inlining via
  a build-chain change or accepting the ~1.5x-C design ceiling (the same
  ceiling the C backend hits vs hand-written C).
- The benchmark `run.sh` LLVM link line needed `-I stage2 -I stage0` (it
  had only `-I stage0`, so `kai_intf` from the unified stage2 runtime did
  not resolve) — fixed in this lane; the unified-runtime PASO 1 left it
  stale.

**Real cost vs estimate:** the i64-inline port itself was straightforward
(mirror emit_c); the time went to (a) re-applying the emit_llvm changes
after an external mid-lane revert wiped the `enums` threading + construction
path, and (b) the profiling round that revealed the cold-path preamble as
the real dominant cost, redirecting the lane from "port i64-inline" to
"route every rb-tree ctor to the fast path".
