# Lane experience — issue #1234 (M:N work-stealing corrupts migrated fibers)

**Outcome: root-caused with `rr`, then fixed by splitting the runtime into a
clang-inlinable hot bitcode and a cc-compiled owner object.** The bug is a
native-backend (P2 bitcode) *miscompile*, not a scheduler-logic bug: clang -O2
caches the thread pointer across `swapcontext`, so a work-stolen fiber reads the
creator thread's `_Thread_local` scheduler state. The fix keeps the hot-path
perf (RC/arith/TRMC still inline) while compiling every suspend-point with cc,
where the thread pointer stays honest. This retro is the reusable artifact: how
a lost-cross-thread-state Heisenbug was run to ground with `rr`, why the obvious
per-function fixes do not work, and how the split is made sound *by construction*
rather than by enumerating a fragile blacklist.

## Scope as planned vs as shipped

- **Planned:** reproduce under `rr`, reverse-continue to the work-steal that
  migrated a fiber without rebinding its TLS, and rebind `uc_link` /
  `kai_main_fiber` / `kai_pending_free` in the steal handoff.
- **Shipped:** a complete diagnosis PLUS the build-architecture fix. The planned
  fix direction (rebind TLS in the steal path) is **incorrect** — the fibers do
  not "drag" TLS via swapcontext at all (a standalone test proves `swapcontext`
  does not carry the FS base). The true fault is one layer down, in codegen:
  clang caches the thread pointer across the switch. The fix splits the runtime
  so the scheduler is never clang-`-O1+`-compiled (hot bitcode = leaf ops only;
  scheduler = cc owner object at `-O0`) — see "The fix that shipped". The lane
  grew a scope it did not plan for: the addendum's "native-only" framing was
  wrong (the C backend hoists too under `CC=clang`), so the lane also had to pin
  the owner opt level rather than lean on gcc, and file #1238 for the C path.

## The bug (proven)

`KAI_THREADS>1` with work-stealing crashes ~7–55% depending on workload: a
worker thread `munmap`s a fiber stack **it is currently executing on**
(`$rsp` inside `[munmap addr, addr+len)`), because in the fiber's own trampoline
tail `kai_free_value`'s guard `v->as.fib == kai_current_fiber()` is **false** —
`kai_active_fiber` has diverged from the running fiber.

Root cause, pinned by `rr` reverse execution + disassembly:

> The native backend's **P2 optimisation links clang-`-O2`-compiled runtime
> bitcode** (`stage0/runtime_llvm.bc`). Clang `-O2` **caches the thread pointer**
> — the base it uses to address our `_Thread_local` scheduler state
> (`kai_active_fiber`, `kai_pending_free`, `kai_thread_id`, `kai_main_fiber`) —
> **in a callee-saved register (spilled to stack) across `swapcontext`.** The C
> abstract machine says a plain call never changes which OS thread runs, so this
> is a legal optimisation. But `swapcontext` under work-stealing **resumes the
> fiber on a *different* OS thread**, so the cached base still points at the
> **creator** thread's TLS block. A resumed fiber then reads/writes the creator
> thread's `kai_active_fiber` slot: two threads share one `active`, the scheduler
> cross-wires, and a live fiber's stack is freed under it.

