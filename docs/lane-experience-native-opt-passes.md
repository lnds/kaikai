# Lane experience — native opt passes (L4, issue #498)

## Scope as planned vs as shipped

**Planned (issue #498, L4 of the LLVM-direct refactor):** with libLLVM
linked (L3 done), run the LLVM optimisation passes in-process over the
native backend's module BEFORE emitting the object, instead of leaning on
the `clang -O2` the out-of-process C / LLVM-text paths get. Two profiles:
release (`-O2`, parity with the C path) and debug (`-O0`). User decision
(firm): the default with no flag is `-O2`; `--debug` drops to `-O0`.

**Shipped:** exactly that. The native emit path (`kai_llvm_emit_object`)
now runs the LLVM New-PM pipeline between verify and object emission,
reusing the TargetMachine the emit step already builds. The level is read
from `KAI_NATIVE_OPT` (default `2`), which `bin/kai --debug` / `--release`
set. `0|1|2|3|s|z` map to `default<O0…Oz>`; unknown values fall back to
`O2`. No new kaikai source file: the pipeline is ~25 LOC of C inside the
existing `#ifdef KAI_LLVM` block, plus one Makefile component and two
shell flags.

## C-API chosen

The modern New-PM C-API, not the legacy PassManager:

```c
LLVMErrorRef LLVMRunPasses(LLVMModuleRef M, const char *Passes,
                           LLVMTargetMachineRef TM,
                           LLVMPassBuilderOptionsRef Options);
```

Pipeline string is the same format as `opt -passes=`: `"default<O2>"`
invokes exactly the per-module pipeline `clang -O2` builds, so native
matches the C path's pass set. The header (`llvm-c/Transforms/PassBuilder.h`)
+ signature + string semantics have been stable across the whole LLVM
18.x series (the binding stabilised in LLVM 13 when the legacy PM binding
was removed), so 18.1.8 (local static) ↔ 18.1.3 (Ubuntu llvm-dev) is
safe — verified by an asu consult and by the Docker Linux build.

Three CÓMO points the asu consult settled (all confirmed by build):

1. **Reuse the emit TargetMachine, don't build a second one.** The
   original brief sketched a separate `llvm_run_passes(m, opt_level)`
   prim called from the kaikai side, but that would reconstruct
   target/triple/cpu/features/tm a second time. Worse than economics: the
   `default<O2>` pipeline consults the TM's TargetTransformInfo cost model
   (vectorisation / unrolling / inlining decisions), so the passes MUST
   run with the same TM that later emits, or the cost model can diverge
   from the emission target. One TM, used for both.
2. **Order: verify → SetDataLayout → RunPasses → EmitToFile.** A pass set
   assumes verified IR; running `default<O2>` over malformed IR is UB, not
   a clean error. Verify first catches a codegen bug with a clear message
   before a pass turns it into an opaque LLVM crash. DataLayout must be
   set before optimising (several passes read sizes/alignments from it).
3. **Error lifecycle.** `LLVMGetErrorMessage` *consumes* the
   `LLVMErrorRef`; you then free the `char*` with `LLVMDisposeErrorMessage`
   (not `free`). Do NOT call `LLVMConsumeError` after `GetErrorMessage` —
   the two paths are mutually exclusive. `LLVMCreatePassBuilderOptions` +
   `LLVMDisposePassBuilderOptions` with default options is enough for
   `clang -O2` parity (clang sets none of the builder options by default).

## Two profiles + default -O2

- **release** (`KAI_NATIVE_OPT` unset / `2`, `bin/kai --release`):
  `default<O2>`. The default with no flag.
- **debug** (`KAI_NATIVE_OPT=0`, `bin/kai --debug`): `default<O0>`.

Threading is by env var, not a CLI flag through `CliOptions`. Decision
(mine, internal-impl): `CliOptions` re-lists every field in ~8
`cli_with_*` constructors; adding an `opt_level` field would touch all of
them for one integer. The opt level is read where it is used (next to the
TM in the emit prim) via `getenv`, the same pattern the file already uses
for `KAI_NATIVE_DUMP_IR`. This keeps `driver.kai`, `native_prims.kai`, and
`emit_native.kai` untouched — so selfhost byte-identity of the C path is
preserved trivially (the C path never enters the `#ifdef KAI_LLVM` block),
and the flag is still verifiable end-to-end (`bin/kai --debug` exports
`KAI_NATIVE_OPT=0`).

## Functional parity (the load-bearing gate)

The risk the brief flagged: running `-O2` must not break parity; a fixture
that diverges after `-O2` is an emit bug the opt exposed, not an opt
success to silence. Result: **zero regressions, three gaps closed.**

- tier1-native (`tools/test-native-parity.sh`): 15/15, including the new
  `examples/native/opt_pipeline_o2.kai` regression fixture, all under the
  -O2 default vs C-direct.
- ratchet (`tools/test-backend-parity.sh` `TARGET_BACKEND=native`,
  `NATIVE_PARITY_RATCHET=1`): pass=458 fail=11, ratchet OK (**improved**) —
  zero NEW gaps, and three baseline gaps now pass consistently under -O2
  (`scalar_fn_sig_deep_recursion`, `list_helpers`, `list_zip3_scan`).
  NOT removed from the baseline: `list_helpers` / `list_zip3_scan` are
  Linux-only SIGSEGV traps that mac does not reproduce with OR without
  opt, so tightening the baseline off a mac run would break CI Linux;
  `scalar_fn_sig_deep_recursion` passing only under -O2 would tie the
  baseline to "always -O2". Closing gaps is burn-down work, not this lane.
- Object IS optimised + behaviour identical (issue acceptance): same
  fixture at -O2 vs -O0 — objects differ byte-for-byte; O2 is 205,920 B vs
  O0 223,328 B, `__TEXT` 85,161 vs 95,975 (−11% code), 856 vs 1,161
  symbols (inlining + DCE of internal fns). Both print the same result,
  exit 0. Default == O2 byte-identical; unknown level == O2; O3 ≠ O0
  (the level string really reaches the pipeline).
- selfhost byte-id: `kaic2b.c == kaic2c.c` — the C path is unchanged.
- ASAN under -O2: an RC + nested-match + list fixture is ASAN-clean at
  both -O2 and -O0 (no UAF / double-free the opt could expose).

## Structural surprises the brief did not anticipate

1. **The C forwarders live in `stage2/runtime.h`, not
   `stage0/runtime_llvm.c`.** The brief pointed at `runtime_llvm.c` for
   the forwarder; the actual `kai_llvm_*` family (under `#ifdef KAI_LLVM`)
   is in `stage2/runtime.h`. `runtime_llvm.c` is the link-time shim the
   driver compiles, not where the C-API forwarders are defined.
2. **No new prim, no `native_prims.kai` / `emit_native.kai` change.**
   Because the right design (asu) is to run passes inside `emit_object`
   reusing its TM, and the level is env-read, the kaikai-side registry
   stays in lockstep automatically — there was nothing to add there. The
   brief's "AÑADE llvm_run_passes to the prim registry" was superseded by
   the better mechanism.
3. **The link needed a new LLVM component: `passes`.** `LLVMRunPasses` /
   `LLVMCreatePassBuilderOptions` live in `LLVMPasses` (the New-PM
   PassBuilder), which the Makefile's `core analysis target nativecodegen
   bitwriter` set did not pull in. Added `passes` to the front of the
   `llvm-config --libs` list; llvm-config expands its transitive closure
   (ipo / vectorize / scalaropts / instcombine …). `libLLVMPasses.a` and
   its deps are present in the vendored static build, and `passes` is a
   standard component present in Ubuntu's `llvm-dev` too.
4. **The static LLVM lives in the sibling worktree.**
   `stage0/third_party/llvm/build/` was built in the main checkout, not
   this worktree; the brief's absolute path pointed there. Used that
   `llvm-config` (build toolchain, not project sources).

## Fixtures added + coverage

- `examples/native/opt_pipeline_o2.kai` — the regression fixture, wired
  into tier1-native (`tools/test-native-parity.sh`, which the harness
  auto-globs from `examples/native/`). Exercises the optimised shape
  (constant-fold + dead internal helper DCE'd + inlined leaves) and
  asserts native -O2 == C-direct on stdout + exit. No `.out.expected`
  golden needed: the harness diffs native against the C oracle directly.

Coverage gap left: there is no automated assertion that the *object*
shrinks under -O2 (the size / symbol-count delta is checked by hand in
this lane, not in CI). A CI assertion on `__TEXT` size would over-fit to
the host LLVM version; left out deliberately. The behavioural parity gate
is the real guard.

## Real cost vs estimate

Issue estimated ~3-4 days. Real: one focused session. The mechanism was a
single asu consult away (the C-API was the only open question), the
emit-shape was already correct (zero parity regressions — the opt found no
latent bugs), and the threading was deliberately kept to an env var to
avoid the `CliOptions` churn. The only non-obvious step was the `passes`
link component.

## Follow-ups unblocked (NOT in this lane)

- **#499 (L5) — bitcode cache.** Now that the module is optimised
  in-process, L5 can cache the post-opt bitcode keyed on a content hash,
  skipping both lowering and the pass pipeline on a clean rebuild. Depends
  on this lane (the thing to cache is the optimised module).
- **#500 (L6) — build modes.** The `--release` / `--debug` flags this
  lane added to `bin/kai` are the surface L6 builds on (release/debug as
  full build profiles: opt level + symbols + assertions + LTO). This lane
  ships the opt-level half; L6 generalises it.
