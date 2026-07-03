# Lane experience — issue #1051

**Scope:** a fiber spawned inside a `nursery` that performs a *builtin* effect
with a stdlib default block (`Log`) segfaulted on the C backend (exit 139) when
the *parent* fiber installed a user handler for that effect. Native diagnosed
("effect not handled in fiber: Log", exit 1) instead of segfaulting, but that
diagnostic was itself wrong: the child should fall to the default handler.

## Scope as planned vs as shipped

Planned (per the issue's excellent diagnosis): a C-backend fix to the spawned-
fiber thunk slot for builtins-with-default. Shipped: a fix that turned out to
be **two distinct bugs, one per backend**, plus a latent native ABI bug the
first fix uncovered:

1. **The default node was never initialised** when a lexical parent handler
   absorbed the effect from `main`'s row (both backends share this cause).
2. **The native op-field layout was derived from clause order, not decl order**
   — a latent bug that only bit once the default node existed and a perform
   crossed the handler→default boundary.

Three source areas touched (`emit_c.kai` main-wrapper, `kir_lower.kai` default
set, `kir_lower_fns.kai` op-field canonicalisation), plus a shared collector in
`emit_shared.kai` so both backends compute the absorbed-effect set once.

## Root cause — how the C dump localised it

The issue hypothesised the parent's evidence node leaked across the spawn. The
C dump (`kaic2 --path stdlib repro.kai > repro.c`) refuted that and pinpointed
the real cause in three greps:

- `_kai_worker_thunk` resolves its `Log` slot via
  `kai_evidence_lookup_or_default("Log", &_kai_default_node_log)` — correct:
  the child walks its *own* (empty) evidence and falls to the default node.
- `_kai_default_node_log` is declared (`KaiEvidence _kai_default_node_log;` —
  zero-init BSS) but, in the repro, **never initialised**: the
  `kai_evidence_init_default(&_kai_default_node_log, …)` line present in the
  control (no parent handler) is *absent* when the parent handles `Log`.

So the segfault was `_node_op->handler` deref on a zero-init node, not a cross-
fiber leak. The parent's `handle … with Log` absorbs `Log` from `main`'s
inferred row; `default_setups_for` installs a default only for effects still in
that row, so an absorbed builtin lost its startup init. A spawned child, which
depends on that global default (its own evidence is empty), then dereferenced a
NULL handler.

## Policy applied (validated with `asu`)

Per-fiber evidence + default: a builtin effect with a default block, performed
in a spawned fiber, resolves to its default handler unless *that fiber* installed
one. The parent handler does not cross the spawn boundary — in *both* senses:
it neither governs the child nor suppresses the child's default fallback. A user
effect with no default still diagnoses (#978 contract, unchanged). The only
asymmetry is the existence of the default.

Fix: `default_effects_absorbed_by_handler` collects extern-default effects
handled by some `handle` in the program, and both backends add them back to the
default-install set (`emit_c.kai`'s main-wrapper row; `kir_default_handlers`).

## Structural surprise the brief did not anticipate

The native backend had the *same* structural bug (the default node was not
installed either), so once it *was* installed, a second latent bug surfaced:
`Log.info` in the child ran the `warn` default. The native computes an op's Ev
field index from the `KHandlerDecl.op_thunks` list, whose order is the handle's
clause order — which the clause collector *reverses* (`[info, …]` builds as
`[…, info]`). The parent handler's Ev and its field index both derived from that
reversed list, so they were mutually consistent and a direct perform worked by
coincidence. But the field index is static while the node it derefs is dynamic:
in the child it hits the *default* node, whose layout is the effect's canonical
decl order. Field-from-reversed applied to a canonical node = wrong slot.

The C backend never had this: it addresses Ev fields by *name* (`_ev_op->info`),
so field order is irrelevant.

Fix (opt A, `asu`-validated): `canon_handler_op_order` reorders every handler's
`op_thunks` into the effect's declaration order once, at `KHandlerDecl`
assembly. `nemit_install_ops` (Ev fill) and `nfx_op_field` (dispatch) both read
that same canonicalised list, so install and dispatch stay consistent and match
the default node. The Ev layout is now intrinsic to the effect, as the C
backend already treats it — a vtable-style fixed slot order, not a per-call-site
order.

## Fixtures added

- `examples/effects/issue_1051_spawn_parent_handled_builtin_default.kai` — the
  `Log` repro verbatim; asserts exit 0, worker stdout golden, and the default
  `INFO` line on stderr via regex (so the op-field lands on `info`, not `warn`).
- `examples/effects/issue_1051_spawn_parent_handled_env_default.kai` — the same
  shape with `Env` (a second extern-default builtin) to prove generality: the
  child resolves `Env.get` to the default (`None`), not the parent handler's
  `Some("from-parent")`.

Both wired into `test-issue-1051-spawn-parent-handled-builtin-default`
(`.PHONY`, `TEST_LIGHT_TARGETS`, `test-fast`), C-backend harness like the #978
sibling.

## Verification

Both backends: repro exit 0, default runs in the child, byte-identical C vs
native (timestamp normalised). Control (no parent handler) unchanged. #978 user
effect still diagnoses on both. ASan (-O2) on both fixtures clean (the segfault
was a cross-fiber-adjacent deref; a double-free from the fix would trip too).
Selfhost byte-id green (`kaic2b.c == kaic2c.c`) — the op-thunks reorder did not
move any KIR golden. tier0 green; `test-effects`, `test-effect-runtime`,
`actors`, `log-asan`, `issue-978`, `issue-997`, `test-kir` all green.

## Follow-ups

- The clause collector's reverse (`[info, …st.clauses]`) is now masked by the
  canonicalisation, but it is still the underlying reason a per-handler layout
  was ever inconsistent. No action needed — the layout is canonical regardless
  of clause order — but a future lane touching clause lowering should know the
  field index is decl-order, not clause-order.
