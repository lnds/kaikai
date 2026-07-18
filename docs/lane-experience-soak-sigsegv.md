# Lane experience — soak-driven SIGSEGV in the M:N scheduler

## Scope

A 12h soak of `examples/effects/mn_cross_thread_copy_stress.kai` (backend C,
`CC=clang`) on a Linux x86_64 host surfaced a rare SIGSEGV in the M:N
scheduler under load: 12 crashes at `KAI_THREADS=4` over ~7.47M runs (~1/580k),
plus 30 at N=8 and 47 at N=16. macOS ARM64 never saw it in any run. The CI's
~30-run sample never hit it.

Scope as planned: find one x86-specific SIGSEGV. Scope as shipped: the soak
volume was masking **two independent scheduler races** that each produce the
same "resume a fiber with a corrupt context → garbage RIP" symptom. Both are
fixed here; either fix alone still crashes.

## What the two bugs are

**Bug 1 — steal of a half-saved context.** `kai_sched_park` (mailbox recv /
await / send-block path) set a fiber `PARKED` under its slot lock and then let a
cross-thread `remote_unpark` publish it onto the steal deque **before** the
fiber's own `swapcontext` had finished writing its `ucontext`. A thief that
stole and resumed the fiber in that window restored a half-written context and
jumped to a garbage RIP. The reactor-park path already avoided this by deferring
the `PARKED` publish to the scheduler root (after the exit swap saved the ctx);
the non-reactor path did it inline. Fix: route the non-reactor park through the
same deferred-commit-to-root mechanism (new `KAI_PARK_SLOT` reason). `PARKED`
is now published from the root's stack after the ctx is saved; a wake that
races the commit takes `remote_unpark`'s `wake_pending` path, which
`kai_sched_commit_park` re-checks under the slot lock before parking.

**Bug 2 — trampoline-tail stack use-after-free.** The fiber trampoline's
DONE/CANCELLED tail dropped the scheduler's ref on its own wrapper
(`kai_decref(self->value)`) while it was **still running on its private stack**
— the subsequent `kai_sched_dequeue` + `setcontext` execute on that stack. When
a peer thread held the last other ref (a discarded `Fiber[T]` handle, e.g.
`let _ = spawn_actor(...)`), that peer could take RC to 0 and `munmap` the stack
out from under the running tail. The existing deferral only covered the
*self*-drops-last case (`kai_pending_free`); the peer-drops-last case freed
immediately. Fix: at N>1 the tail stashes its ref in a per-thread
`kai_pending_sched_drop` slot instead of decref'ing inline, keeping RC ≥ 1
across the tail; the next context drops it (folded into the pending-free drain)
once `setcontext` has left the doomed stack.

Both changes gate on `kai_nthreads > 1`; the N=1 path is byte-identical.

## Why x86-only

Both are genuine races present on any architecture; only x86 materialized them
in the soak. This matches the brief's hypothesis — the fault is *structural*
(a corrupted saved RIP / a freed stack), not a pure memory-ordering race, so
the arch with the weaker model (ARM) exposing *fewer* crashes is informative:
the ARM microarch's timing simply kept both windows closed in practice, while
x86's scheduling/codegen timing opened them at ~1/580k.

## Method (what actually worked, and what rr did)

- Reproduced on a Linux x86_64 host with parallel oversubscribed loops
  (N=16/8, more fibers than cores): the crash surfaced in ~30s under contention
  versus ~1/580k in the soak.
- `rr record -h` (chaos) reproduced readily (~1/300 attempts) but **aborted its
  own recording** on the corruption: the wild control flow executed a `futex`
  syscall from a garbage RIP (`0x70000002`), which trips rr's
  `process_syscall_entry` consistency check. rr therefore could not replay the
  garbage-RIP variant — informative in itself: the fault is a jump into
  data/garbage from a corrupted return address / saved context, not a clean
  null-deref.
- A revertible SIGSEGV-handler diagnostic (dump the faulting `ucontext` RIP/RBP
  and walk the saved rbp chain, resolved offline against a known symbol anchor)
  pinned the fault to `swapcontext` called from `kai_worker_loop`, restoring a
  `next->ctx` whose saved RIP was garbage/NULL — a fiber resumed on a corrupt
  context.
- A **leak experiment** (never `free`/`munmap` a fiber struct or stack)
  isolated bug 2: with fibers leaked *and* the park fix applied, 200k+ runs were
  clean; with either fix alone the crash returned. Differential builds
  (park-fix ±, leak ±) proved the two bugs are independent and each necessary.

## Fixtures and coverage gap

`examples/effects/mn_cross_thread_copy_stress.kai` is the bug-shape fixture
(cross-thread message copy + fiber migration) and is already wired into
`tools/run-mn-{tsan,determinism,c-clang-soundness,native-soundness}.sh` and the
`tier1-tsan` CI tier. **A single CI run cannot catch a ~1/580k race** — the
soak harness (millions of runs) is the real coverage. This is the honest limit:
the fixtures assert TSAN-cleanliness and N=1==N=4==N=8 determinism on every run,
but the crash frequency is a soak-only signal.

## Verification

- 240k+ `KAI_THREADS=4` runs under 5-loop contention: 0 SIGSEGV, 0 hang, 0
  bad-output (the unfixed build crashes fast under the same contention).
- `make tier0` (incl. stage1+stage2 selfhost): green.
- `run-mn-determinism`: N=1==N=4==N=8 for every fixture.
- `run-mn-tsan`: TSAN clean + deterministic (the local host's high ASLR entropy
  makes TSAN's shadow init flaky — "unexpected memory mapping" at a different
  PIE base each run; it passes under `setarch -R`. That flakiness is
  environmental, pre-existing, and unrelated to the fix; CI's tier runs it in a
  compatible environment).

## Follow-ups

- The soak also logged hangs (exit 137) on both arches at all N — a suspected
  separate lost-wakeup, out of scope here. The N=4 post-fix runs showed 0 hangs
  in 240k, but that is not enough volume to call the hang fixed; leave it as a
  separate soak-tracked item.
