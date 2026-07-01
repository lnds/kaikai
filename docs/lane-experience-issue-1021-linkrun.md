# Lane experience — issue #1021 native self-host LINK + RUN

## Scope

- **As planned:** close the LINK + RUN milestone of #1021 — the native-compiled compiler must LINK into an executable and RUN (compile a sample program correctly), ideally recursive self-host (native-built kaic2 compiles itself). Upgrade the gate from `count==0` to require link + run.
- **As shipped:** COMPILE + LINK + RUN achieved and gated. Recursive self-host (native-built kaic2 compiling a program) **NOT** achieved — blocked by a native -O2 codegen bug (issue #1025). #1021 is `refs`, not `closes`: the milestone's self-compile half is open.

## Level reached (the honest state)

| Step | Status |
|---|---|
| COMPILE — native lowers `stage2/main.kai`, 0 `unbound register`, produces `main.o` | ✅ (was already green on the base branch) |
| LINK — `main.o` links into an executable `kaic2-native` | ✅ (this lane) |
| RUN — `kaic2-native` starts, runs real kaikai (`--version` prints via Stdout, exits clean) | ✅ (this lane) |
| SELF-COMPILE — native-built kaic2 compiles a sample program | ❌ SIGSEGV at -O2 in `rqc_decl` (issue #1025) |

## The two gaps this lane found and closed

Measuring the link first (the brief's step 1) surfaced two independent gaps, both closed here:

### Gap 1 — symbol-prefix / linkability (LINK)

The native object references the in-process libLLVM C-API prims as `kaix_prelude_llvm_*` / `kaix_prelude_native_ctx_*` / `kaix_prelude_native_di_*` (88 symbols). Their bodies live in `runtime.h` as `static kai_<name>` — a `static` cannot be named across translation units, so a separately-compiled object cannot bind them. The C-backend compiler links because its whole program is ONE TU (`build/stage2.c` `#include`s `runtime.h` with `-DKAI_LLVM`). The native object is a separate TU.

**Fix:** a dedicated shim TU `stage0/runtime_llvm_native_shim.c` — non-static `kaix_prelude_*` thunks over the `static kai_*`, compiled only under `-DKAI_LLVM` (empty otherwise, so a plain user-program native link is unaffected). Mirrors the existing `kaix_prelude_print` etc. forwarders in `runtime_llvm.c`. A dedicated file (not folded into `runtime_llvm.c`) because the native object is bitcode-self-contained — it already carries `main` + every other `kaix_*` from `runtime_llvm.bc` (generated WITHOUT `-DKAI_LLVM`), lacking ONLY these 88 libLLVM prims; compiling all of `runtime_llvm.c` would duplicate `main` + the whole runtime.

`tools/native-selfhost-link.sh` is the reusable link chain: shim TU (`-DKAI_LLVM` + libLLVM cflags) + the object + libLLVM (same component set the stage2 Makefile links kaic2 against, `-lc++`/`-lstdc++` by host).

### Gap 2 — the compiler's `main` inferred an empty effect row (RUN)

After linking, `kaic2-native` started but died with `effect not handled in fiber: Stdout` — the native install-defaults never mounted the Stdout default handler. Root cause in the TYPER, not the native backend:

`kai_main_install_defaults` mounts the default handlers for the effects in `main`'s inferred row (`main_row_labels` → `default_setups_for`). The compiler's `main` is `args() |> parse_cli |> run` (`run : ... / Console + File`), with no explicit return row. `collect_call_labels` (which the typer uses to write `main`'s inferred row back into the DFn) walked the `EPipe` operands but never read the row of the function the pipe APPLIES. For a bare-function rhs (`|> run`), the `run` node is an `EVar` carrying `TyFnT(_, _, {Console+File})`; the plain `EVar` arm sees no call to read, so the applied row was lost. A direct-call `main` (`fn main() { loop(1,20) }`) worked because the `ECall` arm reads `callee.ty`.

**Fix:** `collect_pipe_labels` reads the applied function's row off the `EPipe` rhs node when it is `TyFnT`. Inert for `x |> f(y)` (the rhs is an `ECall` carrying its return type). One helper, 16 lines. Byte-id preserved: the fix only changes what the default-handler wrapper installs, not the emitted C (tier0 `kaic2b.c == kaic2c.c` stays green) — it was already a latent gap that never bit the C compiler because... (see below).

**Why the C-built compiler never hit this:** it does the same install-defaults, so a C-built kaic2 SHOULD also have missed Stdout for its own `main`. It doesn't crash because the C runtime's default-handler resolution differs enough that the missing mount is benign there, whereas the native install path hard-requires the `_ev_Stdout` blob the empty row omitted. The fix makes both correct; only native observably needed it.

## The blocker (issue #1025)

Recursive self-host revealed a native **-O2 codegen** bug: the native-built kaic2, compiling any program, crashes in `modules.rqc_decl`'s `match d` — the `Decl` scrutinee reaches `kaix_variant_tag_of` as a tagged Int `0xb` instead of a variant pointer. Disassembly shows `d` is already `0xb` on entry to `rqc_decl`, so the caller (`rqc_decls`, destructuring `[d, ...rest]`) passed a tagged Int. `-O0` masks it (the known trap). Hypothesis: evidence-frame arg-shift (`rqc_decl` carries a `Console + File` row → leading `ptr __evf`) colliding with the destructured-list-element arg at -O2. A simple repro (match-over-variant-from-list, ± effect row) did NOT reproduce, so the trigger is narrower — needs reduction from the real compiler. Per the integrator's call, isolating it is its own lane; not dug further here.

## Gate change

`tools/test-native-selfhost-gate.sh`: on a 0 `unbound-register` count it now LINKS (`native-selfhost-link.sh`) and RUNS (`--version`, exit 0) the native compiler, failing loudly if either breaks. It does NOT yet require self-compile (that would fail on #1025). Baseline doc updated: "produces object" → "links + runs; self-compile blocked by #1025". Native-only; the C bootstrap and selfhost C-byte-id are untouched.

## Fixtures

The gate itself is the fixture: `make -C stage2 KAI_LLVM=1` + the gate exercises COMPILE→LINK→RUN end to end. No new `examples/` fixture — the exercised artefact is the compiler, not a user program; a user-program fixture for #1025's crash shape belongs to that lane once reduced.

## Cost vs estimate

Diagnosis dominated. The two gaps were not in the brief's mental model (the brief expected "missing symbols / runtime wiring" — Gap 1 — but not the typer's pipe-row omission — Gap 2, nor the -O2 codegen blocker). Each needed a measure→localise→fix cycle: Gap 1 via `nm` symbol cross-check against `rprelude_table`; Gap 2 via `--effects-json` + `nm` `_ev_Stdout` diff between a working and a failing object; the blocker via lldb `$lr` symbolisation + prologue disassembly. The link chain and gate wiring were mechanical once the gaps were understood.

## Follow-ups for the next lane

- **#1025** — the -O2 codegen bug. Reduce from the compiler to a minimal user repro, confirm the evidence-frame-arg-shift hypothesis (or refute it), fix in the native emitter (RC/reuse/arg-lowering), gate on -O2 + Linux + UBSan.
- Once #1025 is fixed, escalate the gate: require the native-built kaic2 to compile a fixture set with output identical to the C-built oracle, and ideally recursive self-host (native-built kaic2 compiling `main.kai` → a second native compiler). Then #1021 closes.
- Consider whether the gate should become required (branch protection) once self-compile is green — the integrator decides; not touched here.
