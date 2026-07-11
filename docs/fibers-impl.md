# fibers — runtime implementation

The runtime side of the fiber model. Pairs with
`docs/structured-concurrency.md` (surface and lifetime rules),
`docs/actors.md` (mailbox + supervision surface), and
`docs/effects-impl.md` (CPS + handler-stack runtime — the "Doc C"
that this document extends with the m8.x scheduler).

The m8 v1 runtime was inline-eager: `Spawn.spawn` ran the thunk
synchronously, `await` was identity, `select` could not interleave,
`Cancel` was never delivered, `Actor.receive` errored on empty
mailbox. m8.x replaces that with a real cooperative scheduler. The
**type surface does not change** — every op signature and handler
shape from m8 v1 is preserved. The user-visible difference is that
fibers actually suspend.

This document is the runtime spec for **R2** in the production-honest
roadmap (`~/claude/kai.md` Phase R). It pins what the scheduler does;
the surface stays under `structured-concurrency.md` and `actors.md`.

## Substrate
<!-- coverage: skip --> design choice (ucontext vs CPS reification), not a testable feature

**`ucontext` on POSIX**. `swapcontext` / `getcontext` / `makecontext`
are the primitive substrate for stack switching. Available on macOS
arm64, Linux x86_64, Linux aarch64 — the three MVP targets in
`docs/design.md` §*Distribution and bootstrapping*.

Rejected: full CPS reification through `KaiCont`. That path requires
the CPS transform to capture the rest of the caller's body as a heap
closure at every op-call site (Doc C §*resume representation*
§*One-shot case* explicitly defers it as "a later milestone"), and
opens the typer/emitter while runtime work is in flight. Path A
(ucontext) keeps the change containment to `stage0/runtime.h` plus
this doc; the CPS path stays available for a future WASM port.

### macOS deprecation handling

Apple deprecated `ucontext` in macOS 10.6 (POSIX-2008 obsoletion). The
functions still work; the deprecation is a documentation flag, not a
removal warning. `_XOPEN_SOURCE 600` does not silence it — Apple
marked the prototypes with explicit deprecation attributes. The
runtime wraps the include and the call sites with a local pragma:

```c
#if defined(__APPLE__) || defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <ucontext.h>
#if defined(__APPLE__) || defined(__clang__)
#  pragma clang diagnostic pop
#endif
```

The same pragma envelope wraps every `swap/get/makecontext` call in
`stage0/runtime.h`. Verified clean under `-Wall -Wextra -Werror`
during R2 Phase 0.

### Stack model

Each fiber owns a heap-allocated private stack. v1 ships a fixed
size; the budget is a pure runtime parameter, no language surface.

- **Default size**: 64 KB.
- **Grow strategy**: none in v1. Stack overflow is detected at
  the guard page (see *Allocation*); programs that need deeper
  recursion configure the size.
- **Configuration**: environment variable `KAI_FIBER_STACK_SIZE`
  (bytes; rounded up to a page-size multiple — 4 KiB on Linux /
  x86_64, 16 KiB on macOS arm64). Read once on first
  `Spawn.spawn`. Out-of-range values fall back to the default and
  log a warning to stderr.
- **Allocation**: `mmap(MAP_PRIVATE | MAP_ANON)` of `stack_size +
  one page`, with the bottom page (low address) flipped to
  `PROT_NONE` via `mprotect`. Stacks grow down on x86_64 / arm64,
  so the guard sits at the overflow target. A SIGSEGV/SIGBUS
  handler installed on first `Spawn.spawn` runs on a `sigaltstack`,
  detects faults inside the active fiber's guard, prints
  `kai: fiber stack overflow at <ptr>` to stderr, and re-raises
  with the default disposition so the process exits with the
  standard signal-killed status. Faults outside any guard fall
  through to default (we do not interfere with NULL derefs etc.).
  Coverage: `examples/effects/m8_fiber_stack_overflow.kai`.

Grow strategies considered and rejected for v1:

