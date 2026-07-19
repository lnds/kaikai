# Lane experience — issue #1258 (M:N thread affinity) + #1238 (release gate)

Two-part lane. Part 1 unblocked the v0.102.0 tarball; part 2 addressed the
thread-affinity findings behind the M:N lost-work symptom.

## Part 1 — #1238: the parking op that lost its `KAI_SCHED_FN` gate

### Symptom

`scripts/build-release.sh` died on the macOS ARM64 build of v0.102.0:

```
kai: error: C backend: swapcontext leaked into the -O2 program object
```

The tarball was never produced, so the Homebrew and mirror steps were skipped.
The failure reproduces locally with a fresh `kaic2`, which made this a
diagnosable bug rather than a CI-only mystery.

### Root cause

#1238 established that clang at `-O1+` caches the thread pointer across
`swapcontext`, so a stolen fiber reads the *creating* thread's scheduler TLS.
The fix was to keep every fiber suspend-point op out of the program's `-O2`
translation unit, referenced through `KAI_SCHED_FN`. `bin/kai` asserts this
after compiling: it scans the `-O2` object for a residual `swapcontext`.

The seven `kai_default_file_*` handlers were never gated. They are parking ops
by the same definition as their Clock / NetTcp / Stdin / Process / Signal peers
— each calls `kai_reactor_run_in_pool`, which parks the fiber via
`kai_sched_park` and therefore reaches `swapcontext`. Their peers all carry
`KAI_SCHED_FN`; the File handlers were `static`, so the whole park/reactor
closure they anchor stayed in the program TU.

The reason this surfaced now rather than at #1238's close: the gate only fires
for a program that actually *reaches* the File handlers. `kai-pkg` is the first
release-path binary that does heavy file I/O. Every earlier release built
programs whose reachable set excluded them, so the gap sat latent behind a
green gate.

Worth noting as the diagnostic tell: `kai_reactor_inited` (the *state* the
closure mutates) was correctly `extern` in the non-owner TU, while the
*functions* mutating it were `static`. That asymmetry — gated state, ungated
code — is the signature of a handler family that was missed in a sweep.

### Fix

`KAI_SCHED_FN` + the `KAI_SCHED_DECL_ONLY` body pattern on all seven handlers,
matching the form already used by `kai_default_clock_sleep_ns` and peers.

### Verification

- `KAI_BACKEND=c ./bin/kai build tools/kai-pkg/main.kai` compiles; the binary runs.
- Instrumented `bin/kai` to dump the `-O2` object on gate failure: `swapcontext`
  is gone from the program TU after the fix.
- `scripts/build-release.sh` runs to completion and packs
  `kaikai-v0.102.0-darwin-arm64.tar.gz`, with both backend smoke tests green
  and selfhost byte-identical.

One environment trap met on the way: `build-release.sh` refused with "native P2
runtime bitcode is NOT active (status: optout)". That is not a code failure —
editing `runtime.h` invalidates the bitcode stamp, and `tools/gen-runtime-bc.sh`
must be re-run. Worth knowing before diagnosing it as a regression.

## Part 2 — #1273 / #1274 / #1275: fiber thread affinity

### Measurement methodology — and why the first numbers were untrustworthy

This deserves stating up front, because the first three rounds of measurement
produced numbers that led nowhere.

Failures were initially counted by "stdout is not `1000`", with several 400-run
loops sharing the machine. That produced a table where the fixes looked worse
than baseline at N=4 and better at N=8 — a contradiction that is the signature
of variance dominating the signal, not of a fix working:

| | N=4 | N=8 |
|---|---|---|
| baseline (`main`) | 2/400 (0.5%) | 11/400 (2.75%) |
| #1273+#1274 | 13/400 (3.25%) | 3/400 (0.75%) |

Three methodology errors, each worth avoiding next time:

1. **Concurrent measurement loops.** The empty-output rate is contention-
   sensitive, so running two 400-run loops in parallel changes the very thing
   being measured. Numbers from parallel arms are not comparable to numbers
   from serial ones.
2. **No timeout classification.** Early loops could not distinguish "empty
   output" from "hung process". Stale `mn_final` processes accumulated for 35
   minutes and silently absorbed machine capacity, skewing everything measured
   alongside them. `gtimeout 20` plus explicit ok/empty/hang counting fixed
   this — and showed the hang count is actually zero.
