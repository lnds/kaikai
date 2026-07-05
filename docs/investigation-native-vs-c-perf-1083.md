# Native-vs-C codegen perf investigation (#1083)

**Status:** diagnostic spike — root cause localized, fix plan proposed, nothing
implemented. Read-only over `stage0`/`stage2`.

## The fact under investigation

rb-tree 1M, same `.kai`, explicit backends (measured, triple-verified upstream,
reconfirmed here interleaved+warm on Darwin arm64, `default<O2>`):

| backend | instructions | wall (median) |
|---|---|---|
| `--backend=c` | 2.51 G | ~0.30 s |
| `--backend=native` (DEFAULT) | 15.04 G (**6×**) | ~0.83 s (**~2.7×**) |

The front-end is shared (lex/parse/infer/perceus/monomorph identical). The only
delta is codegen: `emit_c.kai` (→ C → `cc -O2`) vs `emit_native*.kai` (in-process
libLLVM). Same KIR in.

## Root cause (proven causally)

**The native backend emits its functions with NO `target-cpu` / `target-features`
attributes. The vendored runtime bitcode (compiled by `clang`) carries them.
LLVM's `areInlineCompatible` refuses to inline a callee whose target-features are
not a subset of the caller's — so the inliner refuses to inline EVERY runtime op
(`kaix_int`, `kaix_variant_arg`, `kaix_int_field`, `kaix_internal_dup`, …) into
the hot path. The un-inlined per-op call chain is the 6× instruction blow-up.**