- **Segmented stacks**: Effekt-style. Doc C §*Out of scope for v1*
  rules them out for the régime-C runtime.
- **Copy-on-grow**: simple but breaks any pointer into the stack
  (frame pointers, jmp_buf, captured `&local` from inline asm). The
  setjmp/longjmp validation in Phase 0 relies on stack stability.

## Fiber state machine

```
                              n.cancel(f)
                              [phase 3]
                       ┌────────────────────┐
                       │                    │
         spawn         │     dispatch       ▼
NEW ────────────► READY ───────► RUNNING ──► CANCELLED
                       ▲           │  │            │
                       │           │  │            │
                       │  yield/   │  │  thunk     │
                       │  unpark   │  │  returns   │
                       └───────────┘  │            │
                                      ▼            ▼
                                    PARKED        DONE
                                      │            ▲
                                      │  unpark    │
                                      └────────────┘
                                      (await/recv/
                                       send wakeup)
```

```
typedef enum {
    KAI_FIBER_NEW       = 0,
    KAI_FIBER_READY     = 1,  /* m8.x: enqueued in run queue */
    KAI_FIBER_RUNNING   = 2,  /* m8.x: currently executing   */
    KAI_FIBER_PARKED    = 3,  /* m8.x: blocked on await/recv */
    KAI_FIBER_DONE      = 4,
    KAI_FIBER_CANCELLED = 5
} KaiFiberState;
```

m8 v1 used only `NEW` and `DONE` (plus `CANCELLED` as a flag-bearer
that never actually fired). m8.x adds `READY`, `RUNNING`, and
`PARKED`. The numeric values are not stable across versions — there
is no ABI commitment yet (CLAUDE.md *Backward compatibility — not
promised until post-MVP*).

Transitions:

- **NEW → READY**: in `Spawn.spawn`. The fiber's `ucontext_t` is
  initialised via `getcontext` + `makecontext`; the trampoline is
  enqueued at the run queue tail.
- **READY → RUNNING**: when the scheduler dispatches the fiber.
- **RUNNING → READY**: on `Spawn.yield()` or any other yield point
  that does not need parking (cooperative preemption only).
- **RUNNING → PARKED**: on `Spawn.await(fib)` when `fib` is not
  DONE; on `Actor.receive()` with empty mailbox; on `Actor.send` to
  a full `BlockSender` mailbox.
- **PARKED → READY**: when the awaited event happens — the
  awaitee's trampoline walks its awaiter list, the mailbox push
  wakes a parked receiver, the mailbox pop wakes a parked sender.
- **RUNNING → DONE**: the thunk returns normally. The trampoline
  stores the result, walks the awaiter list waking each one, then
  swaps to the scheduler.
- **RUNNING → CANCELLED**: a `Cancel.raise()` injection completes
  the unwind. The trampoline drops the thunk's working set and
  walks the awaiter list with a `Cancelled` cause; awaiters wake
  and propagate `Cancel` themselves (Phase 3).

## Scheduler

Single-threaded cooperative scheduler. One ready queue (intrusive
singly-linked list, head/tail), already declared in m8 v1
(`stage0/runtime.h:271-272`).

```c
struct KaiFiber {
    KaiEvidence  *evidence_top;
    int           cancel_requested;
    int           cancel_delivered;
    KaiFiber     *sched_next;       /* run queue link */
    KaiFiber     *parent;
    KaiFiberState state;
    KaiValue     *thunk;
    KaiValue     *result;

    /* m8.x additions */
    ucontext_t    ctx;              /* swap target */
    void         *stack_base;       /* malloc'd; freed on RC=0 */
    size_t        stack_size;
    KaiFiber     *awaiters_head;    /* await wakeups go here */
    KaiFiber     *awaiters_next;    /* link for the awaiter list */
};
```

The `awaiters_*` pair forms a per-fiber awaiter chain. A fiber `b`
awaiting fiber `a` enters `a->awaiters_head` via its own
`b->awaiters_next` slot. When `a` finishes, the trampoline walks the
chain enqueueing each awaiter back to READY.

