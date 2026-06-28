# Effect dispatch — honesty targets

What kaikai's effect-op dispatch actually does. The model is **hybrid**: a user
effect's evidence is passed with the call (capability passing), while two builtin
classes keep a bounded runtime lookup for a sound reason. The history below
records how the by-name walk was retired *for user effects* — it is not retired
wholesale, and this document names every mechanism that survives so the model can
never silently drift back into "one resolution path" prose that is false.

Primary design: `docs/effects-capability-passing-design.md` (Decision A —
retire by-name dynamic dispatch *for user effects*, transport evidence with
calls). Lane plan: `docs/effects-capability-passing-lane-plan.md` (L0–L6).
Umbrella: #820 (closed).

## Load-bearing claim

**Dispatch model: hybrid. Capability passing for user / value-transportable
effects; a bounded runtime lookup retained for fiber-local builtins and Ffi.**

A user effect's op call resolves through the evidence the caller supplied — a
frame slot (`__evf[]`) the call site fills, or, for a named instance, the
capability value bound to `kai_<name>` (the handle's own evidence node, or a
`c: Eff` parameter). No walk, no name match. The fiber-local builtins
(`Cancel`/`Link`/`Monitor`/`Spawn`/`Actor`) resolve through the runtime's
per-fiber disposition (`kai_evidence_lookup_node`), because their handler is
installed inside the fiber and must not cross a `spawn`. `Ffi` is a zero-op
boundary marker that dispatches nothing. This mirrors Koka's split of `io`
(runtime-resolved) from handled effects (evidence vectors).

## The three dispatch mechanisms (all live, each for a reason)

| Mechanism | Serves | Why it is correct, not debt |
|---|---|---|
| **Capability passing** (`__evf[]` slot; `kai_<name>` for a named instance, tag `#<recv>`) | User / value-transportable effects (`State`, `Reader`, `Writer`, `Stdout`, `File`, `Clock`, `Mutable`, …, and user-declared effects) | The evidence is the value the caller resolved; the perform reads its slot. Distinct instances stay distinct by parameter position — this is what closes #789 and lands §6 named instances. |
| **By-id lookup** (`kai_evidence_lookup_node_by_id`, tag `@<name>`) | `var` / `State` / `Reader` cells | A `var c` / `var go` pair installs two `State` nodes on one stack; the by-id `handler_id` distinguishes them in the same body and across the cell's naked-read closure capture, where a by-name walk would collapse to the innermost. Bounded to cells. |
| **By-name walk** (`kai_evidence_lookup_node`, `lookup_or_default`) | Fiber-local builtins (`Cancel`/`Link`/`Monitor`/`Spawn`/`Actor`) and a frameless perform of a default-bearing builtin (`main`, clause roots) | A fiber-local handler is installed per fiber and must not ride a slot across a `spawn`; it resolves against the fiber's own disposition. `Ffi` carries no ops. Bounded to these. |

The `KAI_EVIDENCE_FRAME_ONLY` gate (`tools/evidence-frame-gate.sh`, tier0) pins
the boundary: a user effect that reached the by-name walk is a regression and
fails the build.

## Where we are today

| Row | Claim | State |
|---|---|---|
| **Shipped** | A user effect's named instance performs against its own evidence, threaded through calls by slot — distinct instances distinct by position. | True. The §6 flagship `add(c1, c2, dst)` (three `Cell` instances summed through a call) runs to `3` on C and native. |
| **Shipped** | Op-name collision between two effects is rejected at compile time (#789). | True. The by-name walk cannot tell two effects sharing an op name apart, so the collision is a compile error, not silent corruption. Fixtures: `examples/effects/collision_*.kai` (+ `.err.expected`). |
| **Shipped** | A capability bound by `as` is second-class: only a call argument or op receiver, never returned or stored (§6.2). | True, checked locally with no flow analysis. Fixture: `examples/effects/cap_escape_rejected.kai`. |
| **Bounded by design** | Fiber-local builtins and `Ffi` resolve through the runtime, not a slot. | True and permanent — a soundness constraint (a capability does not cross a `spawn`), not pending work. |

## When #789 / #820 closed

#789 closed when capability passing made op-name collision a compile error and
the bounded-walk gate ensured no user effect reaches the walk. #820 closed when
the §6 named-instance surface landed on the single-path-per-class model and the
spawn evidence clone was deleted (a capability no longer crosses a `spawn`; a
fiber-local op performed in a fiber that installed no handler reports
`effect not handled in fiber`, not a segfault).

## What this document is NOT

- Not a claim of "capability-passing pure" or "the by-name walk is deleted":
  the walk is retained, bounded, for fiber-local + Ffi builtins, and by-id for
  cells. Writing otherwise would be the lying doc this discipline exists to
  prevent.
- Not a roadmap or a calendar.