3. **Sequential A then B.** Thermal state and background load drift between
   arms. The trustworthy shape is alternating A/B in one serial loop, so any
   drift hits both arms equally.

At a base rate near 1%, distinguishing a real effect from noise needs either
many hundreds of alternating rounds or a host that reproduces the failure more
often. Worth budgeting for before promising a before/after table.

The alternating serial A/B is the number to trust. 300 rounds per arm at N=8,
nothing else running on the machine, baseline and fixed binaries interleaved
one run each per round:

| Arm | Failures / 300 |
|---|---|
| baseline (`main`) | 8 (2.7%) |
| #1273 + #1274 | 9 (3.0%) |

A difference of one failure in 300 is nothing. **The affinity fixes have no
measurable effect on the #1258 symptom** — which is the honest headline, and
consistent with the root-cause finding below: the failure is in the evidence
chain, not in the affinity pointers these fixes correct.

### What the fixtures actually showed

Measured before touching anything, on a macOS ARM64 host, C backend:

| Config | Before |
|---|---|
| `KAI_THREADS=1` | 1000 (correct) |
| `KAI_THREADS=4`, 150 runs | 0 bad |
| `KAI_THREADS=4`, 200 runs | 1 bad (empty output) |

The empty-output symptom reproduces, but at roughly 0.5% rather than the ~15%
the brief cited. Either the rate is host- and load-sensitive, or an intervening
fix moved it. Stated plainly because it changes how much signal a 500-run loop
carries: at this rate, a clean 500-run result is consistent with a fix but is
not by itself strong evidence.

### The three findings, confirmed in code

- **#1273** — `kai_sched_enqueue` publishes a fiber onto `kai_sched_slot()` (the
  *current* thread's slot) while `home_thread` keeps its `calloc` zero. Only the
  steal path (`kai_sched_steal_from`) and the roots ever stamped it. A fiber
  spawned on thread 2 therefore claimed to live on thread 0, so slot locks and
  cross-thread unparks addressed the wrong slot.
- **#1274** — `kai_fiber_init_ctx` set `f->ctx.uc_link = &kai_main_fiber.ctx`,
  and `kai_main_fiber` is `KAI_TLS`. The address is baked at spawn time and
  belongs to the spawning thread; a stolen fiber falling off the end of its
  context would resume a root another thread is actively running on.
- **#1275** — `kai_free_value`'s `KAI_FIBER` branch guarded only with
  `v->as.fib == kai_current_fiber()`. Under M:N, "not the current fiber" does
  not imply "dead" — it may be running on another thread, and the `else` branch
  `munmap`s its live stack.

### Fixes

- **#1273** — stamp `home_thread = kai_thread_id` inside `kai_sched_enqueue`,
  under the same slot mutex that publishes the fiber onto the steal list, so a
  reader that can see the fiber can also see coherent ownership. This mirrors
  the discipline `kai_sched_steal_from` already used.
- **#1274** — `uc_link` now points at a per-thread context whose entry point
  (`kai_fiber_uc_link_landing`) resolves `kai_main_fiber` at landing time,
  i.e. on the executing thread, instead of naming a TLS address at spawn time.
- **#1275** — **attempted and reverted.** See below.

### #1275: the fix that made things worse

