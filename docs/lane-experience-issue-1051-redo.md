# Lane experience — issue #1051 (redo)

Spawned-fiber builtin-default resolution, redone after #1052 was reverted in #1058
for regressing the plain non-spawn handler path. This retro records why #1052
broke, and how the redo lands the spawn fix without touching the non-spawn layout.

## Scope

- **As planned:** re-land the #1051 fix (a builtin effect with a stdlib default
  block, performed in a spawned fiber whose parent installed a user handler,
  must resolve to the child's default — not segfault on C, not mis-diagnose on
  native) *without* regressing `examples/quickstart/04_effect.kai` (the plain
  `handle … with Log` that #1052 SIGSEGV'd on native).
- **As shipped:** identical scope, plus a sharper root-cause on why #1052 broke
  (name-collision, not just the op-thunk drop) and a partial-handler fixture the
  original lane never exercised.

## The bug (#1051)

Per-fiber evidence (#820): a child fiber starts with an empty evidence stack; the
parent's handler does not cross the spawn boundary. So a child performing a
builtin effect with a `default {}` block must resolve to that default, exactly as
if the parent installed no handler.

Two independent defects, one per backend:

1. **Default node never initialised (C segfault, native mis-diagnose).** When a
   lexical parent handler absorbed the builtin from `main`'s inferred row, the
   default-install set (`main_row ∩ builtin_order`) dropped it, so no default
   node was built. The child dereferenced a zero-init node → segfault on C; the
   native safety floor turned it into "effect not handled in fiber". Fix:
   `default_effects_absorbed_by_handler` collects extern-default effects handled
   anywhere in the program; both backends add them back to the default-install
   set. **This does not touch op-thunk layout** — it changes *which* defaults are
   installed at startup, nothing about *how* ops resolve.

2. **Native op-field index mismatch.** The perform computes an op's Ev field
   statically from the innermost matching handler's `op_thunks`, but derefs
   whichever node the fiber carries at runtime. A non-spawn perform always hits
   the *same* handler node its field came from — self-consistent regardless of
   order. A spawned child hits the *default* node, whose layout is the effect's
   declaration order. When the handler's clause order differs from decl order,
   the static field lands on the wrong default slot (`Log.info` ran the `warn`
   default). Fix: reorder each default-carrying handler's `op_thunks` into the
   effect's default-block order and fill omitted ops with the default forwarder,
   so install and dispatch match the default node.

## Why #1052 broke the non-spawn path

#1052's `canon_handler_op_order` reordered **every** handler's op-thunks to
`lookup_effect_op_names(decls, eff)` — the decl-order looked up **by effect
name** — and **dropped** any op the handler omitted.

`04_effect.kai` declares its own `effect Log { log(msg) }`, homonymous with the
stdlib `effect Log { debug; info; warn; error; default {} }`. `lookup_effect_op_names("Log")`
walks the raw decls and returns the **first** `Log` — the stdlib one, four ops.
`reorder_op_thunks([debug,info,warn,error], [log])` finds none of the four stdlib
ops in the handler's single `[log]` thunk and **drops everything** → an empty
`op_thunks`. The install then wired no ops, and the `Log.log` perform dereferenced
an empty slot → SIGSEGV before printing.

So the real cause was **effect-name collision resolved to the wrong decl**,
amplified by the drop. The typer already shadows correctly here (the emitted
`EvLog` is the user's one-op struct); the KIR lowering lost that resolution by
keying on the effect name string.

## How the redo avoids it

Two guards, both anchored to the effect's *default block* rather than a global
name lookup:

- **Only default-carrying effects are reordered.** `canon_one_handler` consults
  `kir_default_for_effect(h.eff, decls)` (the same path the default node itself
  is built from). A handler for an effect with no default block is left
  untouched — there is no runtime default node to disagree with, and its
  op_thunks are already self-consistent across its own install and dispatch.
- **Superset guard against the homonym trap.** `op_thunks_covered_by` reorders
  only when every op the handler provides appears in the resolved default block.
  The user `Log { log }` handler's `log` is absent from the stdlib `Log` default
  block → not covered → left untouched. The collision that sank #1052 cannot
  reproduce, because a mis-resolved default block fails the coverage check.
- **Zero drop.** `reorder_op_thunks` fills an omitted op with the effect's
  default forwarder instead of dropping it, so the list is exactly as long as the
  effect's op set and every op's field index equals its declaration position on
  both the handler node and the default node.

The net effect: the op-field layout is intrinsic to the effect for
default-carrying effects (where a child can cross to the default node), and
completely unchanged for user effects without a default (where #1052 crashed).

## The partial-handler case

`asu` flagged the discriminating scenario the original lane never tested: a
partial handler (customises `info`, omits `warn`) whose spawned child performs
`Log.warn`. A fix anchored to the *user handler's* op_thunks gives `warn` field
`-1` (absent from the handler) → crash. The redo fills the omitted `warn` slot
with the default forwarder at its decl index, so the child's `Log.warn` resolves
to the default `warn` handler. Verified on both backends (exit 0, `WARN` line,
not `info`'s slot, not a crash). This is the case that proves the redo is the
"decl-order intrinsic to the effect" direction, not the weaker "align default to
one handler" variant.

## Fixtures

- `examples/effects/issue_1051_spawn_parent_handled_builtin_default.kai` — Log,
  the verbatim repro; asserts exit 0, worker stdout golden, and the default INFO
  line on stderr via regex (so the op-field lands on `info`, not `warn`).
- `examples/effects/issue_1051_spawn_parent_handled_env_default.kai` — Env, a
  second extern-default builtin; the child resolves `Env.get` to the default
  `None`, not the parent handler's `Some("from-parent")`.
- Wired into `test-issue-1051-spawn-parent-handled-builtin-default` (C harness,
  in `.PHONY` / `TEST_LIGHT_TARGETS` / `test-fast`). Native op-field parity is
  the serial backend-parity sweep's job — the gate #1052 lacked.

## Cost vs estimate

Direct. The causa-#1 half was #1052's already-correct code (reapplied verbatim,
five call sites). The causa-#2 half is smaller than #1052's — one reorder pass
plus a coverage guard, no six-file spread. The design time went into the
name-collision diagnosis and the `asu` consult that ruled out the tempting
"align default to the user handler" shortcut before it was written.

## Follow-ups

- The effect-name → decl resolution is still by-string across KIR lowering
  (`lookup_effect_op_names`, `find_effect_default_block`). The superset guard
  papers over the homonym case here, but a by-identity effect handle threaded
  from resolve would remove the whole class of collision bugs. Out of scope for a
  bug-fix lane; worth an issue if another homonym trap surfaces.
- Multiple user handlers of the same builtin with different clause orders share
  one singleton default node. The reorder aligns each handler independently to
  the decl order, so they all agree with the default — this case is covered, but
  it was not fixture-tested; a belt-and-suspenders fixture would pin it.
