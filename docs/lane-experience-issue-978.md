# Lane experience — issue #978: spawned fiber performing a parent-handled effect segfaults

## Scope as planned vs. as shipped

**Planned.** A program type-checks but segfaults at runtime when a fiber
spawned inside a `nursery` performs an effect whose handler was installed in
the parent fiber. Handler evidence is per-fiber and does not cross a `spawn`,
yet the typer accepts the program because the effect's row is satisfied at the
spawn point. Turn the segfault into a clean diagnostic (or reject at compile
time) — a type-checked program must not segfault.

**Shipped.** The runtime/codegen fix (option A below): every non-default
evidence-frame slot fill now routes through the same `kai_evidence_require`
guard the perform-site walk already used, so a spawned fiber whose body
installs no handler for a parent-handled effect exits 1 with
`kai: effect not handled in fiber: <Eff>` instead of segfaulting — on both the
native and C backends. No typer change.

## Decision: A (runtime/codegen) vs B (typer rejection)

Chose **A**. The justification is structural, not just minimal-diff:

- The `#820` capability model already says fiber-local capabilities do **not**
  cross a `spawn` — that is the design, not a bug. A spawned fiber resolving an
  effect against its own (empty) evidence stack and finding nothing is the
  *expected* outcome of that model. The defect was only that one resolution
  path turned "nothing found" into a NULL dereference instead of a diagnostic.
- The clean-diagnostic behaviour **already existed** for the fiber-local
  effects (`Actor`/`Cancel`/`Link`/`Monitor`/`Spawn`): they never ride a frame
  slot (`frame_is_fiber_local`), so their perform takes the by-name walk, which
  was already guarded by `kai_evidence_require`. `Actor.send` in an unhandled
  spawned fiber prints `effect not handled in fiber: Actor` and exits 1. The
  bug was the *same condition reaching a different, unguarded path* — exactly
  the issue's "gold hint". Option A makes the two paths converge on one
  diagnostic; it does not invent a new behaviour.
- Option B (reject at compile time any `spawn` of a fiber whose row carries an
  effect not handled within the fiber) is strictly more invasive and risks
  false rejections: a perfectly valid program can spawn a fiber that performs
  an effect handled by a handler the *spawned body itself* installs, or one
  legitimately delivered through a frame slot the call site fills. Deciding
  "handled within the fiber" at type time would require modelling the spawn
  boundary in the row solver — a soundness-sensitive change to the typer for a
  problem the runtime can report precisely and locally. Not worth it when A is
  a one-line-per-site guard that already had a working precedent.

A is sufficient: the reproducer, the Actor analogue, and the
handled-within-fiber positive all behave correctly with A alone, so B was not
needed.

## Root cause — why two sites, not one

The perform site reads a frame slot directly (`__evf[slot]`) and dereferences
`node->handler`. The slot is filled at the **call site**, not the perform site.
Two emitters fill it, and neither guarded a non-default effect:

- **C** (`emit_c.kai`): `evf_thunk_slot` (the spawned-closure forwarder
  `_kai_<fn>_thunk`, the path the reproducer hits) and `evf_terminal_slot` (a
  direct call) both emitted a bare `kai_evidence_lookup_node("Eff")`. In the
  spawned fiber that walk returns NULL; the NULL was stored in the slot and the
  perform dereferenced it.

- **native** (`emit_native_ops.kai`): `nemit_evf_slot_node`. The surprise here
  was that the native module mints a `_kai_default_node_<eff>` global for every
  effect in `kp.synth_defaults` — **including** effects with no `default {}`
  block (the binary had `_kai_default_node_Note` but **no** `_kai_default_ev_Note`).
  So the slot fill took the `else` (`kaix_evidence_lookup_or_default`) branch
  with a non-null global, returning a **zeroed synth-default node whose handler
  slot is NULL** — and the perform dereferenced that. The first fix attempt only
  guarded the `g-is-null` branch (mirroring the C `default_block` predicate) and
  did **not** fix native, because Note never took that branch. The correct fix
  guards *both* branches, exactly as `nemit_perform_walk_node` in
  `emit_native_fx2.kai` already did for its or_default result.