### Dispatch loop

The scheduler is not a separate thread or context — it lives on the
**main fiber's stack** (the OS thread stack). Every fiber's
`uc_link` points at `kai_main_fiber.ctx`, so when a fiber's body
returns, control flows automatically to the dispatch loop.

```c
static void kai_sched_dispatch(void) {
    while (kai_ready_head != NULL) {
        KaiFiber *next = kai_sched_dequeue();
        next->state = KAI_FIBER_RUNNING;
        kai_active_fiber = next;
        swapcontext(&kai_main_fiber.ctx, &next->ctx);
        /* control returns here when `next` yields, parks, or
         * completes. `kai_active_fiber` is updated by the yield
         * primitive; we re-read it before looping. */
    }
}
```

The loop terminates when the run queue is empty *and* no fibers are
parked. If parked fibers exist with no path to wakeup (deadlock),
the program panics: `kai: deadlock — N fibers parked, run queue
empty`. The panic includes the parked count for diagnosis.

### Yield primitives

Three primitives in `stage0/runtime.h`:

- `kai_sched_yield()` — caller stays READY, swap to next ready or
  to dispatch loop. No-op if the run queue is empty.
- `kai_sched_park(reason)` — caller goes PARKED, swap to dispatch
  loop. The caller must have already linked itself into a wakeup
  list (awaiter chain, mailbox waiter list) before calling park.
- `kai_sched_unpark(target)` — target fiber goes PARKED → READY,
  enqueued at the run queue tail. Caller stays RUNNING (does not
  yield); the unparked fiber runs whenever the scheduler reaches it.

`kai_active_fiber` replaces `kai_main_fiber` as the value returned
by `kai_current_fiber()`. The pointer is updated in
`kai_sched_dispatch` before `swapcontext`. Per-fiber `evidence_top`
is reached via `kai_current_fiber()->evidence_top`, so the evidence
vector switches automatically with the active fiber (Doc C
§*Per-fiber isolation* §"Decided" — the design pre-committed to this
shape).

## Yield-point list

Every operation that may suspend the caller. Phase 3 (Cancel
delivery) checks `cancel_requested && !cancel_delivered` at each of
these and injects `Cancel.raise()` instead of dispatching the op
when the flag is set.

| Op                       | Suspends? | Phase landed |
|--------------------------|-----------|--------------|
| `Spawn.yield()`          | always    | Phase 2      |
| `Spawn.spawn(thunk)`     | never (returns immediately) | Phase 2 |
| `Spawn.await(fib)`       | iff `fib` not DONE | Phase 2 |
| `Spawn.select(fibs)`     | iff none of `fibs` DONE | Phase 2 |
| `Spawn.cancel(fib)`      | never     | Phase 2 (flag set) / Phase 3 (delivery) |
| `Actor.receive()`        | iff mailbox empty | Phase 4 |
| `Actor.send(p, msg)`     | iff `p` is `Bounded(_, BlockSender)` and full | Phase 4 |
| `Actor.self()`           | never (pure lookup) | already in m8 v1 |

Phase 3 also lands a generic Cancel-delivery hook in the op-dispatch
prologue (`kai_evidence_lookup_node`), so any user-defined effect's
op call becomes a yield point too — without per-op runtime
modification.

## Trampoline

Each fiber's `ucontext_t` is initialised with a **trampoline** as
the entry function, not the user's thunk directly. The trampoline:

1. Calls `kai_apply(fiber->thunk, 0, NULL)` to run the body.
2. Stores the return value in `fiber->result`.
3. Sets `fiber->state = KAI_FIBER_DONE`.
4. Walks `fiber->awaiters_head`, re-enqueueing each awaiter as
   READY (calling `kai_sched_unpark`).
5. Returns. ucontext follows `uc_link` back to
   `kai_main_fiber.ctx`, which is the dispatch loop.

