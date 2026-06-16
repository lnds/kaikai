# Lane experience — native-codegen-perf

**Date:** 2026-06-16 · **Type:** diagnosis + plan (no codegen rewrite) ·
**Deliverable:** `docs/native-codegen-perf-plan.md` + `tools/native-perf/`

## Scope as planned vs. as shipped

**Planned:** confirm the 340 KB/6.2 s-vs-154 KB/0.07 s native divergence, fix the
opt-level wrapper one-liner if that was the cause, measure the honest native-vs-C
gap, diagnose the residual, write a priority-ordered plan.

**Shipped:** all of the diagnosis + plan + an honest benchmark harness. The
opt-level fix did **not** ship — because the bug it would fix does not exist in
HEAD. That is the lane's central finding.

## The brief's premise was stale

The brief's lead hypothesis — `KAI_BACKEND=native` leaves `opt_level` empty so
the `default<O2>` pipeline never applies — is **false in HEAD**:

- `bin/kai:1364` already does `export KAI_NATIVE_OPT="${opt_level:-2}"` (default 2).
- `kai_native_opt_pipeline()` (`runtime.h:12069`) returns `default<O2>` even when
  the env var is empty.
- O0 vs O2 native binaries are same-size but byte-different → the pipeline *runs*.

The `-O2`-default work was issue #498 (L4), already landed. The brief was written
against an earlier measurement. **Lesson: re-derive the reproduction on the
current build before trusting a brief's root-cause hypothesis** — I rebuilt
`kaic2` and reproduced first (340 KB/6.05 s confirmed), then checked the wrapper,
and the wrapper contradicted the hypothesis. Had I "fixed" the opt-level I would
have shipped a no-op.

## The real finding is more severe and more useful

Every native route is slow on scalars (~86× on pure arithmetic), regardless of
opt level. Root cause: the KIR walk is **all-boxed** (`emit_native_ops.kai:16`
says so literally), so every Int op is an opaque `kaix_*` runtime call. O2 cannot
penetrate external `declare`s (no LTO). Machine-level proof: the O2 native binary
issues ~30 `bl` per loop iteration; the C binary issues zero (O2 inlined the
unboxed loop away).

## Design decisions

- **Honest benchmarks via `N * (seed / seed)`.** `seed = string_length(program_name())`
  is a runtime value the optimiser cannot fold; `seed / seed` is 1 at runtime but
  the optimiser cannot prove `seed != 0`, so it cannot const-fold `N`. This fixes
  `N` deterministically *and* keeps it opaque — the cleanest way to defeat the
  const-fold that made the brief's pure-arithmetic bench misleading. `program_name()`
  carries no effect row (process-wide constant), so `main` stays `Unit / Stdout`,
  no `Env` handler needed.
- **First bench attempt used the binary-path length as the seed** — fragile (N
  ballooned to billions because mktemp paths are ~60 chars), and `list_fold`
  native ran for minutes. Switched to fixed N with the `seed/seed` opacity trick.
- **`deep_rec` needed a *varying* fib argument.** With `fib(32)` literal, O2
  const-folded the whole tree to a constant (C ran in 0.00 s). Fix: `fib(base + k
  % 3)` so the argument varies per iteration and cannot be CSE'd or folded.

## asu review — three corrections folded into the plan

1. **Attribution:** P1 kills *inlining barriers*, not allocs. Boxed Int is a
   tagged immediate (`kai_tagged_int`, `runtime.h:520`) — re-box is a bit-OR, not
   a heap alloc. The doc must not claim P1 "removes re-box allocs".
2. **Three correctness guards the Real path didn't need:** wrapping `add/sub/mul`
   (no `nsw`/`nuw`); `/` and `%` stay boxed in v1 (UB of `sdiv` on `/0` and
   `INT_MIN/-1`); lower `icmp`→`i1`→`condbr` directly.
3. **P2 reduced to one move:** `LLVMLinkModules2` of the runtime bitcode *before*
   O2 — not LTO-via-cc (toolchain dep) and not emit-inline (the double-source
   bug-class that killed the text-LLVM backend).

asu's honesty gate, recorded in the plan: do **not** promise the ~86× win
magnitude until P1 lands and the post-P1 `objdump` is measured.

## Structural surprises

- The native backend is **written in kaikai** (`emit_native_*.kai`,
  `native_prims.kai`) calling the LLVM C-API via FFI — not a `.cpp` shim. The opt
  pipeline lives in `runtime.h` under `#ifdef KAI_LLVM`.
- The unboxing information **already exists** in the AST (`unbox.kai` marks Int
  arith `MUnboxed`); the native walk simply discards it. P1 is "consume existing
  info", not "build a new analysis" — cheaper than it first appears.
- Native TCO already works (`sum_loop` is a loop, not self-recursion). The slow
  loop has the *right structure*, just a call-heavy body — which is why P1's win
  is plausible (empty the body, the loop is already there).

## Fixtures added

`tools/native-perf/` — harness (`run.sh`) + 6 benches spanning the gap spectrum
(pure arith ~86×, non-tail recursion ~30×, variant interp, list build/fold ~3.4×,
rb-tree corpus ~2×). Not wired into a CI tier (perf benches are diagnostic, not
gates); run on demand.

## Cost vs. estimate

Pure diagnosis lane; most of the cost was waiting on slow native benchmark runs
(the all-boxed binaries take seconds–minutes per run, the very symptom under
study). The classifier outage mid-lane forced several builds to background.

## Follow-ups for next lanes

- **P1 (Int unboxing)** — the high-value lane; scoped in §4 of the plan with
  file:line touch points and the three asu guards. Gate: selfhost byte-id +
  parity 0-gaps + ASAN + `KAI_TRACE_RC`, plus post-P1 `objdump` for the honest
  magnitude figure.
- **P2 (runtime bitcode link)** — second lane; closes the heap-bound residual.
- The benchmark harness could grow a CI smoke (1 fast bench, ratchet on the
  native-vs-C factor) once P1 makes the numbers presentable.
