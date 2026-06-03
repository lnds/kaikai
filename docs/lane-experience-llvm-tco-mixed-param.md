# Lane experience — LLVM backend: mixed raw/boxed params in the UFn calling convention

## The bug was mis-framed as "no TCO in LLVM"

The starting symptom (a 4-line repro) was a `String` accumulator
threaded through a self-tail-recursive function printing the integer
`2` instead of the string under the LLVM backend:

```kai
fn rep(i: Int, n: Int, acc: String) : String {
  if i >= n { acc } else { rep(i + 1, n, acc) }
}
```

A stale memory said "LLVM backend has NO TCO". Wrong: issue #706 had
already ported the tcrec loop to LLVM (`br %tcrec.loop` back-edge). The
actual defect was narrower and deeper — the **UFn raw calling
convention (issue #718) does not handle a boxed param in a mixed
signature**. `lookup_ufn_sig` classifies a fn as a UFn when *any* param
is unboxable (`any_param_unboxable`), and the `UFnSig = US(pts, rt,
raw_mask)` already carries a per-param `[Bool]` mask — but the LLVM
emitter ignored the mask and lowered *every* param through
`llvm_raw_ir_type_t`, which returns `i64` for a boxed type (String /
list / variant). So `acc: String` (a `%KaiValue*`) was emitted as
`i64`, read as a raw scalar, and re-boxed with `kaix_int(<ptr>)`.

## It was NOT a tcrec bug — the no-TCO mixed case failed too

The first instinct was to fix the tcrec lowering. But a mixed-signature
fn with *no* tail recursion (`fn pick(i, n, acc: String) = if ... acc`)
emitted `i64 %kair_acc` and returned `2` just the same. The tcrec path
only inherited the defect; the root was the whole UFn lowering treating
params uniformly raw. The C backend, by contrast, already did this
right — `emit_param_list_masked` splits `kair_<p>` (raw) vs `kai_<p>`
(boxed) per the mask, emitting `int64_t kair_i, int64_t kair_n,
KaiValue *kai_acc`. The fix was to bring the LLVM backend to parity:
thread the mask through every site C already bifurcates on.

## Sites threaded with the mask (all per-param)

The mask had to reach **six** lowering surfaces, each of which had been
assuming all-raw:

1. **Signature** — `llvm_param_list_unboxed` (no-tcrec) and
   `llvm_param_list_unboxed_tcrec`: a boxed param emits
   `%KaiValue* %p_<p>` / `%parg_<p>`, not `iN %kair_<p>`.
2. **Local seeding** — `llvm_init_raw_locals_from_params`: a boxed param
   seeds as a normal boxed `LL(name, %p_<p>)`, not `LRaw` — so the
   body's `EVar` read resolves to the `%KaiValue*` register.
3. **TCO slots** — `llvm_emit_tcrec_slots_raw`: a boxed param allocas
   `%KaiValue*` (fed by `%parg_<p>`), a raw param allocas `iN`.
4. **TCO reload** — `llvm_emit_tcrec_reloads_raw`: boxed reloads into
   `%p_<p>` (`%KaiValue*`), raw into `%kair_<p>` (`iN`).
5. **Back-edge** — `llvm_emit_tcrec_goto_raw` rewritten as the per-param
   union of the old all-raw goto and the all-boxed `None` back-edge:
   a raw param stores its `iN` temp with no RC; a boxed param drops the
   outgoing slot per the C dropmask, then stores the `%KaiValue*` temp.
   New helpers `llvm_emit_args_mixed`, `llvm_emit_tcrec_drops_masked`,
   `llvm_emit_tcrec_stores_mixed`.
6. **Call sites + thunk** — `llvm_emit_ufn_call_boxed` /
   `llvm_emit_ufn_call_raw` (3 call sites) format args per mask
   (`llvm_format_args_masked`); `llvm_emit_thunk` leaves a boxed param
   as the loaded `%a<i>` and does NOT decref it (ownership passes to the
   callee), mirroring the C thunk's `KaiValue *kai_acc = args[2];`.

A boxed param's back-edge / thunk RC discipline follows the C reference
exactly: the body's borrow-reads do not consume the param, so the
dropmask (computed on the boxed Perceus model, which still sees the
boxed params of a mixed UFn) is the source of truth — raw params are
simply skipped.

## One symptom, four CI reds

The fix closed four distinct-looking tier1 failures that were all this
one bug:

- `test-modules-path` — `shout("kai")` (a String-accumulator recursion)
  diverged C `KAI!` vs LLVM `!`.
- `demos-core/portfolio (LLVM)` — `kai: field access on non-record`
  (the corrupted boxed value reached a field access).
- `protocols/m12_8_local_param_shadows_op (LLVM)` and
  `aspirational/pipeline_reorder_smoke (LLVM)` — `non-exhaustive match`
  (the corrupted boxed scrutinee failed every arm test).

The `field access on non-record` / `non-exhaustive match` framing in
the failure mapping was a red herring: a String/variant read as a raw
`i64` then mis-routed downstream. One root, four symptoms.

## Implementation surprises

- **`llvm_emit_expr` returns the `LlvmEmit`, not a `{e, reg}` struct.**
  The boxed arm of `llvm_emit_args_mixed` first read `r.e`/`r.last_val`
  treating the result like the `LlvmRaw` that `llvm_emit_expr_raw`
  returns; kaic2 panicked with `no such field 'e'`. The boxed reg is
  `e1.last_val` and the next state is `e1` itself.
- **kaic1 (stage1) rejects single-line two-arm list matches.** The
  `mask_head_or_true` / `mask_tail` helpers, first written inline
  (`match mask { [m, ..._] -> m  [] -> true }`), failed to parse in the
  bootstrap bundle. The accepted shape is multi-line arms, as the
  existing `pts_tail` does.
- **`llvm_emit_args_raw` had two non-tcrec callers** (the UFn call
  helpers). Renaming it broke them; kept it as a thin wrapper and added
  `llvm_emit_args_mixed` alongside.

## Fixtures

- `examples/perceus/tco_mixed_param_boxed_acc.kai` (+ `.out.expected` =
  `go:xxxxx`) — a mixed raw/boxed-param self-tail-recursion.
- `stage2/Makefile` `test-tco-llvm-mixed` (wired into `test`,
  `test-fast`, `.PHONY`): asserts the boxed param is `%KaiValue*` in the
  IR (not `i64` — the direct fix signal) and that C == LLVM == golden.

## Verification

- Repro `rep`/`build` prints the string on both backends.
- `make -C stage2 selfhost`: deterministic (kaic2b.c == kaic2c.c).
- `make -k test`: the four bug-class reds gone, zero new regressions.
  Remaining reds are pre-existing and unrelated (the C-emitter
  `multiple default labels` bug, `field access on non-record` in the C
  runtime, the proto-scalar-dispatch ABI shim, a `--path` harness gap).

## Follow-ups

- The pre-existing reds above are separate bug-classes, each its own
  lane.
- This fix reaches mixed signatures up to the boxed/raw split; a UFn
  with a `Real`/`Char` boxed-in-variant payload follows the same mask
  path and needs no further work, but was not exercised by a dedicated
  fixture beyond the String case.
- Update `docs/perceus-honesty-targets.md`'s TCO section: the "LLVM
  backend still emits a normal call for the sentinel" caveat is already
  obsolete (#706 fixed that); the "String param miscompiled across a
  self-tail-call" caveat this lane added is now also closed.
