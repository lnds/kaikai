# Lane retro — issue #516 (effect-row propagation through qualified ops)

Date: 2026-05-12
Branch: `lane-issue-516-effect-row`
Closes (partially): #516

## Scope as planned

The brief named six silent-contract fixtures from the #511 audit
that compile cleanly today despite advertising effects that should
be visible in the caller's row:

1. `pub_fn_mutable_unannotated.kai` — `pub fn` writes via
   `Mutable.array_set` on a parameter Array; row pure.
2. `pub_fn_transitive_effect.kai` — `pub fn caller() : Int`
   transitively calls `callee() : Int / Stdout`.
3. `mutable_op_no_handler_or_row.kai` — body row omits `Mutable`
   despite a qualified write.
4. `mutable_through_field.kai` — observable write through a
   record field; row pure.
5. `mutable_param_write_pure_row.kai` — observable write through
   a parameter Array; row pure.
6. `ffi_call_no_capability.kai` — `main : Unit` calls an
   `extern "C" fn / Ffi`.

The plan was: locate the gap in `synth_call` / qualified-op typing,
close it uniformly across bare / qualified / transitive call shapes,
promote all six fixtures with `.err.expected` goldens.

## Scope as shipped

Five of the six fixtures are now enforced. `pub_fn_transitive_effect.kai`
remains in `examples/negative/silent_contract/`; the underlying
"propagate every callee row label into the caller's row" change
landed transiently during the lane and surfaced 318 latent errors
inside `stage2/compiler.kai` itself (109 unique helpers — `diag_*`,
`dump_*`, `check_*`, `synth_*` — currently absorb `Console` /
`File` effects through an open row tail). Rolling out a fix that
breaks selfhost was out of scope for this lane, so transitive
propagation for non-capability effects was reverted and the
`Stdout`-transitive fixture stays parked until a follow-up sweep
annotates the compiler's own rows.

The fix that DID ship is two narrow corrections to existing logic
plus a single-effect propagation tier for capability-only labels:

- `mask_local_mutable_demand` (issue #251 / #252) was rewritten to
  recognise the qualified callee spelling. The bare prelude form
  `array_set(...)` lowered to `EVar("array_set")`; the qualified
  `Mutable.array_set(...)` form lowers to `EVar("Mutable.array_set")`
  (the dispatch key minted by `synth_op_call_with_scheme_keys`).
  The masking walker only knew the bare names, so it stripped the
  `Mutable` label that `synth_op_call_with_scheme_keys` had
  correctly added — even when the write targeted a parameter
  Array, the very shape the discipline forbids.
- `check_body_row`'s `REmpty + fname == "main"` arm was changed
  from "absorb every builtin effect" to "absorb every builtin
  effect except `Ffi`". `Ffi` is the only builtin in
  `builtin_effect_names()` that declares zero ops and has no
  default handler installed by `emit_main_wrapper` — absorbing
  it silently lets `main : Unit` call FFI shims without ever
  declaring the capability.
- A new helper `st_propagate_capability_labels` runs after the
  call-typing unification in `synth_call`'s default arm,
  `try_bare_call_narrow`, and `synth_ufcs_dispatch`. It pulls
  capability-only labels (today: a hardcoded `Ffi` filter) out of
  the callee's instantiated TyFnT row and adds them to the
  caller's `st.row`. That is the minimum needed to make
  `extern "C" fn` calls demand `/ Ffi` transitively without
  forcing a `/ Console` retrofit across the compiler.

The 318-error retrofit is now the body of a separate follow-up
(`pub_fn_transitive_effect.kai` documents the gap that motivates
it).

## Design decisions and alternatives considered

**Why not propagate every callee row label?** The natural fix is
to walk `callee.ty -> row.labels` and add them to `st.row` via
`st_add_labels` after unify (the helper has been in the code for
months under that comment, unused). The pragmatic blocker is the
compiler itself: helpers like `emit_source_caret_from_src`,
`bang_bad_operand`, `check_exhaustive`, `dump_expr`, etc., have
`: T` (REmpty) signatures and call `eprint` / `diag_error` /
`emit_source_caret_from_src` chains that already declare
`/ Console` (or `/ File`). Pre-fix, the caller's open row tail
absorbed everything; post-fix, the callers fail `check_body_row`.
This is the right diagnosis of a real silent contract inside the
compiler — but fixing 109 helper signatures in the same lane that
fixes #516 inflates blast radius far past what the brief described
and risks subtle drift in selfhost reproduction. The right
sequence is: close the narrow gap now, file a follow-up that
sweeps the compiler's own row annotations, then retract the
capability-only restriction in a third lane.

**Why filter capability labels (Ffi) rather than allowlist
default-handled labels?** Either spelling works; the predicate
is currently a singleton check on `Ffi`. Picking "capability-only"
as the conceptual category keeps `capability_labels_only` /
`only_non_absorbed_labels` clear about why the list is short
(every other builtin gets a default handler from
`emit_main_wrapper`). When a second zero-op capability effect
joins the catalogue, it lands in both predicates together.

**Why not bypass `mask_local_mutable_demand` entirely for
qualified-mutable calls?** Treating the qualified form as a
separate code path drops the mask-when-local optimisation for
qualified calls — but that optimisation IS desirable for the
locally-constructed-Array case (e.g. `let xs = Mutable.array_make(...);
Mutable.array_set(xs, 0, 7)` should still mask `Mutable` because
nothing escapes). Teaching the masking walker to recognise both
spellings restores parity without losing the optimisation.

**Why update `examples/effects/ffi_extern_c_basic.kai` instead of
keeping the regression diff zero?** `main` in that fixture used
the implicit `: Unit` REmpty row to absorb the `puts ... / Ffi`
call. With the new main-row rule that's no longer legal, so the
fixture now declares `/ Console + Ffi` explicitly — which matches
the convention already in
`examples/effects/ffi_pub_axiom_cross_module/main.kai` and is the
right user-facing shape.

## Structural surprises

- `try_bare_call_narrow` (issue #235) and `synth_ufcs_dispatch`
  (issue #205) duplicate the open-row expected pattern that
  `synth_call`'s default arm uses. Each had to be patched
  independently — three identical four-line additions. There is
  an opportunity to factor that pattern into a shared helper, but
  doing it in this lane would have widened the diff without
  closing more silent contracts.
- The compiler's silent absorbance of `Console` / `File` through
  the open row tail surfaced only when transitive propagation was
  tried wholesale; the qualified-op masking + main-row fixes are
  zero-impact on the compiler's own typing because they never
  add labels to non-effect-call sites. The "selfhost
  byte-identical" gate held because the new code paths are
  unreachable from inside the compiler's call graph (no qualified
  `Mutable` writes in compiler.kai; no `extern "C" fn` declared
  there).
- `check_body_row`'s `if fname == "main"` short-circuit is the
  only place where a builtin effect can be absorbed without
  explicit declaration. Filtering `Ffi` out of that absorption
  was a one-line conceptual change that produced the
  `enclosing row: (empty)` diagnostic at the right column for the
  FFI fixture — the existing T5 template already renders the
  Ffi-only filtered row correctly.

## Fixtures added / promoted

- `examples/negative/pub_effect/pub_fn_mutable_unannotated.kai`
- `examples/negative/mutable/mutable_op_no_handler_or_row.kai`
- `examples/negative/mutable/mutable_through_field.kai`
- `examples/negative/mutable/mutable_param_write_pure_row.kai`
- `examples/negative/ffi/ffi_call_no_capability.kai`

Each has its `.err.expected` golden (`error: effect not handled:
<eff>`); `tools/test-negative.sh` now reports 36 PASS / 0 FAIL.

`examples/negative/silent_contract/pub_fn_transitive_effect.kai`
stays in `silent_contract/` per the deferral rationale above;
the directory's README was updated to point at this retro and
to drop the closed fixtures from the issue-#516 row.

## Coverage gaps

- No fixture exercises a chain of THREE functions
  (`leaf -> middle -> top`) with capability effects to verify the
  Ffi propagation is fully transitive (it is — every call site
  re-runs `st_propagate_capability_labels` — but a fixture would
  ratchet the behaviour into tier1). Filed mentally; not blocking.
- No fixture verifies that masking still works for
  locally-constructed Arrays under the qualified form (e.g.
  `let xs = Mutable.array_make(3, 0); Mutable.array_set(xs, 0, 1); xs`
  with a pure row). The masking logic now recognises the
  qualified spelling, so this case continues to mask correctly;
  again, a positive fixture would lock it in.
- No fixture covers `extern "C" fn` called through UFCS
  (`x.puts()`) — the propagation patch covered the path but the
  shape is awkward to construct and not idiomatic.

## Real cost vs estimate

Brief estimated 1-2 days. Actual: ~3h of focused work split as:

- ~45 min understanding the typer's call-site code paths and
  locating `synth_op_call_with_scheme_keys` + `mask_local_mutable_demand`.
- ~30 min writing the first cut (full transitive propagation),
  running selfhost, hitting 318 compiler.kai errors.
- ~60 min reading the failure pattern and deciding to scope down
  to capability-only labels + revert the general transitive path.
- ~30 min on the main-row `Ffi` exception and adjusting
  `examples/effects/ffi_extern_c_basic.kai`.
- ~30 min promoting the five enforced fixtures, updating the
  silent-contract README, writing this retro.

The estimate would have held for the full scope iff the
compiler.kai retrofit were trivial; the structural surprise (318
implied row annotations on stage2 helpers) made the broad fix
incompatible with selfhost byte-identical and forced the
two-stage rollout.

## Follow-ups left for next lanes

- **Transitive propagation for non-capability effects.** The
  general fix (every callee row label flows to the caller) is
  semantically correct per CLAUDE.md Tier 1 #1, blocked on
  annotating the ~109 stage2/compiler.kai helpers that currently
  rely on row-tail absorption. A reasonable cadence is:
  1. mechanical sweep adding `: T / Console` to every helper
     surfaced by the typer's diagnostic (script the patch).
  2. selfhost validation under the relaxed-into-strict propagation.
  3. retract the `capability_labels_only` predicate and use
     `st_add_labels(st, row.labels, …)` unconditionally; remove
     the `only_non_absorbed_labels` filter from `check_body_row`'s
     main arm; pruning is mechanical once the helpers are honest.
- **Promote `pub_fn_transitive_effect.kai`** when the above lane
  closes. The fixture is already in place; the `.err.expected`
  will be `error: effect not handled: Stdout`.
- **Factor the open-row-expected pattern** used in `synth_call`
  default, `try_bare_call_narrow`, and `synth_ufcs_dispatch` into
  one helper. Three nearly-identical blocks per lane changing the
  call typing surface is a maintenance smell.
- **`st_add_labels` is now used.** Until this lane the helper was
  defined and unreferenced (a planted stub from the original
  comment). It is reached from `st_propagate_capability_labels`;
  the comment block above it can drop the "Used by `synth_call`"
  language and instead reference both helpers.

## Notes on the brief's "selfhost byte-identical" gate

The lane shipped selfhost byte-identical. The capability-only
propagation path is unreached during compiler self-compilation
because no compiler.kai helper calls a `/ Ffi`-tagged function;
the `mask_local_mutable_demand` change to recognise the qualified
spelling is unreached because compiler.kai uses bare prelude
forms throughout. The main-row `Ffi` filter affects only programs
whose main body performs FFI without declaring `/ Ffi` — never
the compiler. Selfhost emitted byte-identical C in both backends.
