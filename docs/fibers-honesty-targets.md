# Fibers — honesty targets

Decision pinned 2026-04-29 (post m4c Phase 1 + 2 full landed; PR #22 + #23
merged; v0.11.0). Three tiers of "fibers done", what each one means
concretely, and which items belong in which tier. Cost numbers are
real-day estimates with the runtime-engineering velocity calibration
(~5–10× faster than spec, not the 20–50× ratio that applied to
mechanical lanes).

This is a **scope decision**, not a roadmap: it says *which
followups are required for which honesty claim*. Nothing here
promises a calendar. The §*Residual m8.x items* section below
inventories what is left after the R2 lane closed (`0.4.0`) and the
2026-04-30 Tier 2 retrofit landed.

## Where we are today (2026-05-16)

> **Last verified 2026-05-20.** The R1+R2+R3 reactor wave landed
> 2026-05-15→2026-05-16 (#611, #620, #630); the LLVM
> default-handler trio (Spawn, Cancel, Link/Monitor — #570, #582,
> #587) closed during the bug-bash week 2026-05-14. **R4 reactor
> (signal trap fiber-aware) shipped 2026-05-20 (issue #671)** —
> `Signal.await()` now parks the fiber on the reactor's signal
> self-pipe via an async-signal-safe `sa_handler`; concurrent
> fibers progress while it sits there. The Tier 2 bullets below
> were spot-checked against runtime symbols, fixtures, and closing
> PR numbers — all green.

Tier 1 + Tier 2 closed. The runtime is BEAM-style cooperative
single-threaded; everything that surfaced in a 30-minute live demo
or in the 1.0-honesty target list is shipped and verifiable.

- R2 cooperative scheduler landed v0.4.0 (`ucontext`, READY/RUNNING/
  PARKED/DONE state machine, intrusive ready queue, blocking
  mailbox with waiter chains, Cancel cooperative at yield points,
  Link runtime registry).
- m8 bug #12 fixed via per-fiber `in_dispatch_node` (commit
  `3553e9f`). Self-delegating handlers no longer SIGSEGV; m8x_2
  yield interleave runs to completion.
- `demos/ping_pong/` produces observable round-robin output —
  the cooperative interleave is publicly verifiable, not just a
  type-system claim.
- Tier 1 quick wins shipped 2026-04-29: fiber-discard deadlock
  (R4) + stack guard pages.
- Tier 2 production-honest set shipped 2026-04-29 — 2026-05-02:
  Monitor + `spawn_actor`, region-brand region walker (Fiber +
  Pid + sum-type-payload + cap-binding nursery), LLVM
  op-dispatch parity, trap-exit semantics, per-op generics
  including ROW generics on spawned thunks (issue #72).
- **R1 reactor shipped 2026-05-15 (issue #611)** — `Spawn.sleep`,
  `File.read_file` / `write_file`, and `Process.wait` park the
  *fiber*, not the OS thread. Wait primitive is `poll()` on two
  self-pipes (SIGCHLD + a 4-worker file pool) plus a sorted timer
  wheel keyed on `CLOCK_MONOTONIC`. 10 concurrent fibers each
  sleeping 100 ms finish in ~100 ms total (`demos/sleep_concurrent`),
  3 concurrent `Process.wait`s on a 200 ms child sleep finish in
  ~200 ms (`demos/process_wait_concurrent`), and 4 file writes
  share the disk in parallel (`demos/file_concurrent`).
- **R3 reactor shipped 2026-05-15 (issue #620)** — `Stdin.read_line`
  and `Stdin.read_bytes` park the *fiber* on a singleton
  `kai_reactor_stdin_waiter` slot. Fd 0 is set to `O_NONBLOCK`
  once per process with an `atexit`-restored cleanup; the
  scheduler's `poll()` set grows a third slot for `STDIN_FILENO`
  whenever a waiter is registered, and POLLIN / POLLHUP / POLLERR
  promote the parked fiber. `demos/stdin_concurrent` shows the
  compute fiber running while the reader is parked on a slow pipe
  (`(echo a; sleep 0.5; echo b) | demo`); the same demo runs
  cleanly under the regression harness with stdin closed because
  the empty-pipe case maps to `None` on the first iteration.
  Multiple concurrent stdin readers panic with a clear diagnostic
  (`stdin: multiple fibers reading concurrently is undefined;
  serialize via an actor`).
- **R2 reactor shipped 2026-05-16 (issue #630)** — `NetTcp.connect`,
  `NetTcp.accept`, `NetTcp.send`, and `NetTcp.recv` park the *fiber*
  on socket read/write readiness. Two intrusive waiter lists
  (`kai_reactor_socket_read_waiters` + `..._write_waiters`) extend
  the existing `poll()` set with every live socket fd. Each fd is
  flipped to `O_NONBLOCK` at creation time; `send` loops internally
  on partial writes; `connect` handles `EINPROGRESS` and checks
  `SO_ERROR` post-wake. `demos/tcp_concurrent` runs three concurrent
  client/server handshakes that pre-R2 would have deadlocked under
  the OS-thread-blocking accept. `Cancel` stays cooperative — a
  fiber parked in a socket op is interrupted at the park boundary,
  not mid-syscall (the mid-syscall redesign with `close(fd)` +
  `pthread_kill` is Orongo territory).
- **R4 reactor shipped 2026-05-20 (issue #671)** — `Signal.await()`
  parks the *fiber* on a singleton `kai_reactor_signal_waiter`
  slot. The implementation replaces the v1 sigwait body with an
  async-signal-safe `sa_handler` that writes `signo` to a self-pipe
  (`kai_reactor_signal_pipe`), drained by the existing `poll()` set
  on the next loop iteration. The signo is parked in the waiter's
  `reactor_wait_status` and the await handler builds the matching
  `Sig` variant on resume (variant construction is not
  async-safe). Two concurrent `Signal.await()` calls panic with a
  clear diagnostic, mirroring the R3 stdin contract. Coverage:
  `examples/effects/m8x_signal_await_parks.kai` (compute fiber
  interleaves with await) and `demos/signal_concurrent`
  (Spawn.cancel reaches the parked fiber). This unblocks
  `lnds/ahu`'s `run_app(root)` spawn+multiplex+cancel pattern and
  the todo-demo (#606) graceful-shutdown story.
- §*Residual m8.x items* below now reduces to two minor
  items, none of which surface as runtime errors today
  (structured auto-join + cancel-on-fail shipped — see that
  section's item 3).

## Tier 1 — *Show HN honest* (~1 day)

The two bugs a curious visitor hits in five minutes:

| Item | Cost | Why |
|---|---:|---|
| ~~**R4 — fiber-discard deadlock**~~ ✅ shipped 2026-04-29 | ~0.5d | First thing anyone tries (`let _ = fiber_spawn(…)`); fixed via scheduler-side fiber RC discipline |
| ~~**Stack guard pages**~~ ✅ shipped 2026-04-29 | ~0.5d | Overflow now hits a `PROT_NONE` page; handler prints `kai: fiber stack overflow at <ptr>` and re-raises (`m8_fiber_stack_overflow.kai`) |

**Tier 1 closed 2026-04-29.** The two bugs that *would*  show up in a
30-minute live demo on unfamiliar code are both addressed. Everything
else (deferred items above) is debt the visitor would never trigger in
a casual browse.

## Tier 2 — *Production-honest 1.0* (~8–11 days)

The set that lets the project drop the "fibers are inline-eager
syntactic" caveat and claim "Tier 1 #2 (runtime-efficient) holds
without footnotes". Order of attack is mostly independent — they
can be parallelised across short lanes.

| Item | Cost | Why |
|---|---:|---|
| ~~R4 — fiber-discard deadlock~~ ✅ shipped 2026-04-29 | ~0.5d | (carry-over from Tier 1) |
| ~~Stack guard pages~~ ✅ shipped 2026-04-29 | ~0.5d | (carry-over from Tier 1) |
| ~~**Monitor + `spawn_actor`**~~ ✅ shipped 2026-04-30 | ~2–3d | Phase 5.5+ retrofitted; runtime walker mirrors Link + trap-exit, MonitorRef simplified to Pid[Nothing] in v1 |
| **Region-brand full machinery** ⚠ partial 2026-05-02 | ~3–5d | Pid symmetric with Fiber in the shallow check (2026-04-30); sum-type-payload escape closed via `SumInfo` registry + `ty_expr_find_handle` walker (2026-05-02 commit `c18cd4b`, issue #71 option (a)). Full `TyBranded(Ty, BrandId)` propagation with sibling-nursery brand mismatch is option (b), gated on m7b #4 cap-binding form not yet landing — see §*Residual m8.x items* |
| ~~**LLVM op-dispatch `in_dispatch_node`**~~ ✅ shipped 2026-04-30 | ~0.5–1d | Wave A follow-up; same bug #12 shape but in the LLVM backend — three runtime helpers (`kaix_evidence_lookup_node` / `kaix_in_dispatch_enter` / `kaix_in_dispatch_leave`) keep the KaiFiber struct out of the IR |
| ~~**Trap-exit semantics**~~ ✅ shipped 2026-04-29 | ~1d | `Spawn.set_trap_exit(Bool)` opts current fiber in; DONE → "Normal" / CANCELLED → "Crashed" pushed to mailbox instead of cancel_requested |
| ~~**Per-op generics in Spawn API**~~ ✅ shipped 2026-05-02 | ~0.5d | TYPE generics retrofitted 2026-04-30 on `spawn` / `await` / `select` / `cancel`; ROW generics on the spawned thunk closed in issue #72 — `Spawn.spawn[T, e](f: () -> T / e)` is the canonical entry point, wrappers in `stdlib/spawn.kai` reduced to one-line aliases |
| ~~**Reactor — Phase R1 (file + sleep + process)**~~ ✅ shipped 2026-05-15 | ~1d | Issue #611. Sorted timer wheel + SIGCHLD self-pipe + 4-worker file pool, woken via a single `poll()` on the scheduler thread. `Clock.sleep_ns`, `File.read_file` / `write_file`, `Process.wait` no longer freeze the scheduler; sockets (`NetTcp`) queue for R2 in Orongo |
| ~~**Reactor — Phase R3 (stdin)**~~ ✅ shipped 2026-05-15 | ~0.1d | Issue #620. Singleton `kai_reactor_stdin_waiter` slot reuses the existing `poll()` set; fd 0 is flipped to `O_NONBLOCK` once per process with an `atexit`-restored cleanup. `Stdin.read_line` and `Stdin.read_bytes` park on EAGAIN; multiple concurrent readers panic. |
| ~~**Reactor — Phase R2 (TCP sockets)**~~ ✅ shipped 2026-05-16 | ~0.2d | Issue #630. Per-direction socket waiter lists (`socket_read_waiters` + `socket_write_waiters`) join the existing `poll()` set; every socket fd is `O_NONBLOCK` from creation; `connect` handles `EINPROGRESS` + `SO_ERROR`, `send` loops on partial writes, `accept` / `recv` park on read-readiness. `demos/tcp_concurrent` proves three concurrent handshakes interleave. |
| ~~**Reactor — Phase R4 (Signal trap)**~~ ✅ shipped 2026-05-20 | ~0.3d | Issue #671. Singleton `kai_reactor_signal_waiter` + `kai_reactor_signal_pipe` self-pipe in the existing `poll()` set; an async-signal-safe `sa_handler` writes one signo byte, the drain helper maps it to the `Sig` variant on the waiter path (variant alloc is not async-safe in the handler). Closes `lnds/ahu` `run_app` blocker and the todo-demo graceful shutdown story (#606). Closes the reactor for Hanga Roa. |

After this set, `docs/effects.md`, `docs/structured-concurrency.md`,
`docs/actors.md`, and `docs/fibers-impl.md` claims are all
verifiable by running code, not by reading the typer.

## Tier 3 — *Production-grade real* (post-MVP)

Items that don't belong in 1.0. Listed for completeness so they
don't accidentally get pulled into a 1.0-scoped lane.

| Item | Cost | Why post-MVP |
|---|---:|---|
| **Optimised context switch** | ~5–7d | `ucontext` is deprecated on macOS and ~1–2 µs per switch; production needs asm-level (~50–100 ns) like `boost.context`. v1 numbers are good enough for correctness. |
| ~~**Multi-threaded scheduler**~~ ✅ **shipped** | — | M:N work-stealing landed (design `docs/mn-scheduler-design.md`): `KAI_THREADS=N` opt-in, default 1 byte-identical; cross-thread sends physically copied so per-fiber RC stays non-atomic (the "requires atomics" concern was resolved by copy-at-the-boundary, not atomics); TSAN CI tier guards races. Shipped: state partition + isolated reactor, then the M:N scheduler itself, then reactor serving N schedulers. Remaining: the deliberate default-on flip — an owner decision. |
| ~~**Work-stealing fiber-identity hazard**~~ ✅ **closed** | — | A parking fiber's post-resume `kai_active_fiber = current` resolved its thread-local slot address before the swapcontext and reused it after. A fiber parked on one thread and resumed on another (a steal) then stored through the *parking* thread's slot, rotating every thread's active pointer — which truncated the reactor timer wheel (hang) and misrouted effect dispatch (`Actor` unhandled). The `-O0` runtime owner did not neutralise it: on darwin the TLS slot is a `tlv_get_addr` result that `-O0` still spills across the swap. Fixed by writing `kai_active_fiber` from the out-of-line `kai_set_active_fiber`, re-resolved on the resuming thread. Regression fixture `examples/effects/mn_park_resume_steal.kai`, wired into the M:N determinism gate. |
| ~~**Trampoline-tail identity residue**~~ ✅ **closed** | — | The same hazard on the frame the fix above did not cover. `kai_fiber_trampoline`'s frame spans the fiber's whole body, so clang resolves the `kai_active_fiber` slot in the prologue and spills that address onto the fiber's own stack, where it survives every `swapcontext` the body performs. A fiber first dispatched on one thread and finishing on another then handed off through the *dispatching* thread's slot, leaving that thread's active pointer on a fiber it was not running — usually the successor the tail had just dispatched, whose evidence chain is still empty, so the next op there reported `Actor` unhandled. `kai_bootstrap_trampoline` held the same address in a callee-saved register across the whole of user `main`. Both now write through `kai_set_active_fiber`. Regression fixture `examples/effects/mn_trampoline_tail_steal.kai`, looped at `KAI_THREADS=8` inside `test-effects` — which builds the emitted C as a single TU at `-O2`, the path with no `-O0` owner split, and redirects stdout to a file (through a pipe the window closes and the bug is invisible). |
| ~~**Stranded sleeper on the ownerless `-O2` build**~~ ✅ **closed** | — | The read-side of the same hazard, and the last frame the two fixes above did not cover. `kai_nursery_join_child` and the two await sites read a bare `kai_active_fiber` to find themselves; inlined into a caller whose frame spans the join loop, clang resolved the thread-local slot once in the prologue and spilled the address onto the fiber's own stack, reusing it after every `kai_sched_park`. A parent stolen while joining then read the *parking* thread's slot, linked whatever fiber that thread was running onto the child's awaiter chain, and parked itself on no chain at all — nothing could ever wake it, so every worker idled in `kai_worker_loop` and the reactor slept in `kai_reactor_wait` with no output produced. Fixed by routing every park-spanning read through the out-of-line `kai_current_fiber()`, which the write path already used. Guarded by `examples/effects/mn_recv_timeout_wheel.kai` and `examples/effects/mn_fiber_free_race.kai` on the M:N gate's single-TU `-O2` arm. |
| **Profiling + observability** | ~1w | Fiber lifetime traces, mailbox contention, richer deadlock detection. Tooling, not correctness. |

The old guidance to "not design against a multi-threaded scheduler"
is retired: the scheduler exists. What stays pinned is its invariant —
no shared mutation crosses a thread boundary; the copy at the crossing
is what keeps object RC atomic-free.

## Residual m8.x items

After the R2 lane closed (`0.4.0`), the 2026-04-30 Tier 2 retrofit
landed, and issue #72 closed on 2026-05-02, one typer-side item from
the original m8.x scope remains open. It is independent of the
runtime and tracked by a GitHub issue. It does not block the
disclaimer sweep that closes issue #59 — it was always scoped as a
separate lane.

### 1. Full `TyBranded` region brand (issue #71)

**Option (a) closed 2026-05-02 (commit `c18cd4b`).** The walker now descends into
user-defined sum-type constructor payloads via a `[SumInfo]`
registry collected during `infer_program` and threaded through
`infer_all_loop` → `infer_decl` → `check_no_fiber_escape`. The
`ty_expr_find_handle` predicate substitutes the sum's tparams
into each variant's payload TypeExpr and recurses, with a
`visited` set guarding recursive sums (`type Tree[T] = Leaf(T) |
Node(Tree[T], Tree[T])`). When the breach goes through a sum
payload the diagnostic adds a note citing the constructor +
sum type. Coverage: `examples/effects/m8x_7_fiber_in_sum.kai`,
`m8x_7_pid_in_sum.kai`, `m8x_7_recursive_sum_no_breach.kai`. The
pre-existing `m8x_6_*` fixtures continue to fire via the
`HBDirect` path (no behavioural regression for direct/parametric
breaches).

**Option (b) closed 2026-05-02 (PR pending merge).** Both halves
of issue #71 are now closed:

- **Cap-binding nursery surface (m7b #4 prerequisite).** A
  pre-typecheck rewrite turns
  `nursery { n -> ... n.spawn(task) ... n.await(f) ... }` into
  the runtime-equivalent `nursery { ... Spawn.spawn(task) ...
  Spawn.await(f) ... }` the typer already accepts: the lambda's
  `n` parameter is stripped and every `n.<spawn|await|select|
  cancel|yield|set_trap_exit|cancel_all>(...)` inside the body
  is rewritten to the corresponding `Spawn.<op>` call. Each
  rewritten site is tagged with a fresh BrandId and recorded
  in a registry the brand-mismatch checker reads. Coverage:
  `examples/effects/m8x_8_nursery_cap.kai` (positive),
  `m8_5_nursery_join.kai` (migrated to the cap-binding form).

- **Brand-mismatch detection at consume sites.** A walker over
  the post-rewrite AST tracks brand attribution of bindings
  (binding name → brand id) and, at each rewritten
  `Spawn.await(arg)` / `Spawn.cancel(arg)` /
  `Spawn.select(args)`, cross-checks the arg's brand against
  the call's own brand. A mismatch (sibling-nursery breach) is
  rejected with a diagnostic that names both brand ids and
  their allocation sites (`nursery { n -> ... }` file:line:col)
  so the user can trace the breach. Coverage:
  `examples/effects/m8x_8_sibling_nursery_mismatch.kai`
  (negative — direct cross-nursery use),
  `m8x_8_brand_propagation_let_chain.kai` (negative —
  three-step let-chain propagates the brand). The
  `--dump-brands` flag (with `m8x_8_brand_dump.brands.expected`
  golden) lets fixtures pin the registry deterministically.

**Pragmatic implementation note.** The brief prescribed
`TyBranded(Ty, BrandId)` woven into the unifier so brands flow
through generic helper instantiations automatically. This lane
ships the user-visible behaviour (sibling-nursery mismatch
rejection, escape detection, `--dump-brands` diagnostic) via a
side-table walker keyed on call-site `(line, col)` rather than
threading a new `Ty` variant through ~70 pattern-match
touchpoints in the typer / mangler / emitter / Perceus passes.
The trade-off is brand propagation through generic helpers
(`fn id[T](x: T) : T = x`) where the round-trip strips the
brand — direct-binding propagation (`let p = q`) and the
spawn-site-to-consume-site loop are covered today; helper-
passthrough is the next refinement and is now the only piece
of option (b) outstanding. Documented in
`docs/structured-concurrency.md` §*Type system* and
§*Debugging brand inference*.

### 2. Other minor items left behind by the R2 lane

- Real race + cancel-losers semantics for `Spawn.select`. v1
  (Phase 2) returns the head deterministically; the spec's race
  semantics (lose-then-cancel-the-rest) is queued.
- User-installed `with Cancel { raise(_) -> cleanup }` handlers on
  runtime-triggered cancel. Phase 3 longjmps through the
  trampoline `cancel_pad` directly; user cleanup clauses are not
  invoked. Documented in the `kai_check_cancel_yield_point`
  comment.
- `Fail`-at-nursery-level recovery. A child that fails through the
  modelled `Fail` effect (rather than `Cancel`) is not yet caught
  and re-raised by the scope — that needs the nursery to reshape the
  body's inferred row, which is its own type-design lane.

These three are notes for future agents — none of them surface to
user code as runtime errors today, and none of them block the issue
#59 close.

### 3. Structured auto-join + cancel-on-fail in `nursery` (shipped)

`nursery` is no longer a typed pass-through. It opens a runtime scope
(`Spawn.scope_enter`); children spawned inside register on it, and at
the closing brace `Spawn.scope_exit` joins every child FIFO before the
nursery returns — no explicit `await` required. If a child terminates
CANCELLED without anyone requesting its cancellation (it raised
`Cancel` on its own), the surviving siblings are cancelled and the
failure re-raises out of the scope via the running fiber's cancel pad.
A child cancelled on request (`n.cancel`, or the cancel-on-fail walk
itself) terminates CANCELLED with `cancel_requested` set and is an
expected, non-propagating outcome. The scope state is a per-fiber
stack of `KaiNursery` so nested nurseries compose. Coverage:
`examples/effects/m8x_9_nursery_autojoin.kai` (auto-join order),
`m8x_10_nursery_cancel_on_fail.kai` (sibling cancel + re-raise).

## Sequencing recommendation

Right after m4c body subst (`r4-m4c-body-subst` lane in flight as
of this writing) closes:

1. **Tier 1 quick win** — R4 + stack guard pages, ~1 day, single
   commit each. Removes the only two embarrassing surfaces.
2. **Tier 2 runtime lane** — Monitor + spawn_actor + region-brand
   + LLVM in_dispatch + trap-exit + Spawn API cleanup. Can be
   one big lane or split into 2–3 smaller ones; the items are
   mostly independent. ~8–10 days.
3. **L3 tutorial** — only after Tier 2. The tutorial cannot
   honestly cover concurrency until Monitor and supervision work.

Tier 3 stays explicitly post-MVP. If a feature request for
multi-threaded scheduling comes in pre-1.0, the answer is "after
1.0".

## What this document is NOT

- Not a calendar. Cost estimates assume nothing else competes for
  attention; in practice runtime engineering frequently REOPEN-s
  on first try (m12.8 precedent), add 1–2d per lane.
- Not a complete inventory of every m8.x deferred item — only the
  ones that affect honesty claims and the typer-side residuals
  that survived after the runtime lane closed (see §*Residual
  m8.x items*). Spec-compliance items that don't surface to user
  code (e.g. internal RC bookkeeping in the scheduler queue) are
  not tracked here.
- Not an excuse to defer Tier 1. Show HN honesty is one day of
  work; not paying it means the next external reader gets a bad
  impression on minute 5.
