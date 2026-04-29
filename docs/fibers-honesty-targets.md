# Fibers — honesty targets

Decision pinned 2026-04-29 (post m4c Phase 1 + 2 full landed; PR #22 + #23
merged; v0.11.0). Three tiers of "fibers done", what each one means
concretely, and which items belong in which tier. Cost numbers are
real-day estimates with the runtime-engineering velocity calibration
(~5–10× faster than spec, not the 20–50× ratio that applied to
mechanical lanes).

This is a **scope decision**, not a roadmap. The roadmap proper lives
in `docs/m8x-followup.md`; this file says *which followups are
required for which honesty claim*. Nothing here promises a calendar.

## Where we are today (2026-04-29)

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

What does **not** work today:

- ~~`let _ = fiber_spawn(…)` (discard the Fiber value without
  awaiting) deadlocks~~. **FIXED 2026-04-29** (Fibers Tier 1 lane).
  Scheduler-side fiber RC discipline: the wrapper now carries one
  ref for the caller and one for the scheduler; the trampoline's
  DONE/CANCELLED tail drops the scheduler's ref. See R4 in
  `docs/known-regressions.md`.
- Stack overflow in a fiber body lands in unmapped memory with
  no diagnostic. Stack model has no guard page (declared in
  `docs/fibers-impl.md`).
- `Monitor` is type-surface-only — Phase 5.5+ deferred in
  `docs/m8x-followup.md` §5. Needs `spawn_actor` (Pid handoff
  primitive) which itself is deferred.
- `Fiber[T]` / `Pid[Msg]` region brand is shallow. A `Fiber`
  embedded inside a sum-type constructor's payload escapes the
  shallow check — full machinery (TyBranded propagation through
  every binding form) deferred.
- LLVM op-dispatch (`llvm_emit_op_dispatch`) does not carry the
  `in_dispatch_node` flag. The same bug #12 shape exists in the
  LLVM backend, hidden until someone hits a self-delegating
  handler under `--emit=llvm`.
- Trap-exit semantics collapsed to "any termination propagates"
  in v1. BEAM's `process_flag(trap_exit, true)` distinction
  (Crashed vs Normal exit) is post-MVP.
- Spawn API still uses pre-m7b #2 typing shape — per-op generics
  were not retrofitted when m7b #2 closed.

## Tier 1 — *Show HN honest* (~1 day)

The two bugs a curious visitor hits in five minutes:

| Item | Cost | Why |
|---|---:|---|
| ~~**R4 — fiber-discard deadlock**~~ ✅ shipped 2026-04-29 | ~0.5d | First thing anyone tries (`let _ = fiber_spawn(…)`); fixed via scheduler-side fiber RC discipline |
| **Stack guard pages** | ~0.5d | Stack overflow → diagnostic SIGSEGV with message instead of UB |

Closes the two bugs that *would*  show up in a 30-minute live demo on
unfamiliar code. Everything else (deferred items above) is debt the
visitor would never trigger in a casual browse.

## Tier 2 — *Production-honest 1.0* (~8–11 days)

The set that lets the project drop the "fibers are inline-eager
syntactic" caveat and claim "Tier 1 #2 (runtime-efficient) holds
without footnotes". Order of attack is mostly independent — they
can be parallelised across short lanes.

| Item | Cost | Why |
|---|---:|---|
| ~~R4 — fiber-discard deadlock~~ ✅ shipped 2026-04-29 | ~0.5d | (carry-over from Tier 1) |
| Stack guard pages | ~0.5d | (carry-over from Tier 1) |
| **Monitor + `spawn_actor`** | ~2–3d | Phase 5.5+ in `m8x-followup.md` §5; without it the BEAM-style supervision claim is type-surface-only |
| **Region-brand full machinery** | ~3–5d | `TyBranded` propagation through let / match / fn args; closes the sum-type-payload escape hatch from Phase 6 v1 shallow |
| **LLVM op-dispatch `in_dispatch_node`** | ~0.5–1d | Wave A follow-up; same bug #12 shape but in the LLVM backend |
| **Trap-exit semantics** | ~1d | Crashed vs Normal exit distinction in Link propagation; today uniform |
| **Per-op generics in Spawn API** | ~0.5d | m7b #2 cleanup; pinned in `docs/effects-impl.md` §m7b #2 |

After this set, `docs/effects.md`, `docs/structured-concurrency.md`,
`docs/actors.md`, and `docs/fibers-impl.md` claims are all
verifiable by running code, not by reading the typer.

## Tier 3 — *Production-grade real* (post-MVP)

Items that don't belong in 1.0. Listed for completeness so they
don't accidentally get pulled into a 1.0-scoped lane.

| Item | Cost | Why post-MVP |
|---|---:|---|
| **Optimised context switch** | ~5–7d | `ucontext` is deprecated on macOS and ~1–2 µs per switch; production needs asm-level (~50–100 ns) like `boost.context`. v1 numbers are good enough for correctness. |
| **Multi-threaded scheduler** | weeks | Work-stealing across N OS threads; cross-thread RC requires atomics; changes the memory model. Single-thread cooperative is correct and parallelisable later. |
| **Profiling + observability** | ~1w | Fiber lifetime traces, mailbox contention, richer deadlock detection. Tooling, not correctness. |

CLAUDE.md should keep "do not design against multi-threaded scheduler
now" pinned for the same reason it pins WASM and Windows: the cost of
designing for it pre-1.0 dwarfs the cost of doing it after the
single-threaded model is locked in.

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
- Not a list of all m8x-followup items — only the ones that
  affect honesty claims. Spec-compliance items that don't
  surface to user code (e.g. internal RC bookkeeping in the
  scheduler queue) live in `m8x-followup.md` proper.
- Not an excuse to defer Tier 1. Show HN honesty is one day of
  work; not paying it means the next external reader gets a bad
  impression on minute 5.
