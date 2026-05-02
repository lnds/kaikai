# m8.x state audit — verdict and evidence

Lane: `m8x-state-audit` (read-only investigation, 2026-05-02).
Authored against `main` HEAD `24f745a`.

## TL;DR

**(A) m8.x ALREADY landed.** All five runtime phases (Phase 2 scheduler core,
Phase 3 Cancel-at-yield, Phase 4 blocking mailbox primitives, Phase 5 Link
runtime + Tier 2 Monitor + trap-exit, Phase 6 region-brand v1) are present and
wired in `stage0/runtime.h` on `main`, with end-to-end fixtures running under
`stage2/Makefile` and a multi-actor demo (`demos/ping_pong/`) producing
deterministic round-robin output. The disclaimers in `stdlib/actor.kai` and
`stdlib/spawn.kai` are **structural lies** — they describe the pre-`v0.4.0`
inline-eager runtime that no longer exists. PR #64 / issue #59's Wave 3
narrative ("the implementation lane is multi-week and remains open") is
inconsistent with the runtime as it currently stands.

Issue #59 should close as a documentation/disclaimer sweep plus one or two
acceptance fixtures (a real two-actor request/reply round trip and a
restatement of the Honesty-targets table) — *not* a multi-week implementation
lane.

## Runtime evidence

All citations below are at `stage0/runtime.h` unless otherwise marked.

### 1. ucontext-based fiber substrate — landed (Phase 2)

- `#define _XOPEN_SOURCE 600` and `#include <ucontext.h>` at lines
  `35` / `68`. The XSI-shape ucontext_t comment (lines `23-34`)
  explicitly references "m8.x cooperative scheduler substrate" and the
  Phase 2 silent-clobber bug it was added to fix.
- `ucontext_t ctx` field on `KaiFiber` at line `306`. Heap-mmaped
  private stack (`stack_base` line `307`, `stack_size` line `308`)
  with `PROT_NONE` guard page (`kai_fiber_init_ctx` lines
  `2913-2947` — `mmap(MAP_ANON)` + `mprotect(region, page, PROT_NONE)`).
- `getcontext` / `makecontext` at lines `2915` / `2946`;
  `swapcontext` at lines `2960` / `2989`; `setcontext` at line
  `3097`.
- Run queue is real and intrusive: `kai_ready_head` / `kai_ready_tail` /
  `kai_parked_count` at lines `419-421`. `kai_sched_enqueue` /
  `kai_sched_dequeue` at lines `2865-2882`.
- Fiber state machine has all five states (NEW / READY / RUNNING /
  PARKED / DONE / CANCELLED) at `KaiFiberState` lines `280-287`;
  comment at lines `273-279` calls out that "m8 v1 used only NEW/DONE
  (the inline-eager scheduler ran spawn synchronously); m8.x adds
  READY, RUNNING, PARKED."
- `kai_default_spawn_spawn` (line `3129`) does **not** run the thunk
  synchronously — it calls `kai_fiber_init_ctx(f)` (line `3146`) and
  `kai_sched_enqueue(f)` (line `3158`); the thunk runs only when the
  dispatcher swaps in via `setcontext` (trampoline tail line `3097`)
  or `swapcontext` (yield/park lines `2960` / `2989`).
- `kai_default_spawn_yield` (line `3123`) calls `kai_sched_yield()`
  (real swap, line `2952`). The "yield is a no-op" framing in
  `stdlib/spawn.kai` line 50 is stale.

### 2. `kai_mailbox_pop` parks the caller — landed (Phase 4)

`kai_mailbox_pop` at lines `672-695`:

```c
static KaiValue *kai_mailbox_pop(KaiMailbox *mb) {
    while (!mb->head) {
        kai_mailbox_waiter_enqueue(&mb->recv_waiter_head,
                                    &mb->recv_waiter_tail,
                                    kai_current_fiber());
        kai_sched_park();
    }
    ...
}
```