The trampoline is **the only place** that flips a fiber's state to
DONE; it is the only place that walks the awaiter list. Both
operations stay on a single thread of control, so no synchronisation
is needed.

For `Cancel.raise()` injection (Phase 3), the trampoline gains a
`setjmp` landing pad: the fiber's body is wrapped in a setjmp-guarded
region, and the Cancel injection longjmps back to the trampoline,
which then transitions to CANCELLED instead of DONE.

## Balance invariant under fiber boundaries
<!-- coverage: skip --> RC invariant rationale, internal contract

Doc C §*Per-fiber isolation* §"Balance invariant" requires that
`evidence_top == floor` at every fiber-yield point and at fiber
exit. The compiler enforces this by emitting syntactically balanced
`handle` push/pop pairs.

m8.x extends the invariant: a `handle { ... }` block must complete
within the same fiber's body that opened it. Stated negatively: the
following is rejected at compile time:

```kai
spawn {
  handle { ... }   # OK — body opens and closes inside this fiber
}

handle {
  spawn { ... }    # OK — child fiber inherits parent's evidence
                   #      via floor-pointer copy, NOT via handle
                   #      sharing
}

# Not representable in source: a handle that pushes in fiber A and
# pops in fiber B. Source syntax is lexical; spawn is opaque to the
# typer's lookahead. The invariant holds by construction.
```

This closes Doc C OQ #8 (*Balance invariant under non-lexical fiber
boundaries*) for v1: the lexical guarantee is preserved because
`spawn` does not split a handle across the boundary. The child
fiber inherits its parent's `evidence_top` *as its floor* (a
read-only snapshot of the pointer); pushing on top is allowed,
popping past the floor is a compiler bug.

A future construct that *does* split a handle across fibers —
`yield_into(parent_handle)` or similar — would re-open the
question. None is on the roadmap.

## RC contract under linear-consume runtime (post v0.2.0)

R1 Phase 3 (`fbb532c`) flipped the runtime to linear-consume: every
primitive consumes its args, every producer increfs on extraction,
hand-written runtime helpers (`kai_core_map/_filter/_reduce/_each`)
incref each arg before `kai_apply`. R2's Spawn handlers must respect
this contract.

Specifically:

- `kai_default_spawn_spawn(self, thunk, k)`: the thunk arrives owned
  by the handler. The handler stores it in `f->thunk` (donating the
  ref). The trampoline calls `kai_apply(f->thunk, 0, NULL)`, which
  is **not** flipped (CHANGELOG v0.2.0 line 50: `kai_apply` is one
  of the four intentionally-not-flipped primitives). `kai_apply`
  treats `f->thunk` as a borrowed pointer; the trampoline decrefs
  `f->thunk` after the call.
- `kai_default_spawn_await(self, fib_v, k)`: `fib_v` arrives owned;
  the handler decrefs it after extracting `f`. The result is
  produced via `kai_incref(f->result)` (extraction site), donated
  to the continuation.
- `kai_sched_unpark(target)`: takes a borrowed pointer. The
  scheduler does not adjust RC.
- Awaiter list walk: each entry is a borrowed pointer; the awaiter
  fiber's RC stays unchanged. The owning reference is held by the
  parent nursery / spawning frame, not by the awaiter chain.

If a handler reuses an arg across multiple call sites (e.g. a
`select` that re-enqueues a thunk), it must `kai_incref` between
uses, mirroring the pattern in `kai_core_map` (CHANGELOG v0.2.0
line 54-56).

## Phasing

