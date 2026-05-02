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
- ~~Stack overflow in a fiber body lands in unmapped memory with
  no diagnostic.~~ **FIXED 2026-04-29** (Fibers Tier 1 lane).
  Each spawned fiber's private stack is now `mmap`ed with a
  `PROT_NONE` guard page at the low end (stacks grow down on
  x86_64 / arm64); a SIGSEGV/SIGBUS handler running on a
  sigaltstack prints `kai: fiber stack overflow at <ptr>` before
  re-raising with the default disposition. Coverage:
  `examples/effects/m8_fiber_stack_overflow.kai`.
- ~~`Monitor` is type-surface-only — Phase 5.5+ deferred. Needs
  `spawn_actor` (Pid handoff primitive) which itself is
  deferred.~~ **Closed 2026-04-30**
  (Fibers Tier 2 lane). `spawn_actor` lifts on `fiber_spawn` +
  `with_mailbox` via two new runtime helpers
  (`mailbox_alloc_unowned` + `mailbox_assign_owner`) so the
  spawned fiber owns the mailbox; `Monitor.monitor(pid)`
  registers an entry on the *target* fiber's `monitor_head`
  chain and the trampoline's termination tail walks it,
  pushing the original target_pid into each observer's
  mailbox. v1 simplification: `MonitorRef` collapses to
  `Pid[Nothing]` and the `MonitorDown(ref, cause)` payload
  becomes a bare pid — reason distinction (Normal / Crashed)
  is reachable today through Link + trap_exit. Coverage:
  `examples/effects/m8_monitor.kai`.
- ~~`Fiber[T]` / `Pid[Msg]` region brand is shallow. The shallow
  walk now symmetrises Fiber and Pid (Tier 2 extension,
  2026-04-30 — `examples/effects/m8x_6_pid_escapes.kai`), but a
  `Fiber` / `Pid` embedded inside a *user-defined* sum-type
  constructor's payload still escapes the shallow check.~~
  **Sum-payload escape closed 2026-05-02 (issue #71 option (a)).**
  The walker now consults a `[SumInfo]` registry collected per
  `infer_program`, substitutes the type's tparams into each
  variant's payload TypeExpr and recurses. Coverage:
  `examples/effects/m8x_7_fiber_in_sum.kai`,
  `m8x_7_pid_in_sum.kai`, `m8x_7_recursive_sum_no_breach.kai`.
  The remaining gap is option (b) — full `TyBranded(Ty, BrandId)`
  propagation with sibling-nursery brand-mismatch detection —
  pinned in `docs/structured-concurrency.md` §*Type system*
  and gated on the m7b #4 cap-binding form (`nursery { n -> ... }`)
  not yet landing; see §*Residual m8.x items* item 1 below.
- ~~LLVM op-dispatch (`llvm_emit_op_dispatch`) does not carry the
  `in_dispatch_node` flag. The same bug #12 shape exists in the
  LLVM backend, hidden until someone hits a self-delegating
  handler under `--emit=llvm`.~~ **Closed 2026-04-30** (Fibers
  Tier 2 lane). `llvm_emit_op_dispatch` now mirrors the C emit's
  `_saved_disp = ...; in_dispatch_node = _node_op; <call>;
  in_dispatch_node = _saved_disp;` envelope via three new
  runtime helpers (`kaix_evidence_lookup_node`,
  `kaix_in_dispatch_enter`, `kaix_in_dispatch_leave`). The
  `m8_12_self_delegating_handler` fixture is now exercised under
  both backends, with a structural grep on the IR confirming the
  enter/leave pairing.
- ~~Trap-exit semantics collapsed to "any termination propagates"
  in v1. BEAM's `process_flag(trap_exit, true)` distinction
  (Crashed vs Normal exit) is post-MVP.~~ **FIXED 2026-04-29**
  (Fibers Tier 2 lane). `Spawn.set_trap_exit(true)` opts the
  current fiber into trap-exit mode; linked peers' DONE
  terminations push `"Normal"`, CANCELLED terminations push
  `"Crashed"` into the fiber's mailbox instead of setting
  `cancel_requested`. Coverage: `examples/effects/m8_trap_exit.kai`
  + `stage2/tests/link_runtime_test.c`.
- ~~Spawn API still uses pre-m7b #2 typing shape — per-op generics
  were not retrofitted when m7b #2 closed.~~ **Partial 2026-04-30**
  (Fibers Tier 2 lane). The four Fiber-shaped ops (`spawn`,
  `await`, `select`, `cancel`) now carry `[T]`, so `Spawn.await(f)`
  flows back as `T` instead of TyAny. Per-op ROW generics
  (`spawn[T, e](f: () -> T / e)`) is a separate extension still
  pending — see §*Residual m8.x items* below; the wrappers in
  `stdlib/spawn.kai` keep absorbing the thunk's open row via TyAny.
  Coverage: `examples/effects/m8_spawn_per_op_generics.kai`.

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
| ~~**Per-op generics in Spawn API**~~ ✅ partial 2026-04-30 | ~0.5d | TYPE generics retrofitted on `spawn` / `await` / `select` / `cancel`; ROW generics on the spawned thunk still pending — see §*Residual m8.x items* |

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

## Residual m8.x items

After the R2 lane closed (`0.4.0`) and the 2026-04-30 Tier 2 retrofit
landed, two items from the original m8.x scope remain open. Both are
typer-side, both are independent of the runtime, and both are tracked
by GitHub issues. Neither blocks the disclaimer sweep that closes
issue #59 — they were always scoped as separate lanes.

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

**Option (b) — full `TyBranded(Ty, BrandId)` with sibling-nursery
brand mismatch — still deferred, blocked on m7b #4 cap binding.**
The structural prerequisite for option (b) is a syntactic site
that introduces a brand: `nursery { n -> ... }`, where `n` is a
capability that tags every `n.spawn` result with the nursery's
brand. Today `nursery { ... }` is a plain helper
`nursery[T,e](body: () -> T / e) : T / e = body()` with no `n`
parameter — the cap-binding form belongs to m7b #4 which has not
landed (see `docs/syntax-sugars.md` §*Capability read/write*).
Without that surface, two distinct nurseries cannot even be
referenced from user code, so brand mismatch between sibling
nurseries is unwritable — there is nothing to detect. Option
(b)'s typer work (TyBranded variant, propagation through let /
match / list / record / fn args+returns, brand-mismatch
unification) is well-scoped, but landing it before m7b #4 cap
binding lands would have no observable effect on user code; it
would be defensive scaffolding for a surface that doesn't exist.
The right sequencing is m7b #4 first, then option (b) — which
re-uses the cap-binding scope as the brand introduction site.