No `exit(1)`, no `fprintf(stderr, ...)` panic. The empty-mailbox path
parks the caller on `recv_waiter_head` and yields to the scheduler.
The wake side runs from `kai_mailbox_push` (lines `666-669`):
`kai_mailbox_waiter_dequeue` of one parked receiver, then
`kai_sched_unpark`.

The contradicting prose at `stage0/runtime.h:474` ("v1 is unbounded —
every send succeeds, every receive on an empty mailbox is a runtime
error (the inline-eager scheduler can't suspend the caller until a
message arrives)") is itself stale comment text inside the same file
whose code already does the opposite. It's a leftover paragraph.

### 3. `Bounded(_, BlockSender)` parks senders — landed (Phase 4)

`kai_mailbox_alloc_bounded` (lines `597-607`) accepts
`KAI_OVERFLOW_BLOCK_SENDER = 3` (line `495`) without erroring; the
prose comment at line `601` explicitly says "Phase 4: BlockSender now
supported via the per-mailbox sender waiter queue + cooperative
parking on full push."

`kai_mailbox_push` (lines `630-670`) implements the full overflow
policy switch:

```c
} else if (mb->overflow == KAI_OVERFLOW_BLOCK_SENDER) {
    while (mb->len >= mb->cap) {
        kai_mailbox_waiter_enqueue(&mb->send_waiter_head,
                                    &mb->send_waiter_tail,
                                    kai_current_fiber());
        kai_sched_park();
    }
}
```

(lines `644-655`). Wake-side: `kai_mailbox_pop` dequeues one parked
sender after the pop (lines `691-693`).

The disclaimer at `stage0/runtime.h:486-491` ("v1 ships 0/1/2;
BlockSender (3) errors at allocation because the inline-eager
scheduler can't suspend the sender on a full mailbox — that lifts
together with the m8.x cooperative scheduler") is stale paragraph
text that contradicts the code two function bodies later.

### 4. Cancel injection at every op-dispatch lookup — landed (Phase 3)

`kai_check_cancel_yield_point` (lines `3552-3559`):

```c
static void kai_check_cancel_yield_point(void) {
    KaiFiber *f = kai_current_fiber();
    if (f->cancel_requested && !f->cancel_delivered && f->cancel_pad_set) {
        f->cancel_delivered = 1;
        longjmp(f->cancel_pad, 1);
    }
}
```

is called at the top of every effect-op resolution path:

- `kai_evidence_lookup` line `3562`
- `kai_evidence_lookup_node` line `3578`
- `kai_evidence_lookup_node_by_id` line `3602`

The pad is set up in `kai_fiber_trampoline` (line `3027`) via
`setjmp(self->cancel_pad)`; the second-return path lands the fiber in
`KAI_FIBER_CANCELLED` (line `3036`) and proceeds with awaiter / link /
monitor walks.

`kai_default_cancel_raise` (lines `3269-3280`) handles unhandled
`Cancel.raise()` invocations by `longjmp`-ing into the same pad;
`main_fiber` (no pad) falls back to `exit(0)`.

### 5. Link + Monitor + trap-exit registry — landed (Phase 5 + Tier 2)

- `KaiLinkNode` (lines `535-538`), `KaiMonitorNode` (lines `550-554`),
  `kai_link_add_bidirectional` (line `3304`),
  `kai_link_propagate_terminate` (line `3326`),
  `kai_monitor_add` / `kai_monitor_remove` /
  `kai_monitor_propagate_terminate` (lines `3377-3447`).
- Trap-exit handler `kai_default_spawn_set_trap_exit` (lines
  `3252-3258`) toggles `f->trap_exit`; the link propagation walker
  reads `peer->trap_exit` at lines `3333-3344` and pushes
  `"Normal"` / `"Crashed"` strings into the peer's mailbox instead
  of setting `cancel_requested`.

### 6. Region-brand v1 — landed (shallow, Tier 2 symmetric)

Type-level work in `stage2/compiler.kai` (out of scope for the
runtime audit but present per `docs/m8x-followup.md` §6 and
`docs/fibers-honesty-targets.md`). Fixtures
`examples/effects/m8x_6_fiber_in_result.kai` and
`examples/effects/m8x_6_pid_escapes.kai` exist and are wired into
`stage2/Makefile` (lines `1275` and `1289`). Full TyBranded
machinery is the only deferred item.

## Disclaimer audit

Every stale line below describes the pre-`v0.4.0` runtime. None of
the comments below match the code on `main` today.

### `stdlib/actor.kai`

| Line(s) | Stale claim | Proposed replacement |
|---|---|---|
| `4-11` | "v1's mailbox is unbounded — bounded policies require either the m8.x scheduler (BlockSender needs to suspend the sender) or a runtime side-channel (DropOldest / DropNewest can ship earlier)." | "Mailbox policies are fully wired: `Unbounded`, `Bounded(c, DropOldest)`, `Bounded(c, DropNewest)`, and `Bounded(c, BlockSender)` all reach the runtime. BlockSender parks the sender on the per-mailbox waiter chain via the m8.x cooperative scheduler (Phase 4, landed v0.4.0)." |
| `13-16` | "`receive()` on an empty mailbox is a runtime error in v1 — the inline-eager scheduler (m8 #3) cannot suspend the caller until a message arrives. Demos sequence the sends before any receive (e.g., self-send patterns) to stay within the synchronous frame." | "`receive()` on an empty mailbox parks the caller on the per-mailbox `recv_waiter` chain and yields to the scheduler. A subsequent `send` from any fiber wakes the head waiter (FIFO). The two-actor request/reply pattern works end-to-end (`examples/effects/m8x_4_recv_blocking.kai`, `demos/ping_pong/`)." |
| `22-26` | "BlockSender requires the m8.x cooperative scheduler to actually suspend the sender (without it, the runtime errors at allocation)." | "BlockSender is supported: `mailbox_alloc_bounded(c, 3)` returns a normal mailbox; `send` on full parks the caller on `send_waiter`; the next `receive` wakes one parked sender." |

### `stdlib/spawn.kai`

| Line(s) | Stale claim | Proposed replacement |
|---|---|---|
| `19-25` | "Runtime: m8 #3 ships an inline-eager scheduler — `spawn(f)` runs `f` synchronously and stores the result in an opaque KaiFiber; `await(fib)` returns the cached result; `yield()` is a no-op; `select(fs)` picks the head; `cancel(f)` sets a flag (delivery arrives in m8 #4). True mid-fiber suspension (ucontext or full CPS reification) is a future m8.x — the wrappers' polymorphic signature is the final shape, so the migration is runtime-only." | "Runtime: the m8.x cooperative scheduler (ucontext, landed v0.4.0) backs every Spawn op. `spawn(f)` allocates a private mmap stack with a `PROT_NONE` guard page and enqueues; the thunk runs when the dispatcher swaps in. `await(fib)` parks the caller on the target's awaiter chain. `yield()` rotates the run queue. `cancel(f)` sets the flag and Phase 3 delivers `Cancel.raise()` at the target's next effect-op call site." |
| `36-38` | "Wait for any of the listed fibers; v1 picks the head deterministically (every fiber is already DONE under the inline-eager scheduler)." | "Wait for any of the listed fibers. Phase 2 v1 simplification: if no fiber is already DONE, park on the head — real race + cancel-losers semantics is queued (`docs/m8x-followup.md` §1)." |
| `47-50` (`fiber_yield`) | "Under the v1 inline-eager scheduler it is a no-op." | "Rotates the run queue: the caller goes back on the tail and the head ready fiber resumes via `swapcontext`. No-op only when the caller is the lone ready fiber." |
| `64-79` (`nursery`) | "Cancel-on-fail (per `docs/structured-concurrency.md`: a child crash cancels live siblings, then the nursery re-raises the cause) is observable only when children can outlive the spawn call site — which they can't under the inline-eager scheduler. The semantics is in scope for m8.x; the API shape here matches it so user code does not change." | "Children outlive the spawn call site under the cooperative scheduler. The body of `nursery` is currently still a typed pass-through — the structured cancel-on-fail-and-rethrow semantics from `docs/structured-concurrency.md` is its own follow-up lane (it needs the nursery handler to wrap `Spawn` and observe child terminations through Link)." |

### `stage0/runtime.h` (in-file paragraphs that contradict adjacent code)

These are leftover comments inside the runtime itself — same code review.

| Line(s) | Stale claim | Note |
|---|---|---|
| `474` (above `KaiMboxNode`) | "every receive on an empty mailbox is a runtime error (the inline-eager scheduler can't suspend the caller until a message arrives)." | `kai_mailbox_pop` 200 lines later parks instead of erroring. |
| `486-491` (above `KAI_OVERFLOW_*`) | "v1 ships 0/1/2; BlockSender (3) errors at allocation because the inline-eager scheduler can't suspend the sender on a full mailbox — that lifts together with the m8.x cooperative scheduler." | `kai_mailbox_alloc_bounded` 110 lines later accepts policy 3 without erroring; `kai_mailbox_push` parks the sender. |

## Fixture audit

End-to-end coverage of cooperative semantics today. All paths are
exercised by `make -C stage2 test-effects` and `make -C demos
verify`.

| Fixture | Proves |
|---|---|
| `examples/effects/m8x_2_yield_interleave.kai` | Two fibers interleave through `fiber_yield()` (`A0 B0 A1 B1 A2 B2`). Phase 2 demo gate. |
| `examples/effects/m8x_3_cancel_at_yield.kai` | Cancel delivered at the target's next effect-op lookup; the third `Stdout.print` does not run. Phase 3 demo gate. |
| `examples/effects/m8x_4_recv_blocking.kai` | `Actor.receive()` on empty mailbox parks main; spawned worker's `Actor.send` wakes it. Two-actor cooperative receive. |
| `examples/effects/m8x_4_block_sender.kai` | `Bounded(1, BlockSender)` parks the sender on full; consumer's `receive` wakes one parked sender per pop. |
| `examples/effects/m8_monitor.kai` | `Monitor.monitor(worker_pid)` + `spawn_actor` end-to-end: worker `Cancel.raise()`s, supervisor receives the worker's pid in its mailbox without being cancelled. |
| `examples/effects/m8_trap_exit.kai` | `Spawn.set_trap_exit(true)` redirects link propagation: peer's DONE pushes `"Normal"`, CANCELLED pushes `"Crashed"`. |
| `examples/effects/m8_spawn_per_op_generics.kai` | Per-op TYPE generics on `spawn`/`await`/`select`/`cancel` (Tier 2 retrofit). |
| `examples/effects/m8_fiber_stack_overflow.kai` | Guard page + sigaltstack-handled SIGSEGV diagnostic. |
| `examples/effects/m8x_6_fiber_in_result.kai` / `m8x_6_pid_escapes.kai` | Region-brand shallow check (negative tests). |
| `demos/ping_pong/main.kai` | Three producers + main consumer: nine messages drained in deterministic round-robin order (`A:1 B:1 C:1 A:2 B:2 C:2 A:3 B:3 C:3`). Mixes `fiber_await` (producers A and B) with `let _ = fiber_spawn(…)` discard (producer C — the R4 path). Counted in the `demos/baseline.txt = 24` no-regression gate. |

The fixture brief from issue #59 ("two-actor ping-pong fixture") has
its closest match in `m8x_4_recv_blocking.kai` (one-way blocking
receive with a sender fiber) and `demos/ping_pong/` (one consumer +
three producers, observable round-robin). What is **not** in the
fixture set today: a true two-actor request/reply round trip where
fiber A sends to fiber B's mailbox, B receives, replies into A's
mailbox, A receives the reply. That fixture would be a small (~30
LOC) net addition and is the natural acceptance criterion for the
issue #59 close PR — see plan below.

## Plan to close issue #59

Verdict (A) implies a doc/disclaimer sweep plus one fixture, **not** a
multi-week implementation lane. Suggested checklist for a single PR:

1. **Disclaimer sweep — `stdlib/actor.kai`**.
   Apply the three replacements in the table above (header
   paragraphs at lines `4-11`, `13-16`, `22-26`).
2. **Disclaimer sweep — `stdlib/spawn.kai`**.
   Apply the four replacements in the table above (lines `19-25`,
   `36-38`, `47-50`, `64-79`). Note that the `nursery` body
   itself is genuinely still a pass-through — the rewrite should
   be honest about that.
3. **Disclaimer sweep — `stage0/runtime.h`**.
   Rewrite the two stale paragraphs at `474` and `486-491` so the
   header text matches the function bodies that follow it. Same
   PR even though it is the runtime header — the lines are pure
   documentation, no semantic change.
4. **Add two-actor request/reply fixture**.
   `examples/effects/m8x_4_request_reply.kai`: fiber A sends a
   request into B's mailbox, B receives, replies into A's mailbox,
   A receives. Wire it into `stage2/Makefile` next to
   `m8x_4_recv_blocking`. Approx. 30 LOC. This is the explicit
   "two-actor ping-pong" the issue brief asks for.
5. **Refresh `docs/roadmap.md` Wave 3 entry**.
   The current Wave 3 framing ("the implementation lane is
   multi-week and remains open") is inconsistent with the
   honesty-targets file (`docs/fibers-honesty-targets.md`
   already declares Tier 1 + Tier 2 closed). Reclassify Wave 3
   acceptance to:
     - Definition of Done items 1-5 already closed at v0.4.0
       (per `docs/m8x-followup.md` §Status — that file is correct).
     - Item 6 (full TyBranded region brand) and the per-op ROW
       generics extension of item 7 are the residual scope.
     - The "actor-surface gap" lnds/ahu#1 cited in CHANGELOG
       0.30.1 as the upstream dependency does not exist as a
       runtime gap — only as a documentation gap (this PR
       closes it).
6. **Update `CHANGELOG.md` `[Unreleased]`**.
   Add a `### Documented` (or `### Changed` if you prefer the
   existing taxonomy) entry: "stdlib/actor.kai + stdlib/spawn.kai
   disclaimer sweep — bring the inline-eager-era prose in line
   with the m8.x runtime that landed v0.4.0. Adds
   `examples/effects/m8x_4_request_reply.kai` for explicit two-actor
   round-trip coverage."
7. **Verify** `make tier1` (and `tier1-asan` if any of the runtime
   header paragraphs change byte counts that matter to grep
   tooling, which they don't, so probably skip). The change is
   prose-only except for one new fixture; expected to be byte-
   identical-to-stage1 at the compiler level.

The full lane fits in ~100-200 LOC of edits + ~30 LOC of fixture +
expected output. Single PR. Acceptance gate is `make tier0 && make
tier1` plus the new fixture passing.

## Disproof of the (C) hypothesis

The "phases on a branch but not on `main`" hypothesis (C) is
disproved by:

- `git log --oneline 5de03d2 c8aa1a7 4cb56c9 55e6152 97f3f28
  6b18d93 9c49a9f` returns the seven m8.x commits.
- Every line citation above is from the working tree's
  `stage0/runtime.h`, `stdlib/actor.kai`, `stdlib/spawn.kai`,
  `examples/effects/*.kai`, `demos/ping_pong/*.kai` — i.e., the
  files at `main` HEAD `24f745a`.
- `docs/fibers-honesty-targets.md:14-25` independently asserts the
  Tier 1 + Tier 2 close at v0.4.0 / 2026-04-29 / 2026-04-30 with
  the same commit-bisect history.
- `demos/baseline.txt = 24` includes `demos/ping_pong/`, which
  means every `make demos-no-regression` run since the demo was
  added has confirmed the cooperative round-robin output is
  reproducible.

The (B) "partially landed" hypothesis is also disproved: every
Phase 2-5 acceptance fixture exists and is wired into a Makefile
target. The only items with explicit "partial" status in
`m8x-followup.md` are item 6 (region brand) and item 7 (per-op ROW
generics) — neither of which contributes to the actor-surface
disclaimers in stdlib.
