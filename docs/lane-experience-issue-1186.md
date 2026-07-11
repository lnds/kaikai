# Lane experience — issue #1186 (map-pipe field stage into fusible terminal)

## Scope as planned vs shipped

**Planned.** Fix the typer regression where `xs | (r) => r.field |> sum`
(a map-pipe into a fusible terminal) leaves the map-pipe lambda's param
unresolved → "type annotation needed". Add a per-PR regression fixture,
confirm `demos/vs/python` recompiles.

**Shipped.** Exactly that, one-line root cause. The issue was already
bisected and diagnosed by the integrator (culprit `b65d7687` / #1145);
the lane went straight to the fix. No scope creep.

## Root cause

`#1145` added terminal-fold fusion: `(xs | lam) |> sum` rewrites to
`list.map_sum(xs, lam)`. The purity gate (`fuse_terminal_gate` →
`fuse_raw`) synthesises each stage closure **in isolation** solely to read
its effect row, then discards the typed node. The commit's own message
already flagged one InferState-threading hazard ("synthesised stages in
isolation and discarded the returned state; the fresh-tvar counter lives
in InferState") and threaded the counter + substitution forward.

What it did **not** thread back was `deferred_fields`. A stage body
`(r) => r.amount` with an unannotated `r` synthesises the field access
against a still-free receiver tyvar, which registers a `DeferredField`
in `st.deferred_fields`. That entry's receiver is the *probe's* copy of
`r`, which nothing ever pins — only the re-synthesised `map_sum(xs, lam)`
call binds the *real* stage param to `xs`'s element type. The orphan
probe entry survived to `drain_deferred_fields` (run once per decl in
`infer_decl`) and drained to the annotation-needed arm.

The nested form `sum(xs | lam)` never hit this because it takes no fusion
path at all — no `|>` to a fusible terminal, no probe.

## The fix

`fuse_raw` now rolls the probe's `deferred_fields` back to the pre-probe
value via a new `st_restore_deferred_fields` helper, keeping every other
InferState field the probe advanced (substitution, fresh-tvar counter,
insts, holes, errs). Rewinding the counter/sub instead — the naive "run
in isolation, throw away the state" — would re-mint ids the probe already
bound and cross types between chains, which is exactly the hazard #1145's
message warned about. Restoring only the one field that leaks is the
minimal sound rollback.

Rejected alternative: unify the probe's `r` against `xs`'s element type
inside the gate (the issue's suggested direction). That works but
duplicates the element-type plumbing the re-synth already does correctly,
and it would have to reproduce `map_sum`'s scheme instantiation by hand.
Dropping the orphan entry is strictly less code and lets the one
authoritative synth (the rewrite call) own the binding.

## Structural surprises

- The purity gate re-synthesises the stage **twice**: once in the probe,
  once in the rewrite. That is by design (probe reads the row before the
  rewrite decides to fire), but it means any *accumulating* InferState
  field the probe touches needs a rollback decision, not just the counter.
  `deferred_fields` was the one that bit; `op_payload_labels` / `op_res`
  only accumulate on op-calls, which a pure map stage does not make, so
  they were left threaded (touching them without evidence would be
  over-correction).

- The worktree trap fired first: initial Read/Edit landed against the base
  checkout `kaikai/`, not the worktree `kaikai.fix-1186-pipefusion/`
  (different inodes). Caught it when `make kaic2` reported "up to date" —
  reapplied in the worktree, reverted the base. The tell is a build that
  refuses to rebuild after an edit.

## Fixtures added, coverage gap closed

`examples/perceus/pipe_fusion_terminal_field_1186.{kai,out.expected}` —
both shapes: the full `filter |> · | field-stage |> sum` and the bare
`| field-stage |> sum`. The Makefile target
`test-perceus-1186-map-pipe-terminal-field` gates on (1) compiles, (2)
exact output, (3) the emitted C still calls `map_sum` — so a future
"fix" that disabled fusion to dodge the type bug fails the fusion grep,
not just the type check. Wired into `TEST_LIGHT_TARGETS`.

The gap this closes: **no per-PR suite compiles `demos/vs/`**, so the
original break was caught only by the serial parity harness. The
interaction is now covered per-PR at the fixture level.

## Verification

tier0 (selfhost byte-id C), `make -C stage2 selfhost` (C byte-id),
native selfhost gate (native byte-id vs oracle, circle closes), the new
fixture on C and native, all #1134/#1143 fusion fixtures on C and native,
broad typer smoke (infer/run/blocks/match/records/pipes/effects). Heavy
suites (full tier1, ASAN, modular) delegated to CI.

## Follow-ups

None required. The rollback is local to `fuse_raw`; no downstream lane
inherits work from this one.
