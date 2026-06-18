# Lane experience: native raw Int `/` and `%` (P4)

**Date:** 2026-06-17 · **Context:** P4 of the native-codegen perf plan
(`docs/native-codegen-perf-plan.md`), the follow-up to P3 (PR #857, native raw
scalar params). **Result: SHIPPED** — div/rem raw closes the last scalar perf
residual; an earlier attempt was discarded on a misattributed crash that this
lane diagnosed to a pre-existing, unrelated RC use-after-free.

---

## What this lane set out to do

P3 (merged) made the native backend emit raw scalar (`i64`/`f64`/`i1`)
param/return signatures. With it, the `+ - *` arithmetic of a scalar loop
reached C parity. But `arith_runtime` (`acc + i*i - i/3`) stayed ~23× slower
than C because the `i / 3` was still BOXED: `kaix_int(i)` + `kaix_int(3)` +
`kaix_div(boxed, boxed)` + a field read per iteration. That `i/3` was the only
residual.

P1 had left Int `/` and `%` boxed on purpose: the native `sdiv`/`srem` are
undefined behaviour in two cases kaikai otherwise defines — `x / 0` (kaikai
panics) and `INT64_MIN / -1` (the quotient `2^63` overflows `i64`).

## The key finding (correct, from the earlier attempt)

The C-direct backend (the parity ORACLE) ALREADY emits `/` raw and WITHOUT a
guard: a bare `(kair_a / kair_b)`, with the `/0` and `INT_MIN/-1` UB explicitly
documented as "a separate concern" (`emit_c.kai` `emit_kind_raw`). The
`op_is_raw_arith` of the shared unbox pass ALWAYS included `/` and `%` — the
only thing keeping them boxed was the native KIR lowering.

So the correct fix is NOT a guard (that would diverge from the byte-exact
oracle) but `sdiv`/`srem` directly — byte-for-byte parity with C, including the
same UB the C backend already accepts.

## The patch (4 sites, small)

1. `kir_lower_raw.kai` `kir_op_is_raw_iint_arith`: admit `/` and `%` (was `+ -
   *` only). `kir_ibinop_op` extended `3=sdiv 4=srem` for consistency (dead
   code — no consumer — but kept in sync with the runtime switch).
2. `runtime.h` `kai_llvm_build_ibinop`: opcodes `3=sdiv` (`LLVMBuildSDiv`),
   `4=srem` (`LLVMBuildSRem`), alongside `0=add 1=sub 2=mul`. The stub copy
   (native-unavailable) ignores `op`, so it needs no change.
3. `emit_native_ops.kai` `nprim_raw_ibinop_op`: map `i/`→3, `i%`→4. The prim
   name is `cat2("i", op)` (`kir_raw_iarith_prim`), so `/`→`"i/"`, `%`→`"i%"`;
   the existing ibinop dispatch handles them with no new code.
4. Fixture `examples/native/raw_scalar_divmod.kai` — `gcd` (`%` with a runtime
   divisor), `divsum` (`/` with a falling runtime divisor), and `signed`
   (a negative dividend, so the signed `srem`/`sdiv` truncate toward zero
   exactly as C: `-7 / 3 == -2`, `-7 % 3 == -1`). No divisor is a compile-time
   constant, so O2 keeps a real `sdiv`/`srem` rather than a magic-multiply.

   No `.out.expected` is added: `examples/native/*.kai` carry NONE (0 of 18 on
   `main`). The `tools/test-native-parity.sh` and `tools/test-backend-parity.sh`
   harnesses build the SAME fixture with the C-direct oracle and diff stdout +
   exit dynamically — there is no golden file. (The earlier attempt's note to
   add one was wrong; following it would have left an orphan file no harness
   reads.)

## Perf result (measured)

`arith_runtime` (200M iterations): **native 0.07–0.08s vs C 0.07s — parity**
(was ~23×). The `i/3` no longer hits `kaix_div`; the only `kaix_div`/`kaix_mod`
symbols left in the binary are the always-linked `Div_Int_div`/`Rem_Int_rem`
proto-impls, NOT the hot loop (proven empirically: a boxed `kaix_div` per
iteration would be ~1.6s, not 0.08s). The `raw_scalar_divmod` fixture's `gcd` /
`divsum` / `signed` emit real `sdiv`/`srem` (variable divisor) and match C
exactly (`21`, `7485017`, `-201`). selfhost byte-id holds.

## The crash the earlier attempt blamed on this patch — ROOT CAUSE

The earlier attempt saw 53 deterministic stdlib segfaults (exit 139) appear
when the patch was applied and a SERIAL ratchet was run, and discarded the
patch, hypothesising that the boxed→raw reclassification of the
`Div_Int_div`/`Rem_Int_rem` proto-impls broke an indirect consumer.

**That attribution was wrong.** This lane proved the segfaults are PRE-EXISTING
on `main` and have nothing to do with `/` or `%`:

### Minimal repro (8 lines, no `/`, no `%`)

```
fn mycontains(xs: [a], x: a) : Bool = match xs {
  [] -> false
  [h, ...t] -> if h == x { true } else { mycontains(t, x) }
}
fn main() : Unit / Stdout = {
  let xs = [1, 2, 3]
  Stdout.print("c1: #{mycontains(xs, 1)}")
  Stdout.print("c2: #{mycontains(xs, 2)}")
  Stdout.print("c3: #{mycontains(xs, 3)}")
}
```