Until m7b #4 lands and option (b) is built on top of it, option
(a) plus the existing direct/parametric shallow check covers
every gap that user code can express today.

### 2. Per-op ROW generics on Spawn (issue #72)

The four Fiber-shaped Spawn ops carry per-op `[T]` (TYPE generics)
since Tier 2 (`m8_spawn_per_op_generics.kai`). The full Doc B shape

```
effect Spawn {
  spawn[T, e](f: () -> T / e) : Fiber[T]
  ...
}
```

— with `e` a per-op *row* variable propagating the spawned thunk's
row into the caller's row — has not landed.
`add_effect_op_sigs_loop` only allocates `mk_tpbinds_from` (TYPE
binds) for `op_tparams`; there is no op-level `mk_rvbinds`. Until
that lands, the spawn op keeps `thunk: Nothing` (TyAny) so the
wrappers in `stdlib/spawn.kai`
(`fiber_spawn[T, e](f: () -> T / e) = Spawn.spawn(f)`) can absorb
the open row through TyAny. After per-op row generics land, the
wrappers reduce to one-line aliases (or are removed) and
`Spawn.spawn[T, e]` becomes the canonical entry point. Doc C
§*Per-op type generics* §*Implementation plan (m7b)* has the work
plan; the additional row-bind plumbing is the small step that
remains.

### 3. Other minor items left behind by the R2 lane

- Real race + cancel-losers semantics for `Spawn.select`. v1
  (Phase 2) returns the head deterministically; the spec's race
  semantics (lose-then-cancel-the-rest) is queued.
- User-installed `with Cancel { raise(_) -> cleanup }` handlers on
  runtime-triggered cancel. Phase 3 longjmps through the
  trampoline `cancel_pad` directly; user cleanup clauses are not
  invoked. Documented in the `kai_check_cancel_yield_point`
  comment.
- Structured cancel-on-fail in `nursery`. The `nursery` body in
  `stdlib/spawn.kai` is currently a typed pass-through; the full
  cancel-failed-siblings-and-rethrow shape from
  `docs/structured-concurrency.md` requires the nursery to install
  its own `Spawn` handler that observes child terminations through
  Link.

These three are notes for future agents — none of them surface to
user code as runtime errors today, and none of them block the issue
#59 close.

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
