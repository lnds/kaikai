# Lane experience — issue #1083 (native codegen fix: stamp host target-features)

## Scope as planned vs as shipped

Planned (from `docs/investigation-native-vs-c-perf-1083.md`, the diagnostic
spike merged in PR #1086): implement **Fix A** — make the native backend stamp
the host `target-cpu`/`target-features` on every emitted function so
`areInlineCompatible` lets the opt pipeline fold the `kaix_*` runtime ops into
the hot path. The diagnosis proved causally that the featureless emitted
functions block *every* runtime-op inline against the featured runtime bitcode,
which is the dominant ~55% of the native-vs-C wall gap.

Shipped: exactly Fix A, plus a call-count regression gate. One structural
surprise (below) changed *how* the features are sourced, but not the fix's
shape or locus.

## The one structural surprise the diagnostic did not anticipate

The diagnostic's Fix A said to stamp features via `LLVMGetHostCPUFeatures()`
(mirroring the emit TargetMachine at `runtime.h:14162-14163`). **On this
libLLVM-18 build `LLVMGetHostCPUFeatures()` returns the empty string** — the
TargetMachine tolerates that (it infers scalar codegen from the CPU name), but
an empty feature string is useless for the per-function stamp: `+neon,+fp-armv8,
…` never lands, so `areInlineCompatible` stays blocked and the call count does
not move.

The first implementation followed the doc literally (stamp `cpu`/`features`
from `LLVMGetHostCPUName`/`LLVMGetHostCPUFeatures` at the pre-link site). It
built, self-hosted, and shipped a kaic2 whose `strings` showed the new
`target-features` literal — yet rb-tree stayed at **1168** residual `kaix_`
calls. The dump chain localized it: `feat40=` was empty in the emit path while
the runtime bc carried the full `+aes,+complxnum,…` string that clang-18 baked
in. The asymmetry the doc described (clang bakes features, kaic2 bakes none)
was real; the doc's *remedy* assumed the same API clang uses is reachable from
libLLVM-C, and it is not on this build.

**Resolution:** source the features from the module itself, not from a host
query. After the runtime bc is linked, pick the first runtime function that
carries `target-features` as a *donor* and copy its `target-cpu`/`target-features`
string attributes onto every program function that lacks them. This guarantees
an *exact* match by construction (same string, same order — not two independent
host queries that could diverge), and is robust to whatever
`LLVMGetHostCPUFeatures()` reports. It also moved the stamp from *before* the
link to *after* it (a donor must be present), which is still before the opt
pipeline, so the merged module is feature-uniform when the inliner runs. No-op
on the legacy cc-links path (no bc → no donor).

This is strictly better than the doc's plan: it can never plant a *mismatched*
feature set, which a second `LLVMGetHostCPUFeatures()` call theoretically could
if the runtime bc were ever built on a different host.

## Measured result (rb-tree 1M, Darwin arm64, native backend)

Method: interleaved + warm, explicit `--backend`, median of internal
`elapsed:`, static `bl <kaix_*>` call count via `otool -tV` as the honest
instruction proxy (macOS has no `perf`).

| | residual `kaix_` calls | wall (median) | vs C |
|---|---|---|---|
| C oracle | 74 (coarse) | 0.217 s | 1.00× |
| native **before** | 1168 | 0.833 s | 3.84× |
| native **+ fix** | **168** | **0.507 s** | 2.34× |

before → fixed: **1.64× speedup** (the diagnostic predicted 2.7→1.7 = 1.59×);
call count **1168 → 168**, matching `clang -O2` exactly (the diagnostic's target
after feature injection). The vs-C *absolute* ratios run higher than the doc's
2.7×/1.7× because this run's C floor is faster (0.217 s vs the doc's 0.30 s —
machine/round noise); the *relative* improvement, which is what the fix proves,
matches.

## Byte-identity preserved — no re-baseline needed

The brief anticipated a selfhost byte-id re-baseline ("the emitted program
changes — attributes are new IR"). That is true of the native **object**, but
the repo's `selfhost` gate compares emitted **C** (`kaic2b.c` vs `kaic2c.c`),
and the fix lives entirely in the `kai_llvm_*` native path — `emit_c.kai` never
touches `kai_llvm_add_function`. So:

- `make -C stage2 selfhost` (C byte-id): **OK, unchanged** — no re-baseline.
- `test-native-selfhost-gate` (compiles the compiler with the *native* backend,
  then requires its emitted C to be **byte-identical to the C-direct oracle**):
  **OK** — COMPILE + LINK + RUN + SELF-COMPILE all green. This is the decisive
  check: it proves the fix changes *inlining of the object*, not *what the
  compiler emits*. Had the native-built compiler diverged, that would signal a
  behavioural change and would have stopped the lane (per the brief's guard);
  it did not.

The re-baseline the brief warned about would only apply to a stored native
*object* fingerprint. The repo does not gate on one, so there was nothing to
re-baseline — the fix is invisible to every byte-id gate the repo runs.

## Why low risk

- Pure inlining, no semantics. `KAI_TRACE_RC` on rb-tree is **identical** across
  before / fixed / C-oracle: `alloc_total=6301552 free_total=6301535 leaked=17
  incref=13317720 decref=13317728`. The `leaked=17` is pre-existing (the C
  oracle shows it too); the fix does not move a single RC operation — it inlines
  the *same* incref/decref calls.
- It does not touch RC/Perceus logic. The UAF class (#1054/#1069/#1074) lives in
  the reuse-token *emission* (which slot to donate), not in whether
  `kaix_internal_dup` is a call or inlined. The fix does not move that boundary.
- Output byte-identical (`size: 1000000`, `height: 29`) before and after.

## Fixtures added

- `tools/native-perf/inline-gate.sh` — builds rb-tree native, disassembles,
  counts residual `bl kaix_*` calls, fails if > 400 (168 with the fix, 1168
  without — the threshold catches a broken stamp with generous margin for
  LLVM-version inlining drift). Portable: `otool` on Mach-O, `objdump` on ELF
  (CI is ubuntu). SKIPs where libLLVM is absent.
- `stage2/Makefile` target `test-native-1083-inline-gate` + wired into
  `.github/workflows/tier1-native.yml` shard-2 next to the other #1083 gate.

Verified the gate is not vacuous: the pre-fix binary (1168 calls) exceeds the
threshold, so a regression that drops the stamp turns the gate red.

## Follow-ups left for next lanes

**closes #1083 for the codegen half only.** The residual 2.34× vs C (≈1.7× in
the doc's cleaner run) is the *genuine* kaikai-vs-C overhead — RC traffic, the
incref/decref pairs Perceus doesn't elide (decref/alloc ≈ 2.11× per #1084).
That is the **original** #1083 lever (drop-specialization / flat-layout), a
separate class of work orthogonal to and downstream of this codegen fix. The
integrator decides whether the RC residual warrants a fresh issue.

## Real cost vs estimate

The fix is ~34 lines in one file. The cost was entirely in the surprise: the
first (doc-literal) implementation compiled, self-hosted, and *looked* shipped
while the call count never moved. The lesson — a stamp that plants an empty
feature string is byte-silent and gate-silent; only the disassembly call count
exposed it, which is exactly why the perf gate is call-count-based and lives in
CI.