This SIGSEGVs under `--backend=native` on **clean `main` (no patch)**,
deterministically, on TWO platforms:
- macOS arm64 + Homebrew llvm@18.1.8
- x86_64 Linux + apt llvm 18.1.3 (the EXACT CI environment, reproduced under
  `docker run --platform linux/amd64 ubuntu:24.04`)

`set_basic` / `map_basic` / `list_helpers` likewise exit 139 native on x86_64
Linux on clean `main`. The crash is independent of this patch: it reproduces
identically with and without it.

### The crash itself

Backtrace (macOS crash report + AddressSanitizer): `EXC_BAD_ACCESS` /
`SEGV on address 0x8` (zero-page READ) in `kai_op_eq` ← `kaix_eq` ←
`mycontains`. `kai_op_eq` receives a corrupt pointer (`0x4`/`0x8` — neither an
immediate-tagged value, bit-0 clear, nor a valid heap pointer): an argument
freed prematurely by RC. It surfaces on real recursion over a polymorphic `==`
combined with ≥3 string-interpolation sites. It is non-deterministic in the
sense that the segfault depends on the CONTENTS of the freed cell — sometimes
the reused memory holds a value that survives the deref, sometimes it does not.
This is a use-after-free in the native path's polymorphic `==` dispatch, the
"arg-pass leak / owned variant discarded" class already tracked as open.

### Why CI is GREEN on `main` despite this

The CI `tier1-native` log on the `main` merge commit reads:

```
test-backend-parity: native vs c — pass=424 fail=57 skip=58 total=539
ratchet OK — native-parity failures match the baseline (0 gaps; flaky gaps held).
```

`tools/native-parity-baseline.txt` has zero active entries, so all 57 fails are
"new gap" candidates. The ratchet's `recheck_diverges()` re-runs each candidate
up to 3× and, if it converges AT LEAST ONCE, declares it "flaky, not a
regression" and drops it → "0 gaps" → green. CI runs 4 workers (parallel);
under contention the UAF sometimes does not segfault → converges → masked. In
SERIAL (1 worker, no contention) the UAF segfaults all 3 times → real gap.

**This is the documented false-green of the parallel ratchet**
(`feedback_kaikai_rc_change_needs_serial_parity`), confirmed structurally: 57
"flaky" parallel == 53 deterministic serial segfaults, masked by a recheck that
a non-deterministic RC bug clears by luck. `main` is already broken on the
native backend; the parallel ratchet tolerates it.

## Scope decision (asu-validated): ship div/rem alone

The div/rem patch is correct and innocent — it closes the perf residual that
was its only real objective and adds no new parity gaps. The "53 segfaults" the
brief asked to "fix at root" are a separate, pre-existing RC UAF in `==`
dispatch, a large native-RC lane of a different class. Per "one worktree fixes
one thing" and "do not couple a reshape with a consumer that does not need it,"
the two are unbundled:

- **Ship**: the div/rem patch + the `raw_scalar_divmod` fixture + this retro.
- **Filed**: issue #858 for the `kai_op_eq` UAF with the 8-line repro, the
  `0x8` backtrace, the two-platform reproduction, and the infra finding — the
  parity harness has a structural false-green for non-deterministic RC bugs
  (parallel recheck masks them; serial determines them).

### Honest success gate (NOT the parallel "0 gaps")

Because div/rem raw removes the boxed-result allocation of every `/` and `%`,
it changes the heap allocation rhythm, which can REORDER which fixtures flake
under the non-deterministic UAF (silence one, wake another) without moving the
count. So the gate is set-based, measured SERIALLY:

> `fails(patch) ⊆ fails(main)`, by fixture name, under
> `BACKEND_PARITY_JOBS=1`.

"Same number of fails" is insufficient — it would hide a silence-A/wake-B swap.
The serial subset check is what licenses the claim that div/rem introduces no
regression. (Result recorded at lane close.)

## Lessons

- The parallel ratchet's flaky-recheck is a false-green for ANY change touching
  RC / raw classification, AND it masks pre-existing non-deterministic RC bugs
  on `main` itself. The truth is the SERIAL run. The earlier attempt's 57
  "flaky" were 53 real serial segfaults — but they were ALREADY there, not
  introduced by the patch. Distinguishing "the patch broke it" from "main was
  already broken" requires building clean `main` and reproducing there FIRST.
- A cross-platform Docker bind-mount of the live worktree clobbers the host
  build (the in-container `make clean` + rebuild left ELF aarch64 binaries in
  `stage0/`/`stage1/`, which the mac then could not execute). Mount a `git
  archive` COPY, never the worktree; deep-clean before any host rebuild.
- Apple `clang`/llvm@18.1.8 reproduced the same crash as Ubuntu llvm 18.1.3, so
  the bug is LLVM-version-and-architecture independent, not a local artefact —
  established by reproducing in the exact CI container, not by assuming.

## Build (local)

```sh
make -C stage2 KAI_LLVM=1 \
  LLVM_CONFIG=/opt/homebrew/opt/llvm@18/bin/llvm-config \
  'LLVM_CXXLIB=-L/opt/homebrew/lib -lc++' kaic2
# Homebrew llvm@18 (18.1.8); the brief's vendored-LLVM path does not exist in
# this worktree. zstd lives in /opt/homebrew/lib.
```
