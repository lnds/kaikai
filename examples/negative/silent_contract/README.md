# Silent-contract fixtures (#511, #520 audits)

Each `.kai` file in this directory documents a contract that
`docs/design.md`, `docs/effects.md`, `docs/effects-stdlib.md`,
`docs/effects-impl.md`, `docs/protocols.md`, or the module-layer
spec advertises but stage 2 currently does not enforce. The
fixtures compile cleanly today (or compile + segfault at
runtime); when the underlying gap closes, the fixture migrates
out of this directory into the regular negative-space layout
with a `.err.expected` golden.

The `tools/test-negative.sh` harness skips this subtree on
purpose (via `-not -path '*/silent_contract/*'`) — they are
not enforced contracts yet, so they cannot gate tier1.

## Follow-up issues filed

### Phase 1 (#511)

| Fixture(s) | Issue |
| --- | --- |
| `pub_fn_transitive_effect.kai` | [#516](https://github.com/lnds/kaikai/issues/516) — general transitive-effect propagation through ordinary fn calls (deferred follow-up; see lane retro for #516) |

`pub_fn_mutable_unannotated.kai`, `mutable_op_no_handler_or_row.kai`,
`mutable_through_field.kai`, `mutable_param_write_pure_row.kai`,
and `ffi_call_no_capability.kai` closed under #516 — they live in
their enforced sub-dirs (`pub_effect/`, `mutable/`, `ffi/`) with
`.err.expected` goldens. `pub_fn_transitive_effect.kai` stays here:
closing it requires retrofitting ~109 stage2/compiler.kai helpers
that currently absorb non-capability effects through an open row
tail. See `docs/lane-experience-issue-516-effect-row-propagation.md`.

`handle_residual_effect.kai`, `handle_partial_with_other_effect.kai`,
`handle_clause_missing_resume.kai`, `handle_clause_wrong_arity.kai`,
and `main_row_user_effect.kai` closed under #517 — they now live in
`examples/negative/handle_leak/` with `.err.expected` goldens. See
`docs/lane-experience-issue-517-handle-validation.md`.

### Phase 2 (#520)

| Fixture(s) | Issue |
| --- | --- |
| ~~`pattern_duplicate_binding.kai`, `pattern_duplicate_variant_arm.kai`, `pattern_duplicate_literal_arm.kai`~~, `unbound_tyvar_in_signature.kai` | [#534](https://github.com/lnds/kaikai/issues/534) — pattern checker + unbound tyvar gaps (partial close: shapes 1-3 live in `examples/negative/patterns/`; shape 4 / unbound tyvar still open) |
| ~~`impl_missing_required_method.kai`, `impl_method_signature_mismatch.kai`, `impl_method_arity_mismatch.kai`~~ | [#535](https://github.com/lnds/kaikai/issues/535) — protocol impl validation (closed: fixtures live in `examples/negative/protocols/`) |
| ~~`extern_missing_ffi_capability.kai`, `extern_array_kaikai_type.kai`~~ | [#536](https://github.com/lnds/kaikai/issues/536) — FFI surface validation (closed: fixtures live in `examples/negative/ffi/`) |
| `program_name_no_row.kai` | [#537](https://github.com/lnds/kaikai/issues/537) — prelude effect mapping (partial close: `args` and `exit` now mapped; `program_name` stays here per issue #127, which routes argv[0] through the `kai_g_argv` runtime snapshot — pure read of process-wide constant state, not handler-mediated) |
| ~~`import_cycle/`~~ | [#538](https://github.com/lnds/kaikai/issues/538) — module system gaps (closed: shape 1 fixture moved to `examples/negative/modules/import_cycle/`; shapes 2+3 closed in #542) |
| `spawn_qualified_no_row.kai` | [#531](https://github.com/lnds/kaikai/issues/531) — qualified-call masking (existing family) |

Closed as wontfix: #539 (handler missing `return`). The omission is
intentional sugar for `return(x) -> x`; see
`docs/decisions/handler-return-clause-optional-2026-05-12.md` and
`docs/effects.md` lines 311–313. Fixture removed.

## Migration recipe

When an issue closes:

1. Move the fixture out of `silent_contract/` into the relevant
   enforced directory (e.g. `patterns/`, `protocols/`, `ffi/`,
   `effects_phase2/`, `modules/`, `handle_leak/`,
   `type_invariants/`).
2. Add a `<name>.err.expected` file with the new diagnostic's
   first line (or a `<name>.diag.expected` for multi-line
   body-quality assertions per #520 category 7).
3. Run `tools/test-negative.sh` — the fixture should now PASS.
4. Update this README to remove the row.