This is not the missing bitcode link (P2, #854). P2 is active and working: the
runtime bodies are physically merged into the module (`nm` shows every `kaix_*`
as a defined `t` symbol, 0 undefined). The bodies are present and inlinable
(`kaix_int_field` is 5 instrs, `kaix_internal_dup` 13, `kaix_int` 16, none carry
`noinline`/`optnone`). The optimizer simply won't touch them because of the
attribute mismatch.

### The evidence chain

Same rb-tree `insert_loop`, disassembled from each final binary (`otool -tV`):

| | C backend | native backend |
|---|---|---|
| `bl` (calls) in insert_loop | 74 (coarse: `kai_slab_alloc`, `kai_variant_u_fast`, `kai_free_value`) | **390** (fine per-op: `kaix_variant_arg` ×74, `kaix_int` ×70, `kaix_variant` ×59, `kaix_int_field` ×48, `kaix_internal_dup` ×44) |

The C path inlines the fine ops (`cc -O2` sees `runtime.h` as `static inline` in
the same TU); the native path leaves them as calls. Where C does
`kai_var_slots(_scr)[2].i64` (one inline load), native does
`call kaix_variant_arg` (box) → `call kaix_int_field` (unbox) — two calls per
field read. Where C reconstructs a node with raw i64 slots, native re-boxes each
scalar via `call kaix_int`.

Why the in-process optimizer won't inline them — isolated step by step, all on the
**same** linked+internalized module:

| optimizer invocation | residual `kaix_*` defs | residual `kaix_*` bl-calls |
|---|---|---|
| `LLVMRunPasses(m,"default<O2>",tm)` — **exactly what kaic2 runs** (reproduced in a standalone C harness with the host TargetMachine) | 58 | 1286 |
| `opt -O2` / `opt -passes=default<O2>` | 83 | 1424 |
| `opt -O2 -inline-threshold=9999` (unlimited inlining) | **82** (no change!) | — |
| `clang -O2` on the **same** bc | **13** | **168** |
| `clang -O2 -inline-threshold=9999` | 3 | — |

`-inline-threshold=9999` has **no effect** under `opt` — the inliner isn't
declining on cost, it's declining on *legality*. `clang` gets to 13 because clang
propagates the TargetMachine's CPU/features onto every attribute-less function
before the CGSCC inliner runs; `opt`/`LLVMRunPasses` does not.

The causal proof — inject the runtime's `target-cpu`/`target-features` onto the
886 attribute-less kaic2 functions in the linked module, then re-run the identical
`opt -O2`:

| | `kaix_*` defs after opt -O2 |
|---|---|
| baseline (no injection) | 83 |
| **after injecting target-features** | **13** — matches `clang -O2` exactly |

And the symmetric confirmation — *strip* the features off the runtime bc instead
(match the callee down to the featureless caller) → also **13**. Either side
matching the other unblocks inlining. It is purely the mismatch.

### The fix locus

`kai_llvm_add_function` (`stage2/runtime.h:13150`) creates every function with a
bare `LLVMAddFunction` and never attaches `target-cpu`/`target-features`:

```c
static void *kai_llvm_add_function(void *m, KaiValue *name, void *fnty) {
    LLVMValueRef fn = LLVMAddFunction((LLVMModuleRef) m, name->as.s.bytes, (LLVMTypeRef) fnty);
    if (name) kai_decref(name);
    return (void *) fn;
}
```

The runtime bc, by contrast, is built by `clang -O2` (`tools/gen-runtime-bc.sh`)
which bakes the host CPU features in by default (`target-cpu="apple-m1"`,
`target-features="+neon,+fp-armv8,…"`). The asymmetry: clang bakes host features
into the runtime; kaic2 bakes nothing into the program.

### Generality

Not rb-tree-specific. A second, unrelated program (`branch_aware_dup`, 814
emitted functions) has **0** functions carrying target-features. Every native
binary loses runtime-op inlining. Platform-independent: the runtime bc always
inherits host features via clang; kaic2 always emits none, so the mismatch holds
on any target.

## Quantitative attribution

Measured, rb-tree 1M, injecting the fix (target-features) and rebuilding:

| | wall (median) | vs C | residual kaix calls |
|---|---|---|---|
| C backend | ~0.30 s | 1.0× | 74 (coarse) |
| native **current** | ~0.83 s | 2.7× | 1168 |
| native **+ target-features fix** | ~0.53 s | **1.7×** | 168 |

**The attribute mismatch accounts for ~55% of the wall gap (2.7× → 1.7×) and
~85% of the residual runtime-op calls (1168 → 168).** Output is byte-identical
(`size: 1000000`, `height: 29`) before and after — the fix is pure inlining, no
semantic change.

The residual 1.7× that remains after the fix is the *genuine* kaikai-vs-C
overhead — RC traffic (the incref/decref pairs Perceus doesn't elide, decref/alloc
≈ 2.11× per #1084) — which is the ORIGINAL #1083 lever, not codegen. The 6×
instruction / 2.7× wall native-vs-C "defect" is dominantly this single
attribute bug; it does not require the drop-specialization / flat-layout / TRMC-
unroll work.

Mapping to the brief's three candidates:
- **#1 unboxing** — NOT a distinct cause. Function *signatures* already unbox
  (`insert_loop(ptr, i64, i64)` on both backends). The per-node re-boxing
  (`kaix_int`) and field-read indirection are downstream of #2: they exist as
  calls only because those calls aren't inlined. Inlining collapses them.
- **#2 per-op call overhead / inlining** — **THE cause.** ~85% of residual calls.
- **#3 opt pass** — the pipeline runs the inliner correctly; it declines on
  legality, not on a missing pass. `default<O2>` = `clang -O2`'s pass *set*, but
  `LLVMRunPasses` lacks clang's feature-propagation step, which is the actual gap.

## Fix plan (in impact order; NOT implemented — integrator authorizes)

### Fix A (recommended) — kaic2 stamps host target-features on every emitted function

At `kai_llvm_add_function` (or once, in a post-emit / pre-opt sweep over the
module), attach the emission TargetMachine's CPU + features to each function:
`LLVMGetHostCPUName()` / `LLVMGetHostCPUFeatures()` → `"target-cpu"` /
`"target-features"` string attributes via `LLVMAddTargetDependentFunctionAttr`
(or `LLVMCreateStringAttribute` + `LLVMAddAttributeAtIndex` at
`LLVMAttributeFunctionIndex`). This mirrors exactly what clang does and what the
runtime bc already carries, so caller ⊇ callee holds and the inliner fires.

- **Impact:** the measured 2.7× → 1.7× wall / 1168 → 168 calls. Global (every
  native binary), so the whole default backend gets ~35% faster on RC-heavy code.
- **Risk:** low. It changes *inlining*, not semantics; output is byte-identical
  (verified). It does NOT touch RC/Perceus logic — the incref/decref calls it
  inlines are the same calls, just inlined. The UAF class (#1054/#1069/#1074)
  lives in the reuse-token *emission* (which slot to donate, which child is
  unique), not in whether `kaix_internal_dup` is a call or inlined; this fix does
  not move that boundary. Standard care: use the SAME host CPU/features the
  runtime bc was generated with (both are clang/libLLVM host defaults on the
  build machine, so they already agree by construction — see
  `tools/gen-runtime-bc.sh`), and set them BEFORE `kai_llvm_link_runtime_bc` +
  `kai_llvm_run_passes` so the merged module is uniform.
- **Gate:** selfhost byte-id (the emitted *program* changes — attributes are new
  IR — so the stage2→stage2 object will differ; re-baseline is expected, verify
  the rebuilt kaic2 is fixed-point). Backend parity (serial, per memory
  `native_preexisting_mac_parity_fails`), ASAN-on-selfhost (#1042), and
  `KAI_TRACE_RC` balanced. Add a perf fixture asserting the rb-tree native binary
  drops below a call-count / wall threshold vs C so this can't silently regress.

### Fix B (alternative, lower payoff) — strip features from the runtime bc

Build `runtime_llvm.bc` with `-mcpu=generic` / no host features (or post-strip in
`gen-runtime-bc.sh`) so the callee is feature-less and matches the attribute-less
caller. Also reaches full inlining (verified → 13 defs). Simpler (one build-script
line), but leaves BOTH the runtime and the program without host-CPU features, so
the scalar codegen of the program's own arithmetic loses NEON/host tuning. Fix A
is strictly better on generated-program quality; B is a fallback if stamping
attributes in the emitter proves fiddly.

### Not the fix

- Raising the opt level or inline threshold — proven inert (`-inline-threshold=9999`
  unchanged; O3 = O2 here). The block is legality, not cost.
- Adding an LTO step — unnecessary; P2 already merges the runtime in-process. The
  merge works; only the attribute mismatch blocks the inliner.
- The drop-specialization / flat-layout / TRMC-unroll lane — that is the *residual*
  1.7× (RC traffic), the original #1083 lever, orthogonal to and downstream of
  this codegen fix.

## Method notes (rigor)

- Wall measured interleaved + warm, explicit `--backend=`, both wall (internal
  `elapsed:`) and instruction proxy (static call counts via `otool -tV`), re-run
  on surprises. macOS has no `perf`; the residual-`bl`-call count is the honest
  instruction proxy and tracks wall monotonically across all variants.
- P2 activation verified directly: `stage0/runtime_llvm.bc` was absent in a fresh
  checkout; `make KAI_LLVM=1 kaic2` (via `tools/gen-runtime-bc.sh`) regenerates
  it (clang-18 present → `active`). The `.o` kaic2 produced is self-contained
  (0 undefined `kaix_`), confirming P2 ran for the measured binary.
- The `LLVMRunPasses` behavior was reproduced in a standalone 30-line C harness
  linking libLLVM-18, so the "opt inlines less than clang" claim is not an
  `opt`-CLI artifact — it is the same C API kaic2 calls.
