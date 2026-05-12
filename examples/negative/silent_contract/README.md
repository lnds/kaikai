# Silent-contract fixtures (#511 audit)

Each `.kai` file in this directory documents a contract that
`docs/design.md`, `docs/effects.md`, `docs/effects-stdlib.md`, or
`docs/effects-impl.md` advertises but stage 2 currently does not
enforce. The fixtures compile cleanly today (or compile + segfault
at runtime); when the underlying gap closes, the fixture migrates
out of this directory into the regular negative-space layout with
a `.err.expected` golden.

The `tools/test-negative.sh` harness skips this subtree on purpose
(via `-not -path '*/silent_contract/*'`) — they are not enforced
contracts yet, so they cannot gate tier1.

## Follow-up issues filed

| Fixture(s) | Issue |
| --- | --- |
| `pub_fn_transitive_effect.kai` | [#516](https://github.com/lnds/kaikai/issues/516) — general transitive-effect propagation through ordinary fn calls (deferred follow-up; see the lane-experience retro for issue #516) |

`pub_fn_mutable_unannotated.kai`, `mutable_op_no_handler_or_row.kai`,
`mutable_through_field.kai`, `mutable_param_write_pure_row.kai`,
and `ffi_call_no_capability.kai` closed under #516 — they live in
their enforced sub-dirs (`pub_effect/`, `mutable/`, `ffi/`) with
`.err.expected` goldens. `pub_fn_transitive_effect.kai` stays here:
closing it requires retrofitting ~109 stage2/compiler.kai helpers
(`diag_*`, `dump_*`, `check_*`, `synth_*`) that currently absorb
non-capability effects through an open row tail; the lane brief
for #516 chose to scope the fix to capability-only labels (`Ffi`),
the qualified-`Mutable`-op masking gap, and `main`'s REmpty/Ffi
exception. See `docs/lane-experience-issue-516-effect-row-propagation.md`.

`handle_residual_effect.kai`, `handle_partial_with_other_effect.kai`,
`handle_clause_missing_resume.kai`, `handle_clause_wrong_arity.kai`,
and `main_row_user_effect.kai` closed under #517 — they now live in
`examples/negative/handle_leak/` with `.err.expected` goldens. See
`docs/lane-experience-issue-517-handle-validation.md`.

## Migration recipe

When an issue closes:

1. Move the fixture out of `silent_contract/` into the relevant
   enforced directory (e.g. `pub_effect/`, `handle_leak/`,
   `mutable/`, `ffi/`, `shadowing/`).
2. Add a `<name>.err.expected` file with the new diagnostic's
   first line.
3. Run `tools/test-negative.sh` — the fixture should now PASS.
4. Update this README to remove the row.
