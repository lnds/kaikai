# Lane experience — prelude effect-by-name cleanup (§1.1)

Date: 2026-06-04
Scope: remove the `prelude_effect_for` / `add_prelude_effect_label` name-keyed
effect-attribution table from the stage2 typer, moving each builtin's effect
into its scheme row where its type already lives.

## Scope as planned vs as shipped

Planned (from a smell audit): generalize the `prelude_effect_for` table
(§1.1) and the `is_mutable_array_write_name` table (§1.2), both flagged as
"effect/mutability inferred from a cabled table instead of the signature".

Shipped: §1.1 only. §1.2 was **deliberately not touched** after an
architecture review concluded it is an intrinsic boundary, not a smell — see
below. So the lane narrowed by one item, on a soundness argument, not on
effort.

## The load-bearing discovery

The audit doc claimed §1.1 was "blocked on a stdlib-declaration
effect-attribution mechanism that does not exist". That was false. The
mechanism already existed:

- The function type `TyFnT([Ty], Ty, Row)` already has an effect-row slot.
- `add_prelude_sigs` already registered every builtin's *type* — but built it
  with `fn_ty(ps, ret) = TyFnT(ps, ret, inf_row_empty())`, hardcoding an
  **empty** row. The effect lived separately in `prelude_effect_for`,
  attributed at the call site by `add_prelude_effect_label` inside
  `synth_call`. Type in one table, effect in another, synced by hand.
- The call-inference path already propagates a callee's row into the caller:
  `collect_call_labels` walks typed `ECall`s and accumulates the callee
  `TyFnT`'s `row.labels`; `st_propagate_capability_labels` (#528) propagates
  *all* capability labels per `ECall`, not just Ffi. `map`/`filter` already
  ride this path via `row_with_tail`.

So the fix was not to invent a mechanism — it was to stop bypassing the one
that exists. New helper `fn_ty_eff(ps, ret, effs)` stamps a closed effect row
onto the `TyFnT`; each builtin in `add_prelude_sigs` now declares its own
effect; `prelude_effect_for` + `add_prelude_effect_label` + the `synth_call`
shim were deleted. Net −30 lines in infer.kai.

The lesson: an audit doc's "blocked on missing piece" is a hypothesis to
verify in code, not a fact. Reading `add_prelude_sigs` next to
`collect_call_labels` made the real shape obvious in ten minutes.

## Design decisions

- **Kept `let st_p = st`** at the old shim site instead of renaming `st_p` back
  to `st` at its ~6 downstream uses. Minimizes the diff; the alias is inert.
- **`exit` rides Process but `panic` stays effect-free.** Both are
  bottom-polymorphic (`scheme([0], fn_ty_eff([TyInt], tv(0), ["Process"]))`).
  `panic` is an audited runtime escape (Fail/abort), not a Process op, so it
  was never in `prelude_effect_for` and stays out of the scheme.
- **`array_set`/`array_grow` carry Mutable in their scheme.** The provenance-
  masking pass (#251/#252) gates on `list_has_label(st.row, "Mutable")` and
  then walks the AST by name — it is agnostic to *how* the label entered the
  row, so moving the label source from the shim to the scheme is behaviour-
  preserving. Confirmed by the four existing Mutable fixtures all passing,
  including the negative `array_mutable_required` (parameter mutation rejected).

## §1.2 — why it was left

`is_mutable_array_write_name` recognizes the finite set of observable-write ops
(`array_set`, `array_grow`, their `Mutable.*` dispatch keys, `Mutable.ref_set`)
for the masking flow analysis. This is flow-analysis information (the pass needs
to know the first arg is the mutated cell), not type information. It is the
intrinsic definition of "what counts as an observable Mutable write", with no
general mechanism to defer to. Generalizing it costs design and gains no
soundness. Left as-is, reclassified in the smell doc from §1 (accidental) to §4
(intrinsic boundary).

## Fixtures

Added `examples/effects/prelude_effect_via_scheme.kai` (+ golden), wired into
`stage2/Makefile` `test-effects`. It exercises the propagation path the shim
used to cover: a non-main helper calls bare `print` (Stdout) and the effect
flows `emit` → `emit_twice` → `main` through the scheme row. If the scheme did
not carry the effect, `check_body_row` would fail to require the declaration.

Existing coverage that already pinned the risk surface (all still green):
`array_local_masks_mutable`, `array_mutable_explicit`, `array_read_only_no_mutable`,
`array_mutable_required` (negative), and the `m12_8_y_phase4_*` row-propagation set.

## Cost vs estimate

Smaller than expected once the discovery landed: the change is mechanical
(21 scheme edits + 1 helper + 2 deletions) once you trust the propagation path.
The risk was entirely in the Mutable-masking interaction, retired by the
existing fixtures plus the new one. Selfhost stayed byte-deterministic, the
strongest signal that effect inference is unchanged across the whole stdlib.

## Follow-ups

None required. The smell doc's §1.1 is closed; §1.3 (Map indexing protocol)
remains open as a separate typer design lane, untouched here.
