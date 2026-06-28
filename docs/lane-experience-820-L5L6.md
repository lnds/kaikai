# Lane experience — #820 L5-delete + L6 (close the umbrella)

The lane that closes #820: retire by-name dynamic dispatch **for user effects**,
land named handler instances (§6), and rewrite the docs to the honest hybrid
model. Point of no return (it deletes a dispatch mechanism). Closes the umbrella.

## Scope as planned vs. as shipped

**Planned (issue #820 reformulation + lane plan L5/L6):** delete the spawn
evidence clone and the by-id alias dispatch fully; bound `kai_evidence_lookup_node`
to fiber-local + Ffi; gate `KAI_EVIDENCE_FRAME_ONLY`; land §6 named instances
(flagship `add(c1,c2,dst)` → 3); rewrite docs to the hybrid model.

**Shipped:**
- Spawn evidence clone **deleted** in both runtimes (`grep clone_evidence = 0`).
  A capability no longer crosses a `spawn`; a fiber-local op performed in a
  spawned fiber that installed no handler reports `effect not handled in fiber`
  via a runtime guard instead of dereferencing a NULL node.
- §6 named instances **shipped end-to-end on C and native**: the flagship runs
  to 3, the escape rule rejects vectors 3/4 (and 2 by over-restriction), the
  capability type is the effect's own nominal type, monomorphic v1.
- `KAI_EVIDENCE_FRAME_ONLY` gate shipped (`tools/evidence-frame-gate.sh`, tier0):
  a user effect that reaches the by-name walk fails the build.
- Docs rewritten to the **hybrid** model (three mechanisms, each named).

**Shipped DIFFERENTLY than planned — the by-id retention.** The plan said
"delete the by-id alias dispatch fully." Shipped: the by-id is **retired for
user named instances** (they use capability passing now) but **retained,
bounded, for `var`/State/Reader cells**. Reason discovered by running: a `var c`
/ `var go` pair installs two `State` nodes on one stack and distinguishes them by
`handler_id`; extending the capability path (`#`) to cells breaks
`stream_early_stop` (a `var` cell read inside a pipe lambda lowers to a dispatch
on the effect name, emitting an undeclared `kai_State`). The by-id is the
genuine same-body / closure-capture identity mechanism for cells, not dead debt —
exactly parallel to the bounded by-name walk the issue itself retains for
fiber-local builtins. The owner ratified the bounded retention. The honest model
is therefore **three** mechanisms, not one; the docs name all three.

## Design decisions and alternatives considered

- **Capability type = the effect's nominal type** (`TyCon(None, "Cell", args)`),
  not a `TyCap`/`Handler[E]` constructor. Reuses `inf_resolve_ty`'s existing
  fallback, adds no `Ty` arm (selfhost stays byte-identical — no existing
  program produces a capability-typed value), and matches design §3. The arity
  is truncated to the effect's tparam count so a non-parametric `Cell` is
  `Cell[]` even when `with Cell(0)` infers an Int state.
- **`#<recv>` dispatch tag vs reusing `@`.** A new tag keeps the capability path
  (read `kai_<recv>`) cleanly separate from the by-id path (`@`, read
  `kai_alias_<a>_id`), so alias-for-cells and named-instances coexist without
  the typer guessing intent. `strip_alias_suffix` strips both for coverage.
- **Alias and parameter share one codegen path.** A `with Eff as a` binds
  `kai_<a> = &_node` (the handle's own evidence node as a value); a parameter
  `c: Eff` carries the node positionally. The op-call reads the node from the
  named register either way — no by-id walk for user effects.
- **Escape rule positional, no flow analysis** (§6.2). Reused the existing
  cell-escape closure check (`lcr_escape_in_value`), extended it to all aliased
  effects, generalised its message from "cell" to "capability", and added a
  direct bare-return check (vector 3). Vector 2 (let RHS) is rejected by a
  cheaper path with a less specific message — an accepted over-restriction.
- **Clone delete → `evidence_top = NULL`, not a filtered inherit.** Filtering
  the chain to keep only fiber-local nodes was rejected: the parent's mailbox is
  not the child's per-fiber disposition, so inheriting it would make the child's
  `Actor.self()` return the parent's mailbox — the very capability crossing the
  spawn is meant to prevent.

## Structural surprises the brief did not anticipate

1. **L6 is not a thin surface layer.** The brief framed L6 as "named instances,
   monomorphic v1" on top of a single-path model. In fact the flagship failed in
   the **typer**, not codegen: there was no capability-as-typed-value at any
   tier. It required a new typer surface (resolve a capability receiver, type the
   binder, suppress the row label for a provided capability) plus an ABI on both
   backends. Escalated to the owner, who chose to design and implement it in this
   lane.
2. **The spawn clone masked a real soundness hole.** Deleting it turned a
   silently-working program (`Actor.send` in a raw-spawned fiber against the
   parent's handler) into a segfault. The honest fix is the runtime guard +
   migrating those fixtures to install their own mailbox — the clone was making
   an unsound program accidentally run.
3. **Parametric effects nest the ty-arg through a call.** `bump(a)` with `a :
   State[Int]` produced `State[State[Int]]` because the discharge matched the
   full handler state type against an arity-0-or-1 label. Fixed by discharging
   against the effect's own tparam tyargs (truncated to arity).
4. **The naked-read sugar collides with named instances.** A `with State as a`
   for a `var` cell reads `a` naked (`a.get()`); a user `with Cell as a` passes
   `a` as a capability. The `eff_is_cell_effect` split keeps cells on the legacy
   path and user effects on `#`.

## Fixtures added and coverage

- `examples/effects/two_instances_through_call.kai` (+ `.out.expected` = 3) —
  the §6.1 flagship, out of quarantine.
- `examples/effects/cap_escape_rejected.kai` (+ `.err.expected`) — escape rule
  vector 3.
- `examples/effects/spawn_inherited_actor_rejected.kai` — the spawn-clone guard
  (added with the L5-delete commit).
- Migrated `m8x_4_recv_blocking`, `m8x_4_block_sender`, `m8_fiber_discard`,
  `m8_fiber_discard_yields` to install the spawned fiber's own mailbox; goldens
  unchanged.
- `tools/evidence-frame-gate.sh` — the binary grep-oracle, wired into tier0.

**Coverage gaps:** no fixture yet exercises a *parametric* user effect named
instance through a call (the flagship uses non-parametric `Cell`); the `#` path
is proven for `Cell`, and `State` named instances would re-enter the naked-read
split. The capability-through-call surface is monomorphic-only by design (§6.3).

## Follow-ups left for next lanes

- A parametric named-instance fixture (`f(c: State[Int])` through a call) once
  the naked-read / named-instance interaction has a cleaner resolution than the
  `eff_is_cell_effect` split.
- `mask` is not provided (§6.3) — name the outer instance instead.
- Whether the by-id can ever be unified into the `#` path for cells (would
  require porting the cell closure-capture decode to `#`) is left open; the
  bounded retention is correct and not blocking.
