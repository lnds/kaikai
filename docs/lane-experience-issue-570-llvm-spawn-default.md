# Lane retro — issue #570: LLVM backend installs Spawn default handler

## Scope as planned vs scope as shipped

**Planned (brief).** Close issue #570 by porting the missing Spawn
default-handler installation from the C backend to the LLVM backend.
The lane #570+#571 retro had already pinned the diagnosis: the LLVM
emitter's `kai_main_install_defaults` did not push a default Spawn
evidence node, so `kaix_evidence_lookup_handler("Spawn")` returned
NULL on the first op call (typically `spawn_actor` invoked under
`with_mailbox`) and SIGSEGV'd on the null deref.

**Shipped.** The diagnosis held verbatim. The fix is small and
mechanical: add the Spawn case to three sites in
`stage2/compiler.kai` (declaration globals, install body, teardown
pop) and add six wrapper symbols to `stage0/runtime_llvm.c`. Plus the
matching `declare` lines in the LLVM IR header so clang accepts the
references. One regression fixture lands in `examples/llvm/` and is
wired into `tools/test-llvm-driver.sh`.

The lane stayed strictly in scope. Three categories of work that
the brief listed as possible escalations did **not** apply:

- The cause was not "shape of evidence_node tree" — the runtime
  contract is unchanged.
- No existing test that passed pre-fix regressed.
- The lifter / dispatcher / op offsets all worked correctly already
  — the only missing piece was the `kai_main_install_defaults`
  body, exactly as the previous lane's retro predicted.

## Reproduction confirmation

The 18-line repro from the issue body (verbatim):

```kai
import actor
import spawn

type Cmd = Run | Stop

fn body() : Unit / Actor[Cmd] = {
  match Actor.receive() {
    Run  -> ()
    Stop -> ()
  }
}

fn main() : Int / Spawn = {
  with_mailbox {
    let pid : Pid[Cmd] = spawn_actor(body)
    Actor.send(pid, Stop)
  }
  0
}
```

- **Pre-fix (LLVM backend):** `bin/kai build repro.kai && ./repro`
  → exit 139 (SIGSEGV at `ldr x1, [x0]` inside `kai_actor__spawn_actor`).
- **Post-fix (LLVM backend):** clean exit 0.
- **C backend (both pre- and post-fix):** clean exit 0 — was never
  affected because `default_setups_for` in the C path has always
  installed Spawn unconditionally.

## Diff between C backend and LLVM backend in `kai_main_install_defaults`

The C backend's main wrapper installs every effect named in
`builtin_default_install_order()` (18 names: Stdout, Stderr, Fail,
Stdin, Env, File, Clock, Random, SecureRandom, NetTcp, Signal,
Process, Log, Mutable, Cancel, Link, Monitor, Spawn) when present in
main's row. It derives the install body from each effect's `default
{ }` block at AST level — see `default_setups_from_block` and
`default_shims_from_block` at stage2/compiler.kai:17745 and 17736.

The LLVM backend's `llvm_emit_main_install_defaults`
(stage2/compiler.kai:45841) is hand-written, one `if list_has(...)`
branch per effect, and the pre-fix version handled only nine: Stdout,
Stderr, Fail, Random, SecureRandom, Clock, NetTcp, Log, Mutable. The
remaining nine (Stdin, Env, File, Signal, Process, Cancel, Link,
Monitor, Spawn) are silently dropped — a program that depends on any
of them will crash at the first op call.

This lane closes the Spawn gap only. The eight other unhandled
effects are identical in shape (each needs a `kaix_default_<eff>_<op>`
wrapper plus an install branch). Filing them as a single follow-up is
left for the next lane to scope; the runtime symbols already exist in
`stage0/runtime.h` for most of them, so each one is on the order of
~30 lines of mechanical addition.

The structural cleanup — making the LLVM backend AST-derive its
install body the way the C backend does — is a larger refactor and
is not in scope for a #570-style hotfix.

## Fix shape

Three sites in `stage2/compiler.kai`:

1. **`llvm_default_builtin_decls`** (around line 45831) — add the
   `@_kai_default_ev_spawn`, `@_kai_default_node_spawn`, and
   `@_kai_default_spawn_name` globals when `Spawn` is in `main_row`.
   The `%EvSpawn` struct itself is already emitted by
   `llvm_effect_struct_decls(decls)` since Spawn is part of the
   builtin effect inject.
2. **`llvm_emit_main_install_defaults`** (around line 45841) — add a
   `spawn_` branch storing the six op fn-ptrs at slots 3..8 of
   `%EvSpawn` (matching `builtin_spawn_ops()`'s declaration order:
   yield, spawn, await, select, cancel, set_trap_exit) and pushing
   the evidence node. Spawn is added LAST in the concat_all so it
   matches `builtin_default_install_order`'s "innermost" placement.
3. **`llvm_emit_main_teardown_defaults`** (around line 46013) — add
   the matching `kaix_evidence_pop()` FIRST (LIFO from the install
   order).

Plus the LLVM header (`llvm_header`, around line 43670) gets six
`declare %KaiValue* @kaix_default_spawn_*(...)` lines so the install
body's references resolve when clang validates the IR.

Six wrapper functions land in `stage0/runtime_llvm.c` (around line
544): `kaix_default_spawn_yield`, `_spawn`, `_await`, `_select`,
`_cancel`, `_set_trap_exit`. Each is a one-line forwarder to the
matching `kai_default_spawn_*` static defined in `stage0/runtime.h`.
The wrapper pattern is identical to every other `kaix_default_*`
already in `runtime_llvm.c`.

