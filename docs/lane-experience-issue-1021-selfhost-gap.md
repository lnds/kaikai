# Lane experience — native self-host gap: the compiler compiles itself under the native backend

## Scope as planned vs as shipped

**Planned:** close the native self-host subset gap surfaced by the gate — drive the
`kaic2 --emit=native` self-compile of the compiler from 25 `unbound register`
aborts to 0, so the native backend (the default) can compile the compiler itself.

**Shipped:** exactly that, via a 6-line fix in one lowering function. The 25 aborts
were a single mechanism, not five separable classes. Baseline flipped 25 → 0; the
gate now reports "native self-host ACHIEVED".

## The bug

A variant arm whose payload mixes a **binder slot** with an **empty-list slot** —
`TyName(nm, [])` — followed by a catch-all `_`, routes through the *lone-list-slot*
path in `emit_arm_subtests_and_binds` (`kir_lower_walk.kai`). That path called
`lm_emit_cells` (which binds the list's own head/tail names) but **never called
`bind_pattern_fields`** for the sibling slots. So the sibling binder `nm` got no
`KLet`, and the arm body's `KVar(nm)` read an unbound register — which the native
backend refuses (the C-direct oracle emits binds straight from the AST, so it was
immune and the gap stayed native-only and silent).

The comment on `var_arm_lone_list_slot` already *described* the correct behaviour
("every other slot is a binder / wildcard which the post-test `bind_pattern_fields`
reads") — but the code in the lone-list branch never made the call the comment
promised. The fix adds it. A `PList` slot is a no-op in `bind_pattern_fields`, so the
added call binds only the siblings and does not double-handle the list slot.

## Design decisions and alternatives considered

- **Bind siblings after `lm_emit_cells`, not a bespoke sibling-only walk.** The
  existing `bind_pattern_fields` already skips `PList` slots (no-op), so reusing it is
  correct and needs no new traversal. A separate "bind everything except the list
  slot" helper would have duplicated logic for no gain.
- **The C oracle stayed byte-identical.** The list slot is a no-op in the shared bind
  path, so the C backend's output does not change — `make selfhost` is byte-id green.
  The fix is purely additive on the native path.

## Structural surprises the brief did not anticipate

- The initial hypothesis (from an exploration pass) blamed `bind_unsupported_nested`
  / nested-variant-test patterns. Instrumenting the lowering proved that function is
  called **zero** times in the self-compile — the hypothesis was wrong. The real
  cause was the lone-list-slot path dropping sibling binds.
- The bug needs BOTH an empty-list slot AND a catch-all `_` in the same match. With an
  exhaustive same-tag arm set (no catch-all) the arm takes a different decision path
  and binds correctly; the catch-all is what forces the lone-list path.
- The minimal repro took several tries: top-level `TyName(nm, [])` alone does not
  reproduce — the catch-all is essential to the trigger.

## Diagnosis method

Grepping source for the register names (`nm`/`tyname`/`n`/`name`/`eff`) was ambiguous
(these are common variable names). What localised it: temporary `eprint` markers at
`nemit_fn` (prints each function's sym) and at the abort site (`nemit_load_reg`),
then `awk`-correlating each abort with its enclosing function. That pinned the 25 to 6
functions, all sharing the `Variant(binder, [])` + catch-all shape.

## Fixtures added and coverage

- `examples/llvm/variant_list_slot_sibling_bind.kai` — the bug shape as a native-vs-C
  parity fixture (binder slot next to an empty-list slot, plus a catch-all). Verified
  green on both backends with identical output; covered by the parity harness.

## Cost

A 6-line fix, but the diagnosis was the work: three instrumentation rebuilds of the
native `kaic2` (~4 min each) and several minimal-repro iterations to isolate the exact
trigger. The wrong initial hypothesis cost one exploration round.

## Follow-ups

- The gate now passes at baseline 0 ("ACHIEVED"). The next milestone is to upgrade it
  from "compiles with zero aborts + produces an object" to "the native object LINKS +
  RUNS as a full self-host chain" — then the native self-host story is end-to-end and
  the tracking issue closes.
- `tier1-native` is still not a required check. Once the LINK+RUN upgrade lands and
  holds, it can be added to branch protection to make native self-host a hard gate.
