# docs/formal/

Formal models used to **diagnose** compiler bugs. Scaffolding, not
infrastructure.

## Never a gate, never CI

These models are not built, not run by CI, not wired into any tier, and
not a bootstrap dependency. Lean is ~500 MB of toolchain; stage 0 takes
no dependencies by principle and this does not change that. Nothing in
the build system references this directory, and nothing should.

No lane is obliged to update a model here. A lane that changes a pass
these models describe may leave them untouched — they are dated against
the commit they were written for, and that is the whole contract.

They live under `docs/` and not in `tools/` deliberately. `tools/`
is product: its contents ship to the public mirror and some of it is
the `kai` binary itself. These are bitácora — a record of how a bug was
diagnosed, kept with the other internal documentation, and filtered out
of the mirror alongside it. Being doc-shaped also means a commit that
touches only this directory skips the tiers, which is correct for files
no gate reads.

## Why not a gate

The expense of a formal model is not writing it; it is keeping it
synchronised with a compiler that moves. A model in CI acquires that
obligation permanently, in exchange for a signal that only matters while
someone is hunting a bug. And a model that has fallen behind is worse
than none: it passes, and asserts with authority, and is wrong.

Treating them as disposable removes the drift problem rather than
managing it. A model is written against the code of the day, answers the
question of the day, and promises nothing after that.

## What persists instead

If a model finds something, the artefact that enters the repo is the
**regression fixture** (`examples/perceus/`, `examples/effects/`, …)
plus the account in the PR body. The `.lean` file is scaffolding: it may
be committed for the record, but it is not the record.

This keeps one source of truth about what a pass does — the compiler and
its fixtures — rather than two that can disagree.

## Reading a model here

Every model states, at the top, the commit it was written against. If
that commit is far behind `main`, assume the model describes history and
check it against the code before believing it. The file header names the
functions it mirrors, so the comparison is a `grep`, not an
archaeology.

Treat a model as a claim about the past that was true when made. The
primary sources for current behaviour are the compiler and the docs
listed in `CLAUDE.md`.

## Running one