The finding is real: `kai_free_value`'s guard (`v->as.fib ==
kai_current_fiber()`) does not exclude a fiber running on *another* thread, so
the `else` branch can `munmap` a live stack.

The obvious fix — defer whenever the fiber is `KAI_FIBER_RUNNING`, with a
matching check in `kai_drain_pending_free` — is wrong, and the code says so if
you read the comment above the assignment:

> The single-slot pending pointer is sufficient because the only producer is
> the trampoline tail, and every consumer drains before any other produce can
> run.

Deferring foreign fibers adds a **second producer**, and guarding the drain
makes the consumer sometimes *not* consume. Together they break the invariant
the single slot depends on: the next `kai_free_value` that defers overwrites
`kai_pending_free`, leaking the fiber that was sitting there.

The measurements flagged it: at N=4, 400 runs, baseline 2 empty (0.5%) versus
25 empty (6.3%) with the attempt. Reverted; #1273 and #1274 are unaffected
(they change ownership stamping, not the free discipline).

The reasoning stands on its own regardless of the measurement noise discussed
below: adding a second producer to a single-slot structure whose comment states
"only one producer" is wrong by inspection, and a slot that is skipped by a
guarded consumer is overwritten by the next producer. That is a leak, not a
judgement call.

A correct #1275 needs the pending-free slot turned into a list first. That is a
change to the free discipline in its own right and belongs in its own lane, not
bolted onto an affinity fix.

**Process lesson:** when a fix touches a data structure whose comment states an
invariant, the invariant is part of the interface. This one was stated one line
above the code being changed and still got missed on the first pass — only the
before/after measurement surfaced it. Measure the fix against baseline under
identical load, or a regression this size reads as noise.

### The fix that had to be reverted

The first #1274 attempt also re-pointed `uc_link` at the top of
`kai_fiber_trampoline`, reasoning that the executing thread is only known once
the fiber runs. That regressed `mn_cross_thread_copy_stress` from OK to
HANG/FAIL (`effect not handled in fiber: Actor`).

The lesson is worth carrying: **do not write into `self->ctx` while running on
that context.** `swapcontext` reads and writes the same structure, and the
trampoline runs on the very context it was mutating. Initialising `uc_link`
once in `init_ctx` to a thread-resolving landing gets the same correctness
without touching a live context.

This was caught only because the determinism harness was run against a clean
`main` for comparison. Comparing a suspicious result against baseline —
rather than against expectation — is what separated "my regression" from
"pre-existing".

### TSAN: what it caught, and its limit here

TSAN did surface the shape of the problem: two threads writing the same 8 bytes,
one side in `kai_sched_park`, the other in `kai_fiber_trampoline` /
`kai_mailbox_pop`, on a heap block that is a fiber's `mmap`ed stack.

But two facts bound what TSAN can settle:

1. **TSAN does not model `swapcontext`.** It assumes one stack per thread. A
   fiber that migrates between threads therefore *always* looks like two threads
   writing one stack, whether or not the handshake is correct. Stack frames come
   back as long runs of `_ctx_start`, which is the tell.
2. **The 2 remaining reports are pre-existing.** Stashing all changes and
   re-running `tools/run-mn-tsan.sh` against clean `main` produces the same 2
   reports. They are not regressions from this lane, and this lane does not
   close them.

Also relevant: `tier1-tsan.yml` is **green on CI (Linux x86_64)** while the same
harness reports 2 races on ARM locally. That inverts the usual weak-memory
expectation for this lane — here ARM is not exposing a real ordering bug that
x86 hides; it is the sanitizer's context-switch blindness interacting with a
different scheduling shape. Do not read the local ARM TSAN output as a merge
gate without first diffing it against baseline.

One build trap for whoever runs TSAN next: the fixture must be built with
`-std=c11` (what `tools/run-mn-tsan.sh` uses). Hand-rolling the command with
`-std=c99` produces a binary that SIGSEGVs at `KAI_THREADS=1`, which looks like
a dramatic runtime bug and is only a flag mismatch. Use the harness.

### Verification

- `mn_cross_thread_copy_stress`: 1000 at N=1, N=4, N=8.
- `tools/run-mn-determinism.sh`: the target fixture OK
  (`N=1==N=4==N=8: 1000`). The two other fixtures (`parallel_actors`,
  `mn_reactor_io_cpu_mix`) still HANG/FAIL — verified identical on clean `main`,
  so pre-existing and out of this lane's scope.
- selfhost byte-identical.
- rb-tree corpus: 1.29s vs 1.28s baseline (steady state) — no single-thread
  regression, as expected since the affinity work is gated on `nthreads > 1`.

## Follow-ups left open

- The 2 ARM-local TSAN reports on `mn_cross_thread_copy_stress`. They need a
  judgement on whether the harness should suppress `swapcontext`-induced stack
  aliasing, or whether an annotation should teach TSAN about the fiber
  switch. Currently the ARM run of this gate cannot reach zero.
- `parallel_actors` and `mn_reactor_io_cpu_mix` HANG/FAIL at N=4/N=8 on ARM,
  pre-existing and unrelated to affinity.
- The empty-output rate measured here (~0.5%) is far below the reported ~15%.
  Confirming the fix on a host that reproduces the higher rate would give the
  affinity fixes stronger evidence than this host could produce.
