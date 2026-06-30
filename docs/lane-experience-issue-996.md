# Lane experience — issue #996: C backend `redefinition of kai_<var>`

## Scope as planned vs as shipped

**Planned.** Fix the C backend so a `var` cell named `x`, `state`, or `log`
no longer emits `error: redefinition of 'kai_<var>'`. Verify on both backends,
add a regression fixture, generalise the fix beyond the three reported names.

**Shipped.** Exactly that, in one file (`stage2/compiler/emit_c.kai`) plus a
sugars fixture. Net −3 LOC of compiler source.

## Root cause

A `var x := init` that does not qualify for slot-specialisation desugars
(`desugar.kai:2774`) to `EHandle(_, "State", _, Some(init), Some("x"), clauses, ret)`
with `ret = var_canonical_return = HR("x", EVar("x"))` — the return-clause
param name is hardcoded to `"x"` regardless of the user's variable.

`emit_handle` emits the handle as one C statement-expression `({ ... })`. Inside
that single C scope it declared the `as`-bound capability **twice**:

- opening: `KaiHandlerId kai_alias_<a>_id = _ev.handler_id;` (the by-id handle)
  **and** `KaiValue *kai_<a> = (KaiValue *) &_node;` (the §6 capability-value node);
- closing (the return clause): `KaiValue *kai_<pname> = _body_result;` with
  `pname` always `"x"`, plus a stateful prologue with the **literals**
  `KaiValue *kai_state = _ev.state; KaiValue *kai_log = _ev.state;`.

So `var x` collided (`kai_x` opening node vs `kai_x` return param); `var state`
and `var log` collided (`kai_state`/`kai_log` opening node vs the prologue
literals). Any other name was free. The native backend binds these as named SSA
registers, not textual C identifiers, so it never reproduced.

## Design decision — A (suppress the dead node) over B (gensym the temporaries)

Two ways to break the collision:

- **A** — stop emitting `kai_<a> = &_node` for by-id cell effects.
- **B** — rename the closing temporaries (`kai_x`/`kai_state`/`kai_log`) to
  gensym names the user cannot produce.

A is correct because the `kai_<a> = &_node` node only backs the §6
capability-value dispatch path (tag `#<name>`), and the typer **never** tags a
cell effect's op with `#`: `infer.kai:10020-10025` routes `State`/`Reader` cells
(`eff_is_cell_effect`) to the by-id path (`@<name>`) unconditionally, user
effects to `#`. So for a cell the node is dead — emitting it is pure collision
surface. Suppressing it fixes the bug at its root for **every** variable name,
not just the three reported.

B would have been more invasive and riskier: the return-clause body reads
`state`/`log`/`x` by name (`EVar("state")` emits `kai_state`), so a gensym would
force an alpha-rename inside the return clause too — the class of bug that bit
the KIR shadow-binder clobber lane. A touches one `match` arm; B touches
`var_canonical_return`, the stateful prologue, and the return-body `EVar`s.

The discriminant is the **dispatch property** (`alias_is_byid_cell` ⇒ State or
Reader, the same set `infer.kai`'s `eff_is_cell_effect` uses), not the literal
`"State"`. Reader is in the same boat as State; a future by-id cell effect joins
the set in one place.

## Generality

The fix is not special-cased to `x`/`state`/`log`. It removes a binding that is
dead for **all** by-id cells, so a `var` of any name — including one that would
collide with a not-yet-introduced internal C temporary in the closing block —
is safe. The orthogonality asu flagged holds: the partition is *taggable-by-id*,
not *stateful*; the stateful prologue (`kai_state`/`kai_log` for hand-written
`State` handlers whose return clause reads the final state) is untouched, and a
user effect's named instance still gets its `kai_<a> = &_node` node.

## Fixtures

`examples/sugars/issue_996_var_name_runtime_collision.kai` — three cells named
`x`, `state`, `log`, each exercising get + set, asserting concrete results.
Lands in the `examples/sugars/*` glob, so `test-sugars` (C backend, the broken
path) gates it on every PR. Verified manually on `--backend=native` too.
`examples/native/effect_alias.kai` (a user effect's named instance aliased
`log`) was re-checked on both backends to confirm the `#` path stays intact.

## Surprises / cost

- The bug report was unusually precise — it named the exact opening/closing
  collision in the generated C. Confirming it took one repro + one read of the
  generated `out.c` (the error message prints the whole offending line).
- The native kaic2 needs an explicit `make KAI_LLVM=1 kaic2` after the C-only
  build; the C-only binary rejects `--backend=native` outright, so both backends
  needed separate builds to verify.
- A hand-rolled "is kaic2b.c == kaic2c.c yet" poll false-FAILed by reading
  `kaic2c.c` mid-write. The authoritative signal is the `make selfhost` target's
  own exit code (`set -e` + `exit 1` on DIFF), not a side check.

## Follow-ups

None. The dead-node suppression is complete; no phasing, no deferred work.
