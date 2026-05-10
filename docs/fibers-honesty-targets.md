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

## Where we are today (2026-05-09)

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
- §*Residual m8.x items* below now reduces to three minor
  items, none of which surface as runtime errors today.

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
