# Lane experience — issue #1021: native self-host gate (ratchet baseline)

Tooling/gate lane. Scope deliberately bounded by the owner: **install the
meter, NOT close the gap.**

## Scope — as planned vs. as shipped

**Planned:** add a CI gate that compiles the compiler with the native
backend and ratchets the subset-gap count against a baseline; do NOT
attempt to close the gap (drive the count to 0). Land it as an
expected-fail that documents the current failure and catches regressions.

**Shipped:** exactly that, in four additive pieces, zero compiler-source
changes:

- `tools/native-selfhost-baseline.txt` — the ratchet baseline (a single
  integer + documented semantics), mirroring `native-parity-baseline.txt`
  and `coverage-baseline.txt`.
- `tools/test-native-selfhost-gate.sh` — measures the gap and gates
  (regression → FAIL, equal → PASS expected-fail, lower → PASS ratchet-down
  prompt, zero → ACHIEVED).
- `stage2/Makefile` target `test-native-selfhost-gate` (+ `.PHONY` entry).
- A step in `tier1-native.yml`'s `build-native` job.

## The real baseline: 25, identical to 0.96

The brief required measuring **main** (not trusting #1021's 0.96 number).
Measured on `main` (post-bump v0.97.0, 2497450d) with a fresh
`make KAI_LLVM=1 kaic2`: **25 `unbound register` subset-gap aborts**,
deterministic across two runs, breakdown `n×8 nm×7 tyname×6 name×2 eff×2`
— **byte-identical to the 0.96 measurement in the issue.** The gap has not
moved between 0.96 and main. So the issue's 25 was safe to adopt, but we
only know that because we re-measured.

## Structural surprises the brief did not anticipate

1. **The invocation is not `-o`-shaped.** The first attempt,
   `kaic2 --emit=native -o /tmp/x bundle.kai`, failed with `read_file:
   cannot open file`: the driver has **no `-o` flag** (the `else` arm of
   `parse_cli_loop` treats any unknown arg as the input path, last one
   wins), and the native spine derives the object path from the source
   (`main.kai → main.o`), ignoring output flags entirely. The gate passes
   no `-o` and cleans `main.o` afterward.
2. **The bundle is the WRONG input.** Compiling `stage2/build/bundle.kai`
   (the concatenated BUNDLE_SRCS that kaic1 consumes) with kaic2 does NOT
   reproduce the gap — it dies earlier on `second module-level #[doc(...)]`
   (the bundle concatenates ~55 module docs into one translation unit) and
   `cannot open module 'compiler.chars'` (the surviving `import compiler.X`
   lines resolve relative to `stage2/build/`). The correct self-compile
   input is the issue's own form: **`stage2/main.kai` run from `stage2/`**,
   so `import compiler.driver` resolves modularly against `compiler/*.kai`.
   The bundle is kaic1's bootstrap path; it is irrelevant to a kaic2
   self-compile.
3. **A zero count is ambiguous without an exit check.** If the self-compile
   fails for an UNRELATED reason (typer/parser/link, or a new subset-gap
   class that is not an `unbound register`), the count is 0 — which the
   naive ratchet would mis-report as "ACHIEVED". The script guards this:
   `count==0` only means ACHIEVED when the compile **exited 0 and produced
   `main.o`**; a zero count with a failed compile FAILs loudly with the
   first 40 stderr lines. This keeps the gate honest as the gap closes.

## CI wiring — informative, visible, NOT a required check

The gate lives in `tier1-native.yml`'s `build-native` job (which already
builds the `KAI_LLVM=1` kaic2 + P2 bitcode, so the gate script's relink is
seconds, not a full rebuild).

**Key fact confirmed by the integrator:** the only **required** check in
`main`'s branch protection is **`tier1`** — `tier1-native` is NOT required.
So this gate is **informative and visible on every PR** (it runs always-on,
turns red on a real regression) but it **does NOT block the merge of an
unrelated PR**. That is precisely the "additive, native-only, do not
disturb" posture the issue asks for: the regression guard is visible
without obstructing velocity.

- Normal state (count == baseline == 25): step is **GREEN** (exit 0).
  Verified locally end-to-end.
- Regression (count > 25): step is **RED** (exit 1), visible on the PR,
  but not merge-blocking (tier1-native is not required). Verified by
  temporarily lowering the baseline to 24.
- Ratchet-down (count < baseline): GREEN with a prompt to lower the
  baseline. Verified by temporarily raising the baseline to 26.

**For the day the gap reaches 0:** when the count hits 0 and the gate is
upgraded to require the native object to LINK + RUN (the issue's full
milestone), and you want it to actually gate the merge, you must **add
`tier1-native` to `main`'s branch protection** as a required check. Until
then it is a ratchet, not a gate-of-record. #1021 stays OPEN until the
count is 0; this lane does NOT close it.

## Fixtures / coverage

No `.kai` fixture: this lane's test artifact is the gate script + baseline
themselves, and the thing under test is the whole compiler, not a sample
program. The three ratchet paths (regression / steady / ratchet-down) were
exercised manually by perturbing the baseline; the steady path is what CI
runs each PR.

## Cost vs. estimate

Dominant cost was the one-time `make KAI_LLVM=1 kaic2` (native build);
everything after — measuring, scripting, wiring — was fast. The two
false-start invocations (`-o`, bundle) cost one round-trip each but pinned
down the correct self-compile form, which is the lane's load-bearing fact.

## Follow-ups for next lanes

- **Close the gap** (#1021's real work): lower the count from 25 to 0 in
  `stage2/compiler/emit_native_fn.kai` + `kir_lower_*.kai`. The breakdown
  (`n` let/var-bound reads dominate at 8, then `nm`, `tyname`) points at
  the arm-body register-load path as the first target. Each closed slice
  ratchets `tools/native-selfhost-baseline.txt` down.
- When count → 0: upgrade the gate to LINK + RUN (and ideally a
  native-built kaic2 compiling a sample program correctly), add
  `tier1-native` to branch protection, close #1021.
