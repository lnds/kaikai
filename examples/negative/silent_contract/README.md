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
| `pub_fn_mutable_unannotated.kai`, `pub_fn_transitive_effect.kai`, `mutable_op_no_handler_or_row.kai`, `mutable_through_field.kai`, `mutable_param_write_pure_row.kai`, `ffi_call_no_capability.kai` | [#516](https://github.com/lnds/kaikai/issues/516) — qualified-effect calls do not propagate |
| `handle_residual_effect.kai`, `handle_partial_with_other_effect.kai`, `handle_clause_missing_resume.kai`, `handle_clause_wrong_arity.kai`, `main_row_user_effect.kai` | [#517](https://github.com/lnds/kaikai/issues/517) — handle block: residual effects + clause shape not validated |
| `tree_collision_via_import/`, `duplicate_type_decl.kai` | [#518](https://github.com/lnds/kaikai/issues/518) — type-name collision via import + dup decl |

## Migration recipe

When an issue closes:

1. Move the fixture out of `silent_contract/` into the relevant
   enforced directory (e.g. `pub_effect/`, `handle_leak/`,
   `mutable/`, `ffi/`, `shadowing/`).
2. Add a `<name>.err.expected` file with the new diagnostic's
   first line.
3. Run `tools/test-negative.sh` — the fixture should now PASS.
4. Update this README to remove the row.
