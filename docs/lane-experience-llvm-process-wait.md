# Lane experience — LLVM `Process.wait` op-return marshalling (refs #622, Cluster F)

## Scope as planned vs as shipped

- **Planned:** fix the LLVM backend so `Process.wait` on a child that
  exited 0 yields a `Exited(0)` that matches the `Exited(0)` arm.
  Triage in `docs/llvm-parity-plan-2026-05-26.md` Cluster F + the brief
  pointed at "op-return value marshalling on the LLVM path" — how the
  effect-op CALL result flows back into the matched binding.
- **Shipped:** a one-function runtime fix in `stage0/runtime_llvm.c`.
  The root cause was *not* in the emitter or the op-return path. It was
  the LLVM variant-payload accessor `kaix_variant_arg` ignoring the
  variant `slot_mask`. Scope shrank from "emit op-return path" to
  "runtime accessor honor the slot mask" — one of the brief's three
  candidate root causes (op-return unbox vs WEXITSTATUS width vs
  runtime-build) was the truth, refined: it is a runtime accessor that
  reads the wrong union member.

## Root cause — emit vs runtime

Runtime, not emit. The chain:

1. The Process default handler is identical on both backends —
   `kaix_default_process_wait` (runtime_llvm.c) just forwards to the
   shared `kai_default_process_wait` (runtime.h). Both mint
   `Exited(WEXITSTATUS(status))` via `_kai_process_make_exit_exited`,
   which builds the variant cell with **`KAI_VAR_SLOT_INT`** on slot 0
   — the raw exit code stored in `slots[0].i64`, not a boxed `KaiValue*`
   in `slots[0].ptr`. This is deliberate (#440 Phase 2): the typer
   declares `Exited(Int)`, so stage 2 emits typed-slot match readers.

2. The **C backend** reads that payload mask-aware: `emit_c.kai`'s
   `slot_read_for_test` dispatches on `variant_slot_kind(t)` and emits
   `kai_int(scr->as.var.slots[i].i64)` for an Int slot,
   `kai_real(...slots[i].r)` for Real, `...slots[i].ptr` for pointers.
   It matches the producer's mask.

3. The **LLVM backend** reads every variant payload through one
   accessor, `kaix_variant_arg(v, i)` (called by `llvm_emit_pat_subpats`
   in `emit_llvm.kai`). That accessor returned `kai_incref(v->as.var.slots[i].ptr)`
   unconditionally — it **ignored the slot mask**. For `Exited(0)` it
   read the raw integer `0` from `.i64` reinterpreted as `.ptr` (a NULL
   pointer), increfed it, and fed garbage into the `match`. The
   `Exited(0)` arm's `kaix_eq_raw` then compared garbage and missed, so
   control fell to `Exited(_)` → "FAIL: nonzero exit".

The fix routes `kaix_variant_arg` through the **same** boxed-view
accessor the C backend uses, `kai_variant_slot_box` (runtime.h #440
Phase 2), which honors the mask: pointer slots return the borrowed
child, Int/Real slots return a freshly-boxed `kai_int`/`kai_real`. RC
nuance: the LLVM path keeps ownership uniform (caller owns the
returned cell), so we incref only the borrowed pointer-slot case; the
typed-slot temporary already arrives owned (rc==1).

## Process.wait-specific or general?

**General fix, today only Process-relevant.** The bug is in the generic
variant-payload accessor, so it affects *any* variant minted with a
typed slot mask that is matched under LLVM. But the LLVM emitter
*itself* always builds variants with `slot_mask == 0` (see `kaix_variant`
in runtime_llvm.c — it stamps every arg as a pointer slot), so codegen-
minted variants were never wrong. The only typed-slot variants in the
tree are **runtime-minted**: a grep of `stage0/runtime.h` for
`KAI_VAR_SLOT_INT`/`KAI_VAR_SLOT_REAL` constructors finds exactly two —
`Exited` and `Signaled` from the Process handler. So today this only
manifests through `Process.wait`. Tomorrow, any new runtime constructor
that mints a typed-slot variant (e.g. a future effect returning a raw
Int/Real payload) is now correct for free instead of being a fresh
parity bug.

**Implication for other Cluster F fixtures:** they do NOT share this
cause. `demos/spiral` is an Array bounds bug, `rc_discipline_record_variant`
is an unbox/`+` type bug — both per the plan's Cluster F table. This
lane closes exactly one Cluster F row (`process_basic`).

## Structural surprise the brief didn't anticipate

The brief framed it as an *op-return marshalling* bug ("how emit_llvm
lowers an effect-op CALL that returns a boxed variant"). That framing
was a near-miss: the value crossing back from the op-return is fine —
it's a correct typed-slot variant cell. The corruption happens *later*,
when the `match` arm reads the payload, in a path shared by ALL variant
matches, not the op-return path. The tell was that
`kaix_default_process_wait` forwards verbatim (runtime identical) yet
the match still saw nonzero: that ruled out the runtime-build
hypothesis and the WEXITSTATUS-width hypothesis and pointed at the
read side. This is the mechanism eric's structural follow-up warned
about: the hand-written `runtime_llvm.c` mirror drifting from
`runtime.h`. `kaix_variant_arg` was a #440-Phase-1 stub ("Phase 1:
every slot is a pointer (mask=0)") that never got the Phase-2
mask-aware update its C sibling did.

## Fixtures + coverage

- `examples/effects/process_basic.kai` (issue #126) is the coverage,
  now unskipped — its `Exited(0)` arm exercises exactly the typed-Int
  slot read. Removed its line from `tools/backend-parity-skips.txt`.
- No new smaller fixture: `process_basic` already isolates the shape
  minimally, and the only other affected constructor (`Signaled`)
  needs a child killed by a signal — frangible in CI for no extra
  coverage of the slot-mask read.

## Verification

- `process_basic`: C and LLVM both print `hello` / `wait ok` / `done`,
  exit 0. Identical.
- Full `tools/test-backend-parity.sh`: no new divergence. Remaining
  FAILs (`poker_dealer`, `blackjack`, `issue_141_log_default`,
  `auto_install`) are pre-existing nondet (RNG/scheduler), timestamp,
  and harness (#626 missing-module) noise, unrelated to this lane.
- `make selfhost`: byte-identical (`kaic2b.c == kaic2c.c`) — no
  compiler source touched, only `runtime_llvm.c`.
- `make tier0`: green (selfhost deterministic, demos baseline 34).
- `make tier1`: green.
- `make tier1-asan`: green (runtime change — ASAN run mandatory).

## Cost vs estimate

Faster than the brief budgeted. The triage in the brief was accurate
enough to land on the read side within a couple of greps; the fix is a
9-line function body honoring an existing helper. Most of the wall
time was build/selfhost/tier cycles, not diagnosis. The "structural,
leave open" escape hatch was not needed — the fix is localized and the
oracle (C backend + selfhost byte-identity) proves the C side is
untouched.

## Follow-ups left for next lanes

- eric's tier0 symbol-coverage script (plan §"Structural follow-ups")
  would have caught this at the commit that stubbed `kaix_variant_arg`:
  it is a `kaix_` accessor whose `kai_` sibling (`kai_variant_slot_box`)
  gained mask-awareness in #440 Phase 2 while the mirror did not. This
  lane is a second data point (after Cluster A/B) for that script's
  value. Not in scope here.
- Other Cluster F rows (`spiral` Array bounds, `rc_discipline_record_variant`
  unbox/`+`) remain open with distinct causes.