The `rr` smoking gun (watchpoint on the main thread's `&kai_active_fiber`):

```
WRITE by LWP=1 fs_base=0x7b7a51481e80  <main>            (main's own slot)
WRITE by LWP=5 fs_base=0x1b853ba06c0   <kai_sched_park+688>   <-- worker LWP=5,
    its own fs_base is 0x1b853ba06c0 (slot would be ...0670), yet it writes
    0x7b7a51481e30 == MAIN thread's slot. A %fs-relative store would hit its own
    slot; it used a cached absolute address instead.
```

Disassembly confirms `lea -0x50(%rax),%rbp` (`&kai_active_fiber` from the TCB)
computed *before* `swapcontext`, and the post-swap store reusing the stale
`%rbp` (or reloading it from a spill slot).

## It is not native-only — it is *clang-at-`-O1+`-only* (the addendum was half right)

The addendum said "the bug is native, not C." That was an artefact of WHICH
compiler each backend happened to use, not a real backend distinction. The true
axis is the **compiler and its opt level**, measured on the cross-thread stress
fixture at `KAI_THREADS=4`:

| build                                             | result   |
|---------------------------------------------------|----------|
| native, P2 bitcode (always clang `-O2`)           | ~10–55% crash |
| native, P2 bitcode clang `-O0`                     | clean    |
| native, runtime `runtime_llvm.c` via **gcc** `-O2` | clean    |
| native, runtime `runtime_llvm.c` via **clang** `-O2`/`-O1` | crash (6/30, 3/40) |
| native, runtime `runtime_llvm.c` via **clang** `-O0` | clean (40/40) |
| **C backend** (`--backend=c`) via **gcc** `-O2`    | clean    |
| **C backend** (`--backend=c`) via **clang** `-O2`  | **crash (7/40)** |

So the C backend is **not** immune — it only looked immune because the earlier
lane, Docker, and prodigy all default `cc` to **gcc**. Compile the SAME C
backend with `CC=clang -O2` and the scheduler hoists exactly the same way. The
one true rule: **gcc does not cache the thread pointer across `swapcontext`;
clang at `-O1+` does, in whatever TU carries the scheduler.** This is codegen,
not scheduler logic — the same `runtime.h` is sound under gcc and under clang
`-O0`.

Native hit it first and unconditionally because the P2 bitcode is *always* clang
`-O2` — there is no gcc escape hatch on that path, which is why it was the
reported bug. The fix has to make the scheduler sound regardless of the
compiler, which is why the owner object is pinned to `-O0` (below), not merely
"prefer gcc."

## The `rr` session (the reusable lesson)

1. **`rr` cannot catch the *symptom* directly.** The corruption is a torn/stale
   cross-thread memory access; `rr` serialises threads onto one core, so 80
   `rr record --chaos` runs of the low-rate list fixture were all clean. `rr`
   eliminates the true-concurrency window by construction. Do **not** conclude
   "not reproduced under rr ⇒ fixed."
2. **Raise the rate first.** The shipped oracle (heap-carrying list messages)
   crashes ~7%. An **`Int`-message variant** (no deep-copy, pure
   spawn/send/receive) crashes ~45% — and `rr record --chaos` caught it on
   attempt 4. Higher rate = catchable under `rr`.
3. **Replay is deterministic once captured.** `break munmap if $rsp>=$rdi &&
   $rsp<$rdi+$rsi` stops exactly at the fatal self-stack free with the caller
   frame intact (`kai_fiber_trampoline → kai_free_value → munmap`).
4. **Reverse-watchpoint the diverged variable.** `watch *(unsigned long*)&slot`
   + repeated `reverse-continue`, printing `$_thread` / `$fs_base` / `info symbol
   $pc` at each write, showed a *worker* thread writing the *main* thread's TLS
   slot — the whole diagnosis in one screen.
5. **Confirm the mechanism out-of-band.** A 30-line standalone
   `getcontext`/`swapcontext`/`pthread` program proved `swapcontext` does **not**
   carry the FS base (TLS correctly follows the running thread) — killing the
   "fiber drags creator TLS via uc" hypothesis and pointing at codegen.
6. **Bisect the toolchain.** C backend vs native, native-with-bc vs without,
   clang `-O0/-O1/-O2` — localised the fault to clang-`-O2` bitcode.

## Fixes tried that do NOT work (all defeated on the bitcode path)

- **Rebind TLS / route yield+park through root + publish-after-save** (the
  planned fix): no effect. The fibers never carry stale TLS via swapcontext;
  workers in the fixture never even yield. Wrong layer.
- **`returns_twice` on wrapper and on the real `swapcontext`/`setcontext`:**
  LLVM ignores it for TLS-base invariance. ~43–55%.
- **`noinline` resume-fixup helper:** the native inline-bc merge **re-inlines**
  it (symbol absent from the binary); the stale-`%rbp` store returns.
- **Register-clobber barrier** (`asm volatile("":::"memory","rbx","rbp",...)`)
  after each switch: LLVM reloads the base from a **spill slot** — the clobber
  invalidates registers, not the compiler-managed stack copy of the address.
- **Removing the redundant post-swap `kai_active_fiber = current`:** no change;
  the miscompile is pervasive, not one write.
- **`optnone` on the 5 swapcontext-bearing scheduler functions:** no change —
  the vulnerable accesses also live in *callers* (mailbox recv, await, reactor
  park) that hold TLS across the migrating call. Per-function scoping does not
  contain it; only whole-TU `-O0` does.
- **`-ftls-model=initial-exec` in the bitcode:** no change.

## Codegen-flag investigation (no clean flag exists)

Tested on the clang-18 bitcode compile and the native re-optimisation; all still
crash ~45–55%:

- `-mllvm -tls-load-hoist=false` (the "eliminate redundant TLS address
  calculation" pass) — no help (slightly worse).
- `-mllvm -tlshoist`, `-mno-tls-direct-seg-refs`, `-femulated-tls` — no help.
- `-ftls-model=initial-exec` — no help.
- `KAI_NATIVE_OPT=0|1|2` (native/program opt level) — no help; the hoist is
  already baked into the `-O2` bitcode, and lowering the program opt does not
  undo it. Only the *bitcode* at `-O0` (or gcc) is sound.

Why no flag works: the miscompile is not one TLS pass. It is the generic O2
assumption that the thread pointer is **call-invariant** (GVN/CSE keep the TLS
base live, regalloc spills/reloads it) applied to **every** function that holds
scheduler TLS across a call that transitively reaches `swapcontext` (mailbox
recv, `await`, reactor park, …). The vulnerable set is transitive and large;
per-function `optnone`/`noinline` cannot contain it, and there is no flag to drop
the invariance assumption while keeping the rest of `-O2`.

## Sound configurations (verified)

- `KAI_THREADS=1` (the default): 10/10, always sound.
- C backend at any `N`: clean.
- Native with `KAI_NOSTEAL` (no cross-thread migration): 50/50 clean.
- Native with P2 disabled or bitcode at `-O0`: clean.

## The fix that shipped (Option 3 — split the runtime TU)

The scheduler's reliance on compiler `_Thread_local` state is incompatible with
cross-thread `swapcontext` under an optimiser that treats the thread pointer as
call-invariant. The shipped fix splits `runtime_llvm.c` in two along ONE macro,
`KAI_HOT_ONLY`:

- **hot bitcode** (`clang -O2`, merged into the program module so O2 inlines the
  hot loop): ONLY the leaf value/RC/arithmetic/string/list/variant/record/bit
  ops + TRMC — none of which reach `swapcontext`. Safe to inline.
- **owner object** (`cc`, **pinned to `-O0`** via `KAI_RUNTIME_OWNER_OPT`): the
  full runtime, including everything gated out of the hot bitcode (effects,
  evidence, continuations, handlers, the scheduler, actors, `main`). `-O0` is
  load-bearing, not incidental: `cc` may be clang (CI passes `CC=clang`, macOS
  `cc` is clang), and clang `-O1+` hoists the thread pointer here exactly as it
  does in the bitcode (owner clang `-O2`/`-O1` → 3/40 crash; clang `-O0` →
  40/40 clean; gcc clean at any `-O`). Only whole-TU `-O0` is sound under clang
  — per-function `optnone` leaks through the vulnerable *callers* (see "Fixes
  tried"). `-O0` costs only cold effect/scheduler dispatch; the hot
  value/RC/arithmetic path is the `-O2` bitcode, untouched.

Both native link paths now merge the hot bitcode AND link the cc owner object:
the whole-program path (`--emit=native`, the single-module default) previously
linked a self-contained bitcode with no owner; it now links the same owner the
native-modular path already used.

**Why a whitelist, not a blacklist (the key design choice).** The naive framing
— "gate OUT the swapcontext-reaching functions" — is a blacklist: miss one and
the runtime is silently unsound. Instead the hot bitcode is a *whitelist* of leaf
ops. Over-gating an op is harmless (the owner still defines it; it is merely not
inlined, a cold-path perf non-issue); under-gating a suspend-point op is caught
mechanically — `gen-runtime-bc.sh` asserts `llvm-nm` shows NO `swapcontext` in
the hot bitcode and fails the build otherwise. The soundness invariant is one
grep, checked at build time.

**Perf is preserved.** The ops the rb-tree gate exercises (incref/decref/add/
cctx/variant-slot/cons-tail) are all leaf and stay inlined — `llvm-nm` shows them
still `T` in the bitcode; the `#1083` inline gate holds at 6 residual calls
(threshold 50), and rb-tree wall time is unchanged (~1.8s, within noise of the
pre-split build). Only effect/scheduler ops — never the inlined hot path — moved
to the owner object.

### One structural surprise the split forced

Moving `main` to the owner object broke the whole-program link: the owner's
`main()` calls four program-emitted entry points (`kai_main`,
`_kai_proto_init_llvm`, `kai_main_install_defaults`,
`kai_main_teardown_defaults`) across the object boundary, but the whole-program
bitcode-merge internaliser kept only `main` external — before the split, `main`
lived in the merged bitcode and called `kai_main` intra-module, so that was
enough. Fix: widen `kai_llvm_internalize_except_main` (stage2/runtime.h) to keep
those four entry points external too. This is whole-program-only; the modular
root already emits them external.

### Alternatives considered and rejected

- **Disable P2 for the runtime** (cc-compile the whole runtime, no bitcode):
  sound and simple, but drops RC-op inlining for the common single-thread case —
  a real perf regression the split avoids.
- **`optnone`/`noinline`/register-clobber on the swapcontext functions:** all
  defeated (see "Fixes tried" above) — the vulnerable accesses live in the
  *callers* too, and the native inline-bc merge re-inlines `noinline`. Only
  whole-TU cc treatment contains it, which is exactly what the owner object is.
- **Redesign the migration-sensitive state off compiler-TLS** (fetch thread
  identity via an uncacheable primitive after each switch), or pin fibers to
  their home thread (drop work-stealing): largest scope, deferred — the split
  makes the existing design sound without touching scheduler logic.

### Validation (prodigy, Ubuntu 24.04, 4 cores)

- Baseline (pre-fix): the native stress fixture crashed 3/30 at `KAI_THREADS=4`.
- Post-fix: `KAI_THREADS=4` 100/100, `KAI_THREADS=6` 60/60, `KAI_THREADS=8`
  60/60 — 220/220 clean. The new gate `tools/run-mn-native-soundness.sh` (wired
  into `tier1-native`, Makefile `test-mn-native-soundness`) loops the fixture
  native at N=4/8 and asserts zero crashes; the C-backend + TSAN gates stay
  sound (they never saw this native miscompile, which is how it shipped).
- `llvm-nm` on both hot bitcodes: 0 `swapcontext` references.

### Follow-up left for the next lane

**The C backend under `CC=clang -O1+` is unsound the same way** (7/40 crash on
the same fixture) and is NOT closed here. This lane fixes the native path — the
reported #1234 — because native is the only path with a clean seam: the runtime
already links as a separate object (the owner), so pinning that one object to
`-O0` contains the scheduler without touching program code. The default C
backend is a *single* TU (program + `runtime.h` inlined), and the vulnerable
functions (`kai_core_mailbox_recv` ~7907, the reactor ~12000, the scheduler
~13000, `kai_fiber_value` ~3059) are scattered across ~11k lines interleaved
with the RC/arithmetic hot path — so there is no contiguous region to `-O0`, and
whole-TU `-O0` would kill program perf. A proper fix compiles `runtime.h`'s
scheduler as its own `-O0` object even on the C path (the `#748` separate-
compilation path already has the machinery), or moves the migration-sensitive
state off compiler-`_Thread_local`. Filed as #1238; not a regression (it was
always latent) and sound in CI, where the C path uses gcc.

## Traps that cost time (for the next lane)

- **The P2 bitcode is stale-cached.** `./bin/kai build` links
  `stage0/runtime_llvm.bc` and does **not** regenerate it. Editing
  `stage2/runtime.h` has **zero effect on the native binary** until
  `tools/gen-runtime-bc.sh --force` runs (needs `CLANG18`/clang-18). Every
  early instrumentation attempt silently ran the old runtime.
- **Native uses `stage2/runtime.h`, not `stage0/runtime.h`.** `gen-runtime-bc.sh`
  compiles `stage0/runtime_llvm.c` with `-I stage2 -I stage0`, so `<runtime.h>`
  binds to `stage2/`. `stage0/runtime.h` is the old bootstrap runtime with **no
  M:N** — a red herring.
- **`./bin/kai build` defaults to `--backend=native`.** A plain `kai build`
  "C-looking" test is actually native. Use `--backend=c` to test the C backend.
- **Instrumentation perturbs the race away.** `fprintf`/breakpoints on hot
  scheduler paths hid the bug (150 gdb runs, no repro). Prefer post-hoc `rr`
  replay + reverse-watchpoints over live tracing.
