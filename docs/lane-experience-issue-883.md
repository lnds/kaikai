# Lane experience — issue-883 homonymous effect codegen

## Objective metrics

- Compiler diff: 4 lines in `emit_c.kai`, 24 added in `emit_shared.kai`
  (one new shared helper + a robustness tweak to an existing one).
- Fixtures: no new `.kai` file. The repro IS `examples/quickstart/
  04_effect.kai` (already shipped, already wired into the native-parity
  ratchet's quickstart shard); the stdlib-`Log`-default side is
  `examples/effects/issue_141_log_default.kai` (already wired into
  `make test-log-asan`). Both sides of the disambiguation are proven by
  pre-existing fixtures — the lane's job was to make them BOTH green at
  once, which they had never been since the #558 migration.
- Backends: C-direct + in-process libLLVM native. Both fixed; one shared
  identity helper drives both paths.

## The bug

A program that declares its own `effect Log { log(msg) }` (one op, no
`default { }`) coexists in the same binary with the stdlib
`effect Log { debug/info/warn/error default { … } }` (the auto-loaded
core). The C backend emitted:

```
out.c: error: no member named 'debug' in 'struct EvLog'
  (assigns: _kai_default_ev_log.debug = &_kai_default_log_debug_shim; …)
```

Both `Ev` structs were emitted correctly — `EvLog` (user, mo=None, field
`.log`) and `Eveffects__Log` (stdlib, mo=Some("effects"), fields
`.debug/.info/.warn/.error`). The defect was the default-handler
install: it minted the evidence static and shims against the WRONG
struct. The stdlib `Log`'s `default { }` block supplied the
`.debug/.info/.warn/.error` assigns, but the struct name was minted from
the user's `Log` identity, so the assigns landed on `EvLog` (which has
only `.log`).

## Root cause

`ev_name(name, mo)` already module-qualifies the struct, so the two
structs are distinct. But the default-install minting
(`default_shims_from_block` / `default_setups_from_block`, emit_c.kai)
resolved the `mo` via `eff_mo`, which on a homonym clash **prefers the
root-origin (None) decl** — the shadow the resolver chose for the row.
That is correct for the user's perform/handle path, but WRONG for the
default-install path: the `default { }` block being installed belongs to
the stdlib decl, so its shims must target the stdlib struct.

The two resolutions had drifted apart: `find_effect_default_block` found
the stdlib decl's block, while `eff_mo` named the user decl's struct.

## The fix — one identity key for the whole default-install path

Added `default_block_owner_mo(eff_name, decls)` in emit_shared.kai: it
returns the `mo` of the effect decl that ACTUALLY owns the resolved
`default { }` block (the first homonym whose `defblk` is `Some`). The C
default-install now mints `ev_name(eff_name, default_block_owner_mo(…))`
instead of `ev_name(eff_name, eff_mo(…))`, so the static, the shims, and
the struct are all keyed on the owner of the block — they stay in
lockstep and target `Eveffects__Log`.

Also hardened `find_effect_default_block` itself: it used to return the
first homonym's `defblk` even when that was `None`, so the result
depended on decl order (user-first → wrongly `None`, stdlib-first →
correct). It now skips a `None` homonym and keeps scanning for the decl
that declares the block. This makes `default_block_owner_mo` and every
other consumer (`effect_default_block_all_extern`, the KIR
`kir_default_for_effect`) order-independent.

## Native backend

The native path needed NO new code beyond the shared helper. It does not
emit typed C structs — `emit_native_def.kai` writes a generic
`[16 x ptr]` Ev blob and stores op fn-ptrs by FIELD INDEX
(`kaix_ev_set_op(ev, idx, fn)`), so there is no "no member named debug"
shape to break. Its `[KDefault]` set comes from `kir_default_handlers`,
which resolves each block through the SAME `find_effect_default_block`.
The order-robustness tweak to that function is what the native side
inherits; the surface-name globals (`_kai_default_ev_log`) are fine
because only one `Log` carries a default block, so only one global is
minted. 04_effect builds + runs identically on native.

## Relationship to #308

Issue #308 was the same symptom, fixed in 2026-05-06 by a DIFFERENT
mechanism: `row_minus_user_effects` filtered `main_row` by op-name shape
compatibility (`effect_default_compatible`) before the emit sites read
it, dropping the default install whenever a user decl's op names diverged
from the runtime contract. The #558 migration to source-level
`default { }` blocks replaced that gate with
`cmt_row_effects_without_default_handler` /
`effect_default_block_all_extern`, which checks only "is the block all
`$extern_handler`" — NOT op-name shape. That silently dropped #308's
protection, and 04_effect regressed. The parity ratchet's flaky-recheck
masked it until #886 hardened the ratchet, surfacing it as a real RED.

This lane's fix is the structurally correct one: instead of suppressing
the stdlib default install when a homonym exists, it lets BOTH installs
proceed against their OWN structs. The user `Log` keeps its handler; the
stdlib `Log` default is pushed at the bottom of the evidence stack and
correctly masked by the user's `handle` frame (verified: 04_effect prints
`[INFO] …` from the user handler, not the stdlib stderr default).

## Empirical verification

```
KAI_BACKEND=c      bin/kai build examples/quickstart/04_effect.kai && run
  → [INFO] hello, kaikai / [INFO] hello, world
KAI_BACKEND=native bin/kai build examples/quickstart/04_effect.kai && run
  → [INFO] hello, kaikai / [INFO] hello, world

issue_141_log_default (stdlib Log default, debug/info/warn/error):
  static Eveffects__Log _kai_default_ev_log;  ✓ correct struct
  stderr: [<ISO8601Z>] DEBUG/INFO/WARN/ERROR default-{d,i,w,e}  ✓
```

Serial native-parity ratchet (`BACKEND_PARITY_JOBS=1`) over
`examples/quickstart`: pass=5 fail=0, 0 gaps. 04_effect now passes — it
was one of the deterministic fails #886 exposed.

## Friction points

- The combined "one binary declares `effect Log` AND `import log`"
  fixture does NOT compile — the typer rejects it: the stdlib `log.info`
  is `pub` and exposes effect `Log`, which now resolves to the user's
  non-pub `Log`. That is an orthogonal name-resolution constraint, not a
  codegen concern. The two-separate-fixtures coverage (user side via
  04_effect, stdlib side via issue_141) is the right factoring; a
  single-binary homonym with both effects ACTIVE is blocked at the typer
  by pub-exposure rules, so codegen never sees it.
- Both `Ev` structs already coexist in 04_effect's generated C
  (`Eveffects__Log` at the stdlib decl, `EvLog` at the user decl) — the
  bug was never "only one struct emitted", it was "the install targeted
  the wrong one". Reading the generated C directly (`grep _kai_default_
  ev_log\\.`) located it in one pass.

## Limitations

- Same as #308 carried forward: no shadow diagnostic, op compatibility
  is identity/module-keyed (correct) rather than shape-keyed. A user who
  shadows a defaulted stdlib effect still silently loses the ability to
  reach the stdlib one by qualified name in the same module.