Requires Lean 4 (via [elan](https://github.com/leanprover/elan)); no
Mathlib, no `lakefile`, no project scaffolding — a model here is a single
file `lean` reads directly. Last checked with Lean 4.34.0 on
arm64-darwin, under a second. A model that checks prints nothing and
exits 0:

```sh
lean docs/formal/Perceus.lean
```

A `.tla` model runs under TLC: Java 11 or later and `tla2tools.jar`
from the [TLA+ releases](https://github.com/tlaplus/tlaplus/releases),
nothing else. Each `.cfg` next to the model is one run; the model's
section below gives the command and what each run should report.

Install only if you are using one. There is no reason for this toolchain
to be present on a machine that merely builds kaikai.

## Choosing a tool

Lean suits structural invariants: two rules that must not contradict
each other, a property that must hold on every path. It answers whether
a contradiction exists.

TLA+ suits state machines — scheduler ordering, signal deadlocks,
evidence-chain ordering — where the useful output is a *trace* showing
how the system reaches a bad state. Lean does not give you that trace.

Pick per question, not as an architectural commitment. Neither becomes a
project dependency by being used once.

## Models

### `Perceus.lean`

Drop-once for the owned-in-scope move, against `20dcde3d`.

Proves that the pre-#2084 rule was unsound: a never-read owned param in
a function whose body is a top-level match passed the move's selection
gate vacuously (zero reads per arm is "at most one read per path"), so
it received a branch-local drop in every arm *on top of* the entry drop
that every never-read param already takes. Two releases of one
reference.

The model is not told where the double release is. The emitters' rules
and the drop-once invariant are stated; the contradiction is derived.

Also checks that the shipped fix — skipping never-read params in
`pcs_owned_scope_move_params` — is sound both for the shape that broke
and for the shape the move exists to optimise, over every consuming
position below 16 and every arm count below 8.

A release is not the same as a drop: the move consumes the ref at its
single use, and that transfer is the release on that path. An earlier
draft of this model counted drops only and reported the consuming arm as
a leak — Lean rejected it. The `consume` site exists because of that,
and is a good illustration of the failure mode the model is exposed to:
it is only as honest as its transcription of the pass.

Mirrors `pcs_collect_entry_drops` (`perceus.kai:4468`),
`pcs_inject_param_branch_drops` (`perceus.kai:5108`), their selection in
`pcs_owned_scope_move_params` (`perceus.kai:5051`), and `LU`
(`infer.kai:1615`).

Scope: one shape — owned params, single top-level match. Reuse, TRMC and
borrow are not modelled, and the exit-drop path that handles `LUBlocked`
is out of scope; the model states that gap as a theorem rather than
leaving it implicit. It does not establish that the approach scales to
the whole pass.

### `PerceusFour.lean`

Drop-once across all four param release emitters, against `0de97943`.

`Perceus.lean` models one emitter pair on one shape. This one asks the
wider question: across every emitter that can release an owned param
(entry drop, exit drop, branch drop, and the move's consuming read), and
every classification the pass can assign it, does some combination
release twice or leak?

**Result: no collision.** Every reachable configuration releases the
param exactly once on every path, at 2, 3 and 4 arms, over all consuming
positions below 6 and use counts below 4.

That green is only worth as much as the search behind it, so the file
also checks its own teeth: 80 of 512 configurations are reachable,
spanning all three `LU` classes and both the `owned` and `forced`
treatments, and removing a fence makes the search report a violation. Two
fences are checked this way — `pcs_owned_scope_move_params` fence 6 (the
#2084 fix), whose removal reproduces the historical bug exactly, and the
`skip_set` test in `pcs_collect_exit_drops`, whose removal double-releases
the consuming path.

The interesting part of this model is not the theorems but the
reachability predicates. A release set that looks unbalanced is almost
always a configuration the driver never builds, so each predicate had to
be traced to the line that enforces it — `owned_moves ⊆ skip_set`
(perceus.kai:1885), `skip_set` requiring consumption on every path
(3293-3297), branchy single-use params always forced (4565-4579). Writing
those down is most of the work and most of the value: they are the
compiler's real preconditions, stated in one place.

Mirrors `pcs_collect_entry_drops` (4468), `pcs_collect_exit_drops` (4481),
`pcs_inject_param_branch_drops` (5108), `pcs_is_non_last` (3060),
`pcs_branch_aware_skip_params_b` (3289), `pcs_branchy_single_loop` (4565),
and the driver that wires them (1874-1984).

Scope: owned params of a fn whose body is a top-level match. Arm binders
and block-let binders have their own emitters and are NOT modelled — they
release different references, so they cannot collide with these four, but
they can collide with each other. Reuse, TRMC/goto lowering, borrowed and
raw params, and guards are out of scope; the file lists these explicitly
at the end.

### `PerceusBinders.lean`

Drop-once for arm binders and block-let binders, against `0de97943`.

Params cannot collide with binders — they release different references —
but the binder emitters can collide with each other, and they are the
shape behind #1784, #1786, #1791 and #1758.

**Arm binders: clean.** No double release and no leak across 40 reachable
configurations. The interesting part is that soundness here is not
"exactly one release": `pcs_arm_elide_names` rewrites a never-read plain
`PBind` to `_`, so no bind-incref is emitted and the correct number of
releases is zero. The invariant is "pay iff a birth ref exists", and an
earlier draft that demanded one release unconditionally reported four
false leaks.

**Block-let binders: clean, under the unified payer.** The model now
mirrors `pcs_let_payer` (`perceus_payer.kai`), which names exactly one
payer per binder — exit, tail, inline-emitter, the read itself, or none.

Getting here took two corrections, both from lanes measuring better than
this model did:

- **The leak this model first found was real but under-stated.** An
  unused `let` whose rhs was not a fresh allocation had no payer:
  `pcs_collect_block_let_exit_drops` declined `LUUnused` expecting
  `block_unused_lets` to pay inline, but that emitter requires
  `is_fresh_alloc(rhs)`. The first fix asked for a syntactic *shape*
  (`pcs_rhs_is_bare_var`) — which is not the complement of
  `is_fresh_alloc`, so `if`, `match`, a block, a field access and a pipe
  rhs all still leaked 30 over 10 calls. Eight corpus programs were in
  that class with no gate watching. Enumerating shapes is what failed;
  one predicate on both sides is what closed it.
- **The two payers are not independent.** `block_unused_lets` runs over
  the body perceus has already rewritten and decides by
  `name_read_in_block`, so a planted drop counts as a read and the
  emitter skips that binder. Modelling them as independent would report
  a double release for a shape the compiler handles correctly.

`never_both_payers` and `unread_always_has_exactly_one_payer` state the
result as properties rather than as a count: of 150 reachable
configurations the payer is total and single-valued — 85 exit (42 of
them planted at the post-tail site), 16 read, 1 inline.

A third correction, from checking the model against the merged code
rather than against the PR description: **there is no `tail` payer.**
`PcsPayer` has six variants and none of them is one; a binder whose read
is in the tail is still `PyExit`, and `ptd_needs_drop`
(perceus_tail_drop.kai) asks the same `pcs_fate_of` while guarding with
`ptd_has_drop` so the drop is planted once. The tail is a different
*site* for the same payer. This model had invented a separate `tail`
payer, which would have reported the two as independent decisions.

Excluded from the search, and therefore NOT cleared: a tail holding a
self-tail-call, where `ptd_tail_exit_drops` deliberately declines so TCO
survives and the goto/TRMC ledger pays per goto path. That ledger is not
modelled, so those 74 configurations are a gap, not a result.

Mirrors `pcs_collect_arm_drops` (perceus.kai:5517), `pcs_arm_drop_arms`
(5379), `pcs_collect_block_let_exit_drops` (4935), `ptd_tail_exit_drops`
(1958), `block_unused_lets` (emit_shared.kai:2002) and `is_fresh_alloc`
(2065).

Out of scope: destructuring `SLet`, rest/`@`/narrowing binders, the tcrec
goto ledger, handler-clause binders, and raw locals. The file lists these
at the end.

### `FiberWorker.tla`

The scheduler state a fiber reaches after a switch, against `7799bf9b`.

The runtime owner object is compiled at `-O0` because clang at `-O1`+
treats the address of a `_Thread_local` as constant within a function and
keeps it across `swapcontext`. A fiber that parks on one worker and is
stolen by another then writes the first worker's scheduler state (#1234).
This model asks whether reaching that state through the fiber instead, as
Go reaches its `m`/`p` through the current `g`, closes the hole without
`-O0`.

**What is modelled.** N workers, each with a steal list and a root
context that publishes a switched-out fiber only after its context is
saved (the commit and requeue stacks). Fibers run, switch at a park or a
yield, are re-enqueued, stolen from the head of another worker's list,
and resumed on any worker. Four fibers exercise the paths: `R` receives
two messages through `kai_mailbox_pop`; `S1` and `S2` send through
`kai_mailbox_push_cross_thread`, whose unpark runs after the mailbox
unlock; `S2` then parks in `kai_default_spawn_await` until `S1`'s
terminate walk wakes it; `G` drives two stack segments and is the
`Cancel` target. Every slot-lock or mailbox-lock critical section is one
atomic step, and every unlock-then-unpark window is a separate one —
those windows are where `wake_pending` matters.

The compiler's freedom is the `Mode` constant. After every switch the
resumed fiber first touches "its" worker's scheduler state (standing for
`kai_set_active_fiber`, `kai_drain_pending_free` and the commit and
requeue stacks):

- `"tls"`: through the thread pointer it read before the switch, or a
  fresh one. Both are legal compilations, so TLC tries both.
- `"fiber"`: through `worker[self]`, which the scheduler stores on every
  dispatch. Caching `self` across the switch is harmless (it is the same
  fiber); the field is reloaded because the switch is an opaque call and
  the fiber is reachable from the steal lists.

**Segments.** `G` can start, suspend, resume, drop (discontinue, pinned
with `seg_pin`), return from and abandon past segments, nest them, hold
one segment's continuation in another's frames, and yield the fiber from
inside a segment or from inside a discontinue's `finally`. A `Cancel`
observed at a yield point inside a segment escapes it and is continued by
the resumer (`kai_seg_escape` / `kai_seg_rethrow`).

**Properties.** Safety: `TouchesOwnWorker` (a fiber on W touches only W's
state), `ActiveCoherent`, `NoFiberOnTwoWorkers`, `NoLostNoDouble` (every
live fiber is in exactly one place; a parked one is reachable by a waker),
`ExactlyOnce` (each message is in flight or received, once),
`PinHolds`, and for segments `SegStates` (each segment is running iff on
the chain, suspended iff exactly one frame holds its box, otherwise none
or dead), `CleanupOnce`, `StackOnOneWorker`, `ParentAcyclic`,
`SegsDeadAtExit` (abandon and cancel leave nothing behind). Liveness
under weak fairness: `RunnableRuns`, `ParkedWithMessageResumes`,
`AwaiterResumes`, `AllDelivered`, `AllExit`.

**Running.** From the repo root, with `tla2tools.jar` anywhere:

```sh
java -cp tla2tools.jar tlc2.TLC -workers 4 -noGenerateSpecTE \
    -metadir /tmp/tlc-fw -config docs/formal/FiberWorker_tls.cfg \
    docs/formal/FiberWorker.tla
```

Swap the `.cfg` for each run below. Last checked with TLC2 2026.10.06
(rev `94d0c50`) on OpenJDK 17, arm64-darwin, 4 workers.

| Config | Mode | Bounds | Checks | Result | Distinct states | Time |
|---|---|---|---|---|---|---|
| `tls` | tls | 2 workers, 3 fibers | safety | `TouchesOwnWorker` violated, 14-state trace | — | < 1 s |
| `fiber3w` | fiber | 3 workers, 3 fibers | safety + liveness | pass | 99,241 | 13 s |
| `segs` | fiber | 2 workers, 4 fibers, 2 segments | safety + liveness | pass | 2,011,773 | 7 min 16 s |
| `segs3w` | fiber | 3 workers, 4 fibers, 2 segments | safety | pass | 24,257,713 | 3 min 17 s |

`Budget` bounds `G`'s choices: 4 in `segs`, 2 in `segs3w`. At 3 workers
with `Budget = 4` the safety search ran 15 minutes, 94,470,091 distinct
states to depth 38 of 60, with no violation, and did not finish.

**The #1234 counterexample** (`tls`, abridged):

1. `w1` runs `R`; the mailbox is empty, so `R` links itself as a receive
   waiter, reads its thread pointer (`w1`) and parks. `w1`'s root commits
   the park.
2. `w1` runs `S1`, which sends `m1`, takes `R` off the waiter list and
   unparks it onto `w1`'s steal list, then yields.
3. `w1` runs `S2`. `w2`, idle, steals `R` and resumes it.
4. `R`, now on `w2`, finishes its switch through the `w1` thread pointer:
   `w1`'s `kai_active_fiber` becomes `R` while `w1` is running `S2`.

The same state also breaks `ActiveCoherent`. The fiber-pointer variant
passes every property at the bounds above.

**Teeth.** The search sees each fence fail when it is removed (`Ablate`),
at the bounds of the run it belongs to:

| Config | Fence removed | First violation | Trace |
|---|---|---|---|
| `update` | the `worker` store on dispatch | `TouchesOwnWorker` | 9 states |
| `pin` | thieves skip a fiber with `seg_pin` | `PinHolds` | 25 states |
| `permit` | the park commit honours `wake_pending` | `NoLostNoDouble` (`R` parked, no waker) | 8 states |
| `dropheld` | a discontinue releases the boxes its frames own | `SegsDeadAtExit` | 22 states |

`FiberWorker_witness.cfg` holds the segment bounds without invariants.
Each `Witness*` operator in the model, checked as its only invariant,
must be violated, showing the search reaches that shape: a stolen
receiver resuming, a park committed over a pending permit, a segment
resumed on another worker, two segments nested, a segment holding
another's box, a fiber yielding while pinned, a cancel crossing a
segment, and an abandon landing on a segment. All eight are reached.

**Observation for the runtime change.** `HomeIsRunner` holds in every
passing run: whenever a fiber runs on W, its `home_thread` is W, because
enqueue and steal already stamp it under the slot lock. The pointer the
fiber needs exists; what `-O2` needs is that code after a switch reads it
instead of the thread pointer.

**Out of scope.** The idle-worker park and permit protocol
(`kai_worker_park`, spinning, `kai_sched_notify_work`): an idle worker
here keeps searching, and the liveness results hold under that
abstraction. Reactor parks (timer, socket, signal, file pool). Pinning to
the main thread. Discontinue and escape unwind one context per atomic
step, except the yield inside a discontinue's `finally`; a `Cancel` is
not observed during a discontinue. A continuation resumed from another
fiber is not modelled — `cont_crossing.kai` rejects it and
`kai_seg_take` traps it. Segment pool access goes through `noinline`
helpers in both modes, so it is not one of the cached touches.