Op slot order verified against the emitted IR's struct definition:
`%EvSpawn = type { i64, i8*, %KaiValue*, %KaiValue* (%EvSpawn*,
%KaiCont*)*, ... }` — first three fields are the standard
`hid / env / state` prefix, then ops in declaration order.

## Why option "audit all 9 missing effects" was not the right shape

The brief explicitly framed #570 as a hotfix to unblock ahu (12 of
13 fixtures crashing). Bundling the eight other missing builtins
(Stdin, Env, File, Signal, Process, Cancel, Link, Monitor) into the
same lane would have:

- Tripled the diff size and review surface for a fix where the
  ahu-blocker is one specific effect.
- Taken longer than the 2–4 hour brief estimate (each effect needs
  its op slots verified against `builtin_<eff>_ops` and a wrapper
  per op — Cancel has fewer ops, but Link/Monitor each have
  several).
- Risked introducing subtle bugs across nine effects when only one
  was load-bearing for the immediate user (#570).

The 4-of-13-pass jump in the ahu test surface confirms the right
move: the failing 5 ahu tests all use `Link + Cancel + Console` (per
their type signatures), so they need the Link/Cancel additions too.
A follow-up lane can audit all eight missing builtins together,
ideally migrating the LLVM backend to the AST-derived path the C
backend has used since #558.

## Verification against ahu fixtures

Cloned `git@github.com:kaikailang-org/ahu` at HEAD and ran every
`examples/*/main.kai` and `tests/*.kai` file through `bin/kai build`
+ direct execution.

- **Pre-fix baseline (per the issue body):** 1 of 13 passing.
- **Post-fix:**
  - 4 of 4 examples (`counter`, `echo`, `pipeline`,
    `resilient_counter`) compile and run to exit 0.
  - 4 of 9 tests pass (`cell_behavior_switch`, `cell_done_first`,
    `cell_state_record`, `stream_pipeline`).
  - 5 of 9 tests still fail with exit 139: every failing one
    declares `/ ... Link + Cancel + Console` in its row.

So the count is **8 of 13 passing post-fix vs 1 of 13 pre-fix** — a
material unblocking, leaving 5 fixtures that need the next lane's
Link/Cancel additions. Filing those as a follow-up is the right
shape; bundling them into this lane would have widened scope past
the brief's estimate.

## Selfhost / tier behaviour

- `make tier0` — green. Selfhost byte-identical (Spawn is not in
  the compiler's own `main` row), 28 demos passing (baseline 27).
- `tools/test-llvm-driver.sh` — 13 fixtures pass parity (was 12),
  including the new `examples/llvm/spawn_actor_in_mailbox.kai`
  regression. The pre-existing `examples/quickstart/02_fizzbuzz.kai`
  failure is unchanged on `main` (Stderr row issue, unrelated to
  this lane).
- `make tier1` — green local (full run included).

## Real cost vs estimate

Brief estimated 2–4 hours for a localised fix. Actual: ~1.5 hours,
of which:

- ~30 min reading the previous lane's retro and confirming the
  diagnosis still held by re-reproducing on baseline.
- ~30 min locating all the call sites (decls, install, teardown,
  IR header declares, runtime wrappers) and verifying the slot
  indices against `builtin_spawn_ops` and the emitted `%EvSpawn`
  struct.
- ~15 min implementing the five mechanical additions.
- ~15 min validating on the repro, the new fixture, the test
  driver, the ahu tree, and tier0/tier1.

The estimate was generous because the previous lane's diagnosis
removed all the discovery work — the only judgment call was
"hotfix Spawn alone vs sweep all eight missing effects", and the
brief framed Spawn-only explicitly.

## Fixtures shipped

`examples/llvm/spawn_actor_in_mailbox.kai` — the verbatim shape of
the #570 repro plus a `Stdout.print` so the parity sweep can diff
the stdout between the C and LLVM backends. Wired into
`tools/test-llvm-driver.sh`'s `FIXTURES` list. Pre-fix on the LLVM
backend reproduces SIGSEGV; post-fix both backends exit 0 with the
same stdout.

No negative fixture: the install branch is unconditional within the
`if list_has(main_row, "Spawn")` gate, and there is no organic path
to construct an effect named "Spawn" that should *not* receive a
default handler — `effect_default_block_all_extern` returns true for
any effect whose `default { }` block is all `$extern_handler`, which
the builtin Spawn decl always satisfies via
`builtin_default_block_for("Spawn", builtin_spawn_ops())`.

## Cross-references

- Issue #570 — closed by this lane.
- Issue #571 — closed by the previous lane (#573 merged).
- `docs/lane-experience-issue-570-571-llvm-lambda.md` — the prior
  retro that did the diagnosis work this lane consumed verbatim.
- `default_setups_for` (stage2/compiler.kai:18385) and
  `default_shims_for` (stage2/compiler.kai:18448) — the AST-derived
  path that the C backend uses; a future LLVM cleanup lane should
  migrate the LLVM emitter onto the same shape rather than continue
  the hand-written branch-per-effect pattern.
- ahu repository (5 of 9 tests still failing post-fix because Link /
  Cancel are the remaining missing LLVM defaults) — natural next
  lane.

## Follow-ups

1. Audit the eight other missing LLVM defaults (Stdin, Env, File,
   Signal, Process, Cancel, Link, Monitor). The same shape applies;
   each adds ~30 lines (decls + install + teardown + header declares
   + runtime wrappers per op).
2. Strategic refactor: migrate the LLVM backend's
   `llvm_emit_main_install_defaults` to derive from the AST the way
   the C backend does (`default_setups_from_block`). Eliminates the
   per-effect maintenance and prevents this category of gap entirely.
   Larger lane; not a hotfix.
3. Once Link/Cancel are installed, re-run the ahu test suite and
   close out the remaining 5 failures or open issues for whatever
   surfaces.
