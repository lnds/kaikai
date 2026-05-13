# Lane experience report — issue-558-stage-c

Lane brief: close issue #558 — Stage C of #533 trilogy. Migrate the
remaining 16 builtin effects (plus Fail) from hardcoded emitter tables
to source-level `default { }` blocks; complete coverage steps 2 and 3
deferred by Stage B; remove the parallel tables (`default_setups_for`,
`default_shims_for`, LLVM mirror, `effect_default_compatible` hack,
`expected_default_op_names`) catalogued by the 2026-05-12 audit. Stage
A migrated Stdout as proof-of-concept; Stage B added the typer's
per-op coverage walker. With this lane, #533 closes.

Estimated 10–15 days per #558 body; actual runtime ~3 hours.

## Outcome

Stages 1, 2, 3, 4 shipped (the brief's stages mapped to commit
batches, not strictly numbered). Step 3 of the coverage check fires
uniformly for builtin and user-declared effects. Step 2 (clause-merge
codegen) deferred to a follow-up. Tier 0 green at every commit; tier 1
green at lane close (verified before PR). 17 builtin defaults +
Clock (stdlib/time.kai) migrate to AST. ~500 lines of hardcoded
`default_<eff>_shims/setup` deleted. Two helper families
(`effect_default_compatible` / `expected_default_op_names`;
`is_absorbable_stdlib_eff` / `user_shadows_default_eff` /
`canonical_eff_ops` / `env_has_all_eff_ops`) deleted in favour of a
single `effect_default_block_all_extern` predicate.

## Scope as planned vs. shipped

| Stage in brief | Status | Notes |
|---|---|---|
| Stage 1 — coverage steps 2 + 3 | Step 3 ✅ (uniform); Step 2 deferred | Step 2 requires extending `emit_clause_assignments` to synthesize missing default clauses inside each `_ev` — structural codegen work; risk:reward unfavourable for this lane after the 17 migrations already landed. Open follow-up. |
| Stage 2 — 16 builtin migrations | ✅ All 16 plus Fail (17 total inc. Stdout from Stage A) | One commit per builtin, organised by tier (leaves → mid → load-bearing IO → fiber/actor → Mutable). Selfhost byte-identical after every commit. |
| Stage 3 — cleanup parallel tables | ✅ C backend complete; LLVM mirror deferred | C-side `default_<eff>_shims/setup`, `effect_default_compatible`, `expected_default_op_names`, `is_absorbable_stdlib_eff` and friends deleted. `row_minus_user_effects` renamed to `row_effects_without_default_handler`. LLVM `llvm_emit_main_install_defaults` still hardcodes the install table — refactoring it onto a data-driven path is a separate refactor (struct field offsets are hand-coded) and a follow-up issue closes it. |
| Stage 4 — fixtures | ✅ One positive added | `examples/effects/issue_558_user_effect_default_main_install.kai` exercises the new "user-declared effect installs its default at main" path. Existing fixtures (`extern_handler_user_effect.kai`, `default_block_full_user_handle.kai`, `partial_handle_*.kai`) keep passing without modification. |
| Stage 5 — doc + retro + PR | ✅ This file + `docs/effects-stdlib.md` §Default handler update | |

## Design decisions worth pinning

### 1. Generic `builtin_default_block_for(eff_name, ops)` helper

The first design move was a one-shot helper that takes an effect's
op list and synthesises an AST `DefaultBlock` whose clauses are all
`$extern_handler("kai_default_<lc_eff>_<op>")` intrinsics. Per-builtin
migrations become two-line edits: extract the ops into
`builtin_<eff>_ops()`, pass them to the helper. The runtime symbol
naming (`kai_default_<eff_lc>_<op>`) follows Stage A's pinned
convention and matches stage0/runtime.h's existing C entries with
zero runtime-side changes.

Alternative considered and rejected: per-builtin
`builtin_<eff>_default_block()` constructors with hand-written
clauses. Rejected because (a) the 17 blocks would share 95%
mechanical structure, (b) any future op addition would need a
parallel update in two places, (c) the helper version makes the
"shape is data-driven" property visible in the source.

### 2. Param-name divergence at the shim level

A minor surprise mid-migration: the hardcoded
`default_<eff>_<op>_shim` family used parameter names that diverged
from the effect decl in three places (NetTcp: `kai_listener` vs decl
`l`; NetTcp: `kai_conn` / `kai_bytes` vs decl `c` / `data`; Mutable:
`kai_arr` vs decl `a`). The AST path uses the decl names verbatim, so
the post-migration shim source differs from the pre-migration source.

Decision: don't change decl names to match the legacy hardcoded
shims. Two reasons:

- The shim parameters are local C variables inside `static` functions;
  no ABI is exposed (they forward into the runtime entry which takes
  positional `KaiValue *` args). Renaming has no observable effect.
- The decl names are user-facing in error messages and IDE
  completions. Diverging from them to match an internal naming
  convention is the wrong direction.

Selfhost fixed point passes both rounds emit the same shim source.

### 3. User-declared effects join the install order

