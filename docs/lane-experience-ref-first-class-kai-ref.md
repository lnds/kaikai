# Lane experience — `Ref[T]` as a first-class `KAI_REF` cell

**Date:** 2026-05-27 (overnight autonomous lane, follows the Mutable
effect-leak fix)
**Scope:** replace the `Ref = length-1 KAI_ARRAY` hack (issue #257) with a
proper single-cell runtime representation (`KAI_REF`). The #257 retro
explicitly flagged this as "the clean fix"; this lane does it.

## Why

`Ref[T]` had no representation of its own. `ref_make` was
`kai_array_make(1, init)`, `ref_get`/`ref_set` were `array_get_impl`/
`array_set_impl` on slot 0. Three problems:
1. The representation didn't reflect the invariant — a Ref is exactly one
   cell, but as a KAI_ARRAY it carried a length/capacity field and was
   conceptually open to `array_set(r, 5, v)` / `array_grow` / `array_length`
   (operations a cell must not support).
2. Dead weight: every Ref paid for a length+capacity it never used.
3. Mixed two distinct concepts (fixed cell vs variable-length array).

The user's call: "kaikai debe ser un ejemplo de excelencia técnica; que los
Ref sean array de tamaño 1 rompe eso, es flojera." Correct, and the lineage
is ML/OCaml `ref` and Haskell `IORef` — a one-slot identity box.

## What shipped

New tag `KAI_REF` in `stage0/runtime.h`:
- enum entry + a `struct { KaiValue *cell; } ref;` arm in the value union.
- `kai_free_value`: decref the single cell.
- `kai_op_eq`: identity comparison (`a==b` handled above → distinct refs
  are unequal). A Ref is a location, not a value.
- `kai_to_string`: renders `Ref(<inner>)` — closes the #257 retro follow-up
  (length-1 arrays used to print as `<array>`).
- `kai_head_tag`: `KAI_HEAD_ANON` (a Ref is not protocol-dispatchable).
- type-name debug switch: `"ref"`.

Rewritten primitives (`kai_prelude_ref_make/get/set`): allocate `KAI_REF`,
operate on `.ref.cell` directly. They **steal** the value into the cell
(ref_make/ref_set) instead of the array version's incref+decref dance — so
each op does strictly less RC work, which matters for Front A (the next
lane puts InferState hot fields in Refs and calls these on the hot path).

`runtime_llvm.c` needed **no change**: its `kaix_prelude_ref_*` and
`kaix_default_mutable_ref_*` are thin shims that call the `runtime.h`
functions, so they inherit the new representation automatically.

`stage1` needed no change: kaikai-minimal does not know `Ref`.

## Structural notes / surprises

- `-Wswitch` was the perfect guide: enabling `KAI_REF` immediately flagged
  every tag-switch that needed a new arm (head_tag, op_eq, free, to_string).
  The two cosmetic switches (type-name, ctype-name) had `default:` so they
  didn't warn; handled type-name for honesty, left ctype on default.
- The LSP reported phantom errors at an unrelated `struct *` init line
  (4729-ish) on every edit — those are the linter mis-parsing a macro; `cc`
  compiled clean throughout. Do not chase LSP diagnostics in runtime.h;
  trust the compiler.
- kaic0 does **not** embed runtime.h — it *emits* `#include "runtime.h"`
  into the C it generates (emit.c:2013). So a runtime.h change does not
  require rebuilding kaic0; it takes effect when the emitted C is compiled.
  kaic1/kaic2 link against it via `-I stage0` and were rebuilt.

## Verification

- Direct C unit test: ref_make/get/set round-trips (7 → 42), `to_string`
  yields `Ref(42)`. ASAN+UBSan clean.
- All Ref fixtures compile + run with correct output: `mutable_ref_basic`
  (2), `ref_sugar_basic` (0/42), `ref_sugar_cross_fn` (13, Ref crosses a
  function boundary), issue #285 while/until (3).
- ASAN+UBSan clean on `mutable_ref_basic` and `ref_sugar_cross_fn` compiled
  through kaic2 — the new ownership convention has no leak/double-free.
- selfhost byte-identical (the compiler does not use Ref in its own source
  yet, so its emitted C is unchanged), tier1 green.

## Cost vs estimate

Estimate: contained runtime change, no typer/emit work. Actual: matched.
The representation swap was ~6 small edits in one file plus the primitive
rewrite. No backend codegen touched (ref ops are runtime builtins behind a
trampoline; the emitter never assumed `Ref`→`KAI_ARRAY` in any path).

## Follow-ups for next lanes

- Front A (RC churn): InferState hot fields (`sub`, `env`, accumulators)
  become `Ref[T]` cells so `st_set_*` mutate-in-place instead of rebuilding
  the 18-field wrapper. The honest Ref is now the substrate. Snapshot the
  *value* (not the cell) in `st_restore_entries` to preserve branch
  isolation — canary fixture: a `match` with backtracking.
- Re-enable Ref-local masking in the typer (`is_mutable_array_write_name`
  already lists `Mutable.ref_set`; add `Mutable.ref_make` back to
  `rhs_is_local_array_origin`) — SAFE now only if `ref_*` gets a
  handler-free direct lowering. Until then the effect-leak lane's
  conservative "Ref writes always need `/ Mutable`" stands. NOTE: this lane
  did NOT change the dispatch path, so that precondition is not yet met —
  keep Ref-local masking off.
