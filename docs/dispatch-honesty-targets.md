# Effect dispatch — honesty targets

What kaikai's effect-op dispatch actually does today versus what
`docs/effects.md` and CLAUDE.md describe. The two diverged silently for
months because they are **observationally equivalent on every program in
which each effect has at most one live handler** — which is every program
in the corpus, the stdlib, and the compiler itself. No fixture *could*
fail. This document gives the dispatch model the same honesty discipline
that fibers and Perceus already carry, so the drift can never be invisible
again.

Primary design: `docs/effects-capability-passing-design.md` (Decision A —
retire by-name dynamic dispatch, transport evidence with calls). Lane plan:
`docs/effects-capability-passing-lane-plan.md` (L0–L6). Umbrella: #820.

## Load-bearing claim

**Dispatch model: dynamic-scoping-by-name (NOT the capability-passing the docs claim) — accidental equivalence holds only while each effect has ≤1 live handler.**

An op call walks the fiber's evidence stack at runtime and picks the
innermost handler whose **effect label** matches (`kai_evidence_lookup_node`,
`stage2/runtime.h`). Resolution happens wherever the perform executes, not
where the obligation is discharged; callees receive nothing. That is
dynamic scoping. The by-id path added for `with Eff as a` aliases is real
but lexical-only — the minted `handler_id` is a C local that dies at the
first function or lambda boundary.

## Where we are today (2026-06-27)

| Row | Claim | State |
|---|---|---|
| **Shipped** | Effect ops resolve to *a* handler of the right effect, one-shot resume, masking drops from the row, spawn copies the parent evidence chain into the child fiber. | True today. |
| **Accidental** (works only because ≤1 live handler) | An op call binds the *intended* handler instance. | Holds **only** while each effect has at most one live handler on the stack. With two live handlers sharing an effect label (two `var`s, a delegating runner, an alias shadow), the by-name walk binds the **innermost** — which is correct dynamic scoping but **not** instance-addressed capability passing. The two models are indistinguishable here; the corpus never exercises the distinguishing case. |
| **Broken (#789)** | Two handlers sharing an **op name** resolve to the right evidence. | False. Op-name collision binds the wrong handler's evidence: silent value corruption (Repro 1), runtime type mismatch (Repro 2), or segfault (Repro 3). Reproduced on **both** backends today (C + native). Fixtures: `examples/effects/quarantine/collision_value_corruption.kai`, `examples/effects/collision_type_mismatch.kai` (+ `.err.expected`), `examples/effects/quarantine/collision_segfault.kai`. |
| **Broken (#820 flagship)** | A capability passed through a function call performs against its own instance. | False. The `with Eff as a` capability cannot leave the `handle` body as a call argument; capability-as-parameter op calls (`dst.set(...)`) are rejected at compile time. Fixture: `examples/effects/quarantine/two_instances_through_call.kai` (quarantined, L3-TARGET in its top comment). |
| **Deferred** | Named handler instances surface (§6), the by-name walk / alias special case / spawn evidence clone deleted (§4). | Not started. Lands across L2–L6; surface in L6-surface, deletes in L5. |

## The distinguishing fixtures

selfhost byte-id is **false-green for this entire effort**: the compiler
corpus contains no op-name collision and no instance passed through a call,
so the model can be silently wrong and still self-host byte-identically. The
fixtures in `examples/effects/` (+ `examples/effects/quarantine/`) are the
only real oracle — the programs that *distinguish* dynamic-scoping-by-name
from capability passing. They document today's broken/accidental behaviour
as quarantine (no golden — corruption/segfault have no stable golden) or as
`.err.expected` (a stable abort), and flip to defined behaviour as the lanes
land:

- `collision_value_corruption.kai` — quarantine → compile error in L2.
- `collision_type_mismatch.kai` — `.err.expected` (broken abort) → compile error in L2.
- `collision_segfault.kai` — quarantine → compile error in L2.
- `two_instances_through_call.kai` — quarantine → `.out.expected` in L3.
- `m8_3_spawn_console` & siblings — spawn-audit baseline goldens (the
  before-picture L3's spawn audit diffs against).

## When this flips

The whole table collapses to a single **Shipped: capability-passing** row in
L6-docs, after L5 deletes the by-name walk and makes one resolution path the
only path. Until then, the Broken and Accidental rows stay — they are the
truth, and the gate on each lane is the distinguishing fixtures, not byte-id.

## What this document is NOT

- Not a roadmap — sequencing lives in the lane plan.
- Not a calendar — no dates on the flips beyond "the lane that owns them."
- Not a claim that the corpus is unsafe to run today: every program in the
  corpus has ≤1 live handler per effect, so the Accidental row holds for all
  of them. The Broken row is reachable only by the distinguishing programs,
  which is why they had to be written by hand.