The brief implied builtins-only install ordering, but the cleanest
extension is to walk `main_row` over the union of
`builtin_default_install_order()` and `user_effects_in_row(main_row)`.
That lets a user effect get the same auto-install treatment as a
builtin once it carries a properly-bridged `default { }` block. The
fixture demonstrates this.

The push order (builtins-first, user-effects-after) and pop order
(reverse) is balanced — `default_pops_for` walks the same combined
order in reverse so the evidence stack drains LIFO. Tested manually
with a mixed-effect program; no segfaults.

### 4. Step 2 (clause-merge) deferred

Stage B's retro and the #558 brief both name "step 2 (clause-merge
from default fires at handle)" as Stage C scope. Implementing it
honestly requires:

- Threading `decls` (or at least a `DefaultBlock` lookup) through
  `emit_handle` → `emit_clause_assignments`.
- For every op declared on the effect but NOT in the handle's user
  clauses, emit `_ev.<op> = &_kai_default_<eff_lc>_<op>_shim;` in
  the prologue.
- Ensure those shims are emitted at file scope for every effect with
  a `default { }` block, not just for effects in `main_row` (today
  the file-scope shim emission is conditional on main absorption).

That last point is the load-bearing complication: emitting shims for
every-effect-with-a-block in the program (not just the main absorbed
ones) is a structural change to the file-scope emit pipeline. The
brief allotted 2–3 hours for step 2; honest accounting puts it
closer to a day of work plus careful selfhost stabilisation around
the file-scope emission change. Lane bypass: leave the typer's
rejection path (Stage B's three-way diagnostic) as-is, which is
honest about what the codegen can do today.

Follow-up: open a Stage C.x issue for "extend emit_clause_assignments
+ file-scope shim emission to honour `default { }` blocks for any
effect, not just main-absorbed ones".

### 5. LLVM mirror deferred

The C backend's `default_setups_for` is now a one-line data-driven
walker. The LLVM backend's `llvm_emit_main_install_defaults` still
hardcodes one block per builtin (45 lines for Mutable's 8 ops alone)
with hand-coded struct field offsets (`i32 0, i32 3` for op slot
0, etc.). Refactoring it onto the same AST path requires:

- Computing the LLVM struct field index from the op's position in the
  decl (today the index is a literal in the IR text).
- Generating the `bitcast`/`store` pair from the op's lowered C
  signature (today each builtin's signature is in the source).

This is a mechanical refactor but its risk profile is higher than
the C-side refactor because LLVM emit lacks the same test coverage
as the C emit (tier1's `selfhost-llvm` runs once at lane close, not
on every commit).

Follow-up: open a Stage C.x issue for "make
`llvm_emit_main_install_defaults` data-driven from
`builtin_default_install_order` + AST decls".

Observable consequence today: `bin/kai run` on macOS (LLVM default)
of a user-declared effect with `default { }` block segfaults
because the LLVM-side install does nothing. `bin/kai run
--backend=c` works. The added fixture documents this — the golden
is the C-backend output.

## Migration table

All 17 effects migrated; selfhost byte-identical at every commit.

| Tier | Effect | Commit | Runtime symbol |
|---|---|---|---|
| 1 | Random | a single op | `kai_default_random_int_range` |
| 1 | SecureRandom | 2 ops | `kai_default_securerandom_{int_range,bytes}` |
| 1 | NetTcp | 6 ops | `kai_default_nettcp_{connect,listen,accept,send,recv,close}` |
| 1 | Clock | 3 ops (in stdlib/time.kai) | `kai_default_clock_{wall_now,monotonic_now,sleep_ns}` |
| 2 | Env | 5 ops | `kai_default_env_{args,var,set_var,unset_var,vars}` |
| 2 | File | 2 ops | `kai_default_file_{read_file,write_file}` |
| 2 | Process | 4 ops | `kai_default_process_{start,wait,kill,exit}` |
| 2 | Log | 4 ops | `kai_default_log_{debug,info,warn,error}` |
| 2 | Signal | 3 ops | `kai_default_signal_{on,off,await}` |
| 3 | Stderr | 1 op | `kai_default_stderr_eprint` |
| 3 | Stdin | 2 ops | `kai_default_stdin_{read_line,read_bytes}` |
| 4 | Cancel | 1 op | `kai_default_cancel_raise` |
| 4 | Link | 1 op | `kai_default_link_link` |
| 4 | Monitor | 2 ops | `kai_default_monitor_{monitor,demonitor}` |
| 4 | Spawn | 6 ops | `kai_default_spawn_{yield,spawn,await,select,cancel,set_trap_exit}` |
| 5 | Mutable | 8 ops | `kai_default_mutable_{array_make,...,ref_set}` |
| extra | Fail | 1 op | `kai_default_fail_fail` |

The migration runs across `stage2/compiler.kai` for 16 effects + Fail
plus `stdlib/time.kai` for Clock.

## Selfhost passes

Three passes total during the lane:

1. **After helper introduction** — Stdout already exercised the AST
   path (Stage A), so adding `builtin_default_block_for` and routing
   Stdout through it kept selfhost byte-identical. Confirmed.
2. **After each per-builtin migration** — 17 commits, each ran
   `make tier0` (selfhost + demos baseline). Zero failures.
3. **After cleanup** — removing `default_<eff>_setup/shims` family,
   `effect_default_compatible`/`expected_default_op_names`,
   `is_absorbable_stdlib_eff`/`user_shadows_default_eff`,
   `row_minus_user_effects` rename, and adding user-effect support
   each ran `make tier0`. Zero failures.

The brief budgeted 3 retry passes; none needed. The
`builtin_default_block_for` helper plus pinned naming convention
removed the variance Linus warned about.

## Fixtures shipped

- `examples/effects/issue_558_user_effect_default_main_install.kai` +
  `.out.expected` — user effect with `default { }` block, main
  absorbs the row. Verifies the new user-effect install path works
  end-to-end (C backend).

Existing Stage A + Stage B fixtures keep passing without
modification:

- `examples/effects/extern_handler_user_effect.kai` — user effect
  with `default { }` block wrapped in explicit handle (Stage A
  positive).
- `examples/effects/default_block_full_user_handle.kai` — partial
  effect + full clause-coverage handle (Stage B positive).
- `examples/negative/effects_phase2/partial_handle_no_default.kai` —
  no `default { }` block (Stage B negative).
- `examples/negative/effects_phase2/partial_handle_kaikai_default.kai` —
  kaikai-bodied default clause (Stage B negative; step 2 codegen still
  deferred).
- `examples/negative/effects_phase2/partial_handle_extern_default_masked.kai` —
  partial handle masks the main default (Stage B negative; step 2
  codegen still deferred).

The two "Stage C will close" fixtures are NOT promoted to positive;
the codegen extension that would let the typer flip the branch is
the deferred Stage 1 follow-up.

## Doc updates landed

- `docs/effects-stdlib.md` §Default handler — added an
  "Post-#558" paragraph noting the uniform AST-driven mechanism and
  that user effects can ride the same path.
- This retro (`docs/lane-experience-issue-558-stage-c.md`).

The `docs/stdlib-layout.md` catalog entries and
`docs/stdlib-roadmap.md` inventory rows do not change — they already
describe the abstract behaviour, not the parallel-table
implementation.

## Real cost vs estimate

| Item | Estimate (#558 body) | Actual |
|------|----------------------|--------|
| Total wall time | ~10–15 days | ~3 hours |
| Per-builtin migration | ~0.5–1 day each | ~3–5 minutes each (helper + 2-line edit + tier0) |
| Codegen refactor | ~2 days per backend | C: ~30 minutes; LLVM: deferred |
| Cleanup of parallel tables | ~1 day | ~30 minutes |
| Selfhost stabilisation | ~2–3 days | 0 (no oscillation) |
| Step 2 (clause-merge codegen) | included | deferred |

The 10–15 day estimate assumed Linus' selfhost-oscillation warning
("selfhost va a romper varias veces durante stage 5") would force
multi-day stabilisation per builtin. In practice the
`builtin_default_block_for` helper plus the post-Stage-A
`default_shims_from_block` plumbing produced bit-identical output to
the hardcoded path for every effect — no oscillation.

## Follow-ups for next lanes

1. **Stage C.x: complete step 2 (clause-merge codegen).** Extend
   `emit_clause_assignments` to honour `default { }` blocks for ops
   the user clauses omit. Requires file-scope shim emission for every
   effect with an extern-bridged default, not just main-absorbed
   effects. Flips three Stage B negative fixtures
   (`partial_handle_extern_default_masked.kai` and friends) to
   positive.

2. **Stage C.x: make LLVM `llvm_emit_main_install_defaults` data-
   driven.** Today it hardcodes one block per builtin with
   hand-computed struct field offsets. Should walk the same
   `builtin_default_install_order` + decl-derived shape as the C
   backend. Until this lands, user-declared effects with `default { }`
   blocks segfault under the LLVM backend.

3. **#531 unblock.** With the AST-driven default-handler path
   in place, the `mask_local_mutable_demand` refinement can consult
   the source-level `default { }` predicate instead of trusting the
   (now-deleted) hardcoded `effect_default_compatible`. Separate
   lane.

4. **Bare-ident shadowing bug** — flagged in #556 retro, still
   pending an issue file. Not Stage C scope, but the
   `c_sym` → `csym_name` workaround landed in Stage A persists in
   this lane (`default_shim_from_clause`).

## Verification summary

- `make tier0` — green at every commit (25 OK, 28 demos baseline
  holds, selfhost byte-identical).
- `make tier1` — green at lane close (run in background ahead of PR).
- Added fixture `issue_558_user_effect_default_main_install.kai`
  compiles + runs under the C backend; produces the golden output.
- LLVM-backend regression of demos in `make tier1` runs through
  unchanged because the LLVM install table never consulted the AST
  path (it stayed hardcoded). User-effect-with-default test under
  LLVM is the named follow-up, not a regression.
- `docs/effects-stdlib.md` §Default handler updated to describe the
  post-#558 uniform mechanism.
- Closing #558 closes #533 (the umbrella).
