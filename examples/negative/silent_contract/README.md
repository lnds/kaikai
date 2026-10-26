# Silent-contract fixtures

Each `.kai` file in this directory documents a contract that
`docs/design.md`, `docs/effects.md`, `docs/effects-stdlib.md`,
`docs/effects-impl.md`, `docs/protocols.md`, or the module-layer
spec advertises but stage 2 does not enforce: the fixture compiles
cleanly. When the gap closes, the fixture moves out of this directory
into the regular negative-space layout with a `.err.expected` golden.

The `tools/test-negative.sh` harness skips this subtree on purpose
(via `-not -path '*/silent_contract/*'`) — these are not enforced
contracts, so they cannot gate tier 1.

| Fixture | Gap |
| --- | --- |
| `program_name_no_row.kai` | `program_name()` reads process state, yet a `main` with a pure declared row calls it with no effect in its row. |
| `spawn_qualified_no_row.kai` | `Spawn.spawn(worker)` from a `main` whose declared row omits `Spawn`. The same call inside an ordinary function is rejected (`effect not handled: Spawn`); only `main` accepts it. |

## Migration recipe

When a gap closes:

1. Move the fixture out of `silent_contract/` into the relevant
   enforced directory (e.g. `patterns/`, `protocols/`, `ffi/`,
   `effects_phase2/`, `modules/`, `handle_leak/`,
   `type_invariants/`).
2. Add a `<name>.err.expected` file with the new diagnostic's
   first line (or a `<name>.diag.expected` for multi-line
   body-quality assertions).
3. Run `tools/test-negative.sh` — the fixture should now PASS.
4. Update this README to remove the row.