R2 lands across five phases. Each phase is its own commit (or a
small set), gates with `make selfhost` + `make test`, and merges
via PR (per the policy established by R1 PR #1).

| Phase | Status | Scope | Demo gate |
|-------|--------|-------|-----------|
| Phase 2 | ✅ landed | Scheduler core (KaiFiber.ctx, run-queue, 5 Spawn handler rewrites, this doc) | `examples/effects/m8x_2_yield_interleave.kai` |
| Phase 3 | ✅ landed | Cancel delivery (yield-point hook in `kai_evidence_lookup_node`, setjmp landing in trampoline) | `examples/effects/m8x_3_cancel_at_yield.kai` |
| Phase 4 | ✅ landed | Blocking primitives (mailbox waiter queues, `Actor.receive` parking, `BlockSender` parking) | `examples/effects/m8x_4_recv_blocking.kai` + `m8x_4_block_sender.kai` |
| Phase 5 | ✅ landed (Link only) | Link runtime registry: KaiMailbox.owner_fiber, KaiFiber.linked_head, EvLink + default handler, trampoline propagation | `stage2/tests/link_runtime_test.c` (C-level) |
| Phase 6 | ✅ landed (v1) | Region-brand shallow check: `is_fiber_producer_helper` allow-list + `ty_expr_contains_fiber` recursive walk | `examples/effects/m8x_6_fiber_in_result.kai` |

**Phase 5 v1 caveats**: ships LINK only. Monitor stays a type-only
declaration — its kaikai-level demo needs `spawn_actor` (m8.x #6) or
custom message types carrying Pid, neither of which are in v1. Trap-
exit semantics (BEAM "crash propagates, normal exit doesn't")
collapsed in v1 to "any termination propagates"; the distinction is
queued for post-MVP.

**Phase 6 v1 caveats**: the shallow check in
`is_fiber_producer_helper` + `ty_expr_contains_fiber` rejects
`Fiber[T]` in any user-fn return-type position the walker reaches
(direct, `[Fiber[_]]`, function-type return, `Result[_, Fiber[_]]`,
etc.). It does NOT track:

- Branding through let-bindings, pattern matches, list literals, or
  record fields whose types don't lexically mention `Fiber` (e.g.
  `type Boxed = Wrapper(Fiber[Int])` — the return type `Boxed`
  hides Fiber from the walker).
- `Pid[Msg]` escape (the same brand machinery would apply per
  `docs/structured-concurrency.md` §*Type system*; v1 only checks
  Fiber).

The full `TyBranded(Ty, BrandId)` machinery — extending the
inferencer to mint brands at handler-installation sites and tracking
them through every binding form — is queued for post-Production-
honest 1.0. See `docs/fibers-honesty-targets.md` §*Residual m8.x
items* for the inventory.

## Out of scope for v1

- **Pre-emptive scheduling**. No timer-driven yields. A CPU-bound
  fiber with no effect ops blocks the program until it yields
  voluntarily (`Spawn.yield()` or any effect op). Documented in
  `structured-concurrency.md` §*Non-goals*.
- **Multi-thread parallelism**. One OS thread, one scheduler. Doc
  C §*Out of scope for v1* item *Thread-level parallelism that is
  not fiber-based*.
- **Stack grow / shrink**. v1 fixed size; pick `KAI_FIBER_STACK_SIZE`
  per workload. (Guard-page overflow detection landed 2026-04-29 —
  see *Stack model* above.)
- **Priority queues**. Run queue is FIFO. Priority is a
  `structured-concurrency.md` §*Non-goals* item.
- **Fairness guarantees**. No anti-starvation. A fiber that yields
  immediately can be re-dispatched immediately if it is the only
  one ready.
- **Cross-thread fiber migration**. A fiber stays on the thread
  that spawned it. There is only one thread today, so this is
  trivially satisfied; if multi-thread lands, migration is its own
  design pass.

## References

- `docs/structured-concurrency.md` — surface and lifetime rules.
- `docs/actors.md` — mailbox + supervision surface.
- `docs/effects-impl.md` — Doc C: CPS, handler-stack runtime, the
  evidence vector that this scheduler threads through.
- `docs/fibers-honesty-targets.md` — scope decision for which
  followups gate which honesty claim, plus the §*Residual m8.x
  items* inventory of work left after the R2 lane closed.
- ucontext(3), setcontext(3), makecontext(3), swapcontext(3).
- libco, libtask, Boost.Context — prior art using the same
  POSIX substrate with the same macOS deprecation workaround.