The C backend guards by `effect_has_default_block` (a real default block exists);
native guards by "does a `_kai_default_node_<eff>` global exist". Those two
predicates are **not** equivalent — native mints node globals for synth-defaults
that have no real default. That asymmetry is the entire reason the native fix
needed a different shape than the C fix, and it is the trap a future lane
touching either emitter should remember.

## Structural surprises

- **The first print never appears.** The reproducer's `worker` prints a line
  *before* `Note.say`, but the diagnostic fires before that print — because the
  evidence frame is built eagerly when the spawned thunk is entered, ahead of
  the body. The guard trips at frame construction, not at the perform. Output is
  `kai: effect not handled in fiber: Note\n` with nothing before it, identical
  on both backends.

- **`set -e` + a rc-1 binary.** The regression target captures the binary's
  exit code, but under `set -e` a `./bin; rc=$?` aborts the recipe the instant
  the binary returns 1 — before the assignment. The fix is `rc=0; ./bin || rc=$?`.
  The exit code matters to this test specifically: rc 139 (segfault) is the
  regression, rc 1 the clean diagnostic, so the target asserts `rc == 1`
  explicitly rather than only diffing stdout.

## Fixtures

- `examples/effects/issue_978_spawn_parent_handled_effect.kai`
  (+ `.out.expected`) — the issue's reproducer verbatim. Compiles, then exits 1
  with the clean diagnostic. Wired as `test-issue-978-spawn-parent-handled-effect`
  in `stage2/Makefile` (C path, asserts rc 1 ≠ 139 and diffs the diagnostic),
  added to `.PHONY`, `TEST_LIGHT_TARGETS`, and `test-fast`.
- The same fixture is auto-discovered by `tools/test-backend-parity.sh`, so its
  native↔C equivalence (same stdout, same exit code 1) is gated for free — no
  harness change needed for the native coverage.

## Verification

- Reproducer: `kai: effect not handled in fiber: Note`, exit 1, on **native and
  C** — no segfault.
- Actor analogue (`Actor.send` in an unhandled spawned fiber): still
  `effect not handled in fiber: Actor`, exit 1, both backends (no regression).
- Stdout/Console in a spawned fiber: still prints, exit 0, both backends.
- An effect handled *within* the spawned fiber: still dispatches to the local
  handler, exit 0, both backends.
- `tier0`: green (selfhost `kaic2b.c == kaic2c.c` byte-identical; evidence-frame
  gate `KAI_EVIDENCE_FRAME_ONLY` PASS; demos no-regression).
- `test-effects` + `test-effect-runtime` + `issue-668`/`679`/`682`: green.
- Serial backend-parity (`BACKEND_PARITY_JOBS=1`, native vs C): green.

## Cost vs. estimate

Two small emitter edits + one new function (`evf_require_walk`), one fixture,
one harness target. Two time sinks:

1. The worktree-path slip (the same one #970's retro records): the first round
   of `Edit` calls used the bare `…/kaikai/` path from the briefing context
   instead of the worktree `…/kaikai.fix-978-spawn-effect-segfault/`, landing
   the change in the main checkout. Caught when `make` reported "kaic2 up to
   date" and a grep for the new function came back empty in the worktree.
   Reverted main with `git checkout --` and re-applied via `git apply`.
2. The native synth-default-node asymmetry (above): the C-shaped fix was not
   enough, and only disassembling the native binary (`__kai_worker_thunk`
   calling `kaix_evidence_lookup_or_default` four times, never
   `kaix_evidence_require`) revealed Note was taking the or_default branch.

## Follow-ups

- None for this defect. The native/C guard-predicate asymmetry
  (`_kai_default_node_<eff>` minted for synth-defaults vs. a real
  `default {}` block) is documented here as a trap; unifying the two predicates
  is possible cleanup but is not motivated by this fix and would touch the
  default-minting path.
