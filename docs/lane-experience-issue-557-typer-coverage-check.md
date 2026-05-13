# Lane experience report — issue-557-typer-coverage

Lane brief: close issue #557 — Stage B of the #533 trilogy. Wire the
typer's per-op coverage check so an `Eff.op` reached from a body
either lands inside a `handle` clause, falls through to a `default { }`
block, or is rejected with a precise diagnostic. Stage A (#556)
already shipped `$extern_handler` + AST-derived `default_*_from_block`
helpers; Stage B turns the spec's 5-step algorithm into a real walker.

## Outcome

Stages 1-4 shipped. tier0 + tier1 green; selfhost byte-identical.
Six new fixtures land (one positive in `examples/effects/`, three
negatives in `examples/negative/effects_phase2/`, plus the existing
Stage A positive keeps passing). `docs/effects.md` gains a
`default { } blocks and per-op coverage` sub-section under
`### Handling`. Diagnostic shape pinned with four explicit variants:
no block, partial block, kaikai-bodied (Stage C deferred), masked-by-
enclosing-handle (Stage C deferred).

## Spec vs. reality: which steps shipped

The #533 body and the #557 lane brief both list a 5-step algorithm.
Stage B ships steps 1, 3, and 4 fully; step 2 is **rejected with a
Stage-C-aware diagnostic** instead of discharging the op, because
Stage A's codegen masks any enclosing default at the handle level
(see "Why step 2 was deferred" below).

| Step | Spec  | Stage B (this lane) |
|------|-------|---------------------|
| 1. Clause discharge | Enclosing `handle ... with E { clauses }` lists `op` → discharge. | Implemented — `check_op_call_coverage` walks the lexical handle stack and absorbs the obligation when `clause_ops` contains `op`. |
| 2. Clause-merge from default | Enclosing `handle ... with E`, omits `op`, `E.default` declares `op` → discharge. | **Deferred to Stage C.** Codegen Stage A's `emit_clause_assignments` does not synthesize the missing clause inside `_ev`, so the inner handle masks the enclosing default and dispatch hits `_ev.<op> = NULL`. Until the handle emitter consults `default { }` blocks, the typer rejects with a `masks the main-level default — Stage C will let the handle inherit unmissed default clauses` note. |
| 3. Main absorption | `main` row contains `E`, `E.default` declares `op` → discharge via auto-installed handler. | Implemented for the 17 canonical builtins. For Stage A's `Stdout` migration, the source-level `default { }` block discharges; the 16 others still take the legacy hardcoded path. User-declared effects are rejected at the row level (`check_main_row_handlable`) until Stage C extends `default_setups_for` / `default_shims_for` to walk arbitrary decls. |
| 4. Reject | Otherwise — emit `effect not handled: E.op`. | Implemented with a four-way diagnostic note that distinguishes the deferred Stage C cases from real "missing default" cases, so the user can decide between adding a clause now vs waiting for Stage C. |
| 5. `return` clause | Treated uniformly with ops. | Untouched — `return` already behaves uniformly in `synth_handle` (`check_return_clause_opt`). |

## Why step 2 was deferred

Initial scaffolding implemented step 2 by accepting the partial handle
when `default_ops` covered the omitted op. A round-trip with the C
output of a fixture revealed the codegen gap:

```c
typedef struct EvMyLog {
    KaiValue *(*info)(EvMyLog *self, ...);
    KaiValue *(*warn)(EvMyLog *self, ...);
} EvMyLog;
/* handle prologue */
EvMyLog _ev = {0};
_ev.info = &_kai_main__clause_12_3_info;  /* only the clause-listed op */
/* _ev.warn stays NULL */
```

`emit_clause_assignments` (stage2/compiler.kai:16066) walks the
`HClause` list and emits one assignment per user-typed clause.
`default { }` blocks are not consulted. The inner handle's evidence
node sits on top of the evidence stack, so `kai_evidence_lookup_node("MyLog")`
returns it for `MyLog.warn`, and the dispatch path

```c
EvMyLog *_ev_op = (EvMyLog *) _node_op->handler;
KaiValue *_op_r = _ev_op->warn(_ev_op, _op_arg_0, &_k);   /* NULL */
```

calls a NULL function pointer. Even when `default { }` clauses bridge
to `$extern_handler("c_sym")` and the C runtime entry exists, the
inner `_ev.warn` is what dispatch reads, not the main-level default.

Implementing step 2 honestly therefore requires extending the handle
emitter to synthesize the missing default clauses inside each `_ev`.
That is structural codegen work, not in scope for Stage B per the
lane brief's `Stage B does NOT migrate the 17 builtins; that's Stage C`
clause — and the same emitter machinery handles both extensions.

The Stage B compromise: the typer's coverage walker treats step 2
**identically to step 4** (reject), but `describe_default_status`
distinguishes the four reasons in the diagnostic so the user knows
exactly which Stage closes the gap.

## Touchpoints landed

- `check_default_coverage` + helpers (stage2/compiler.kai:~17820) —
  post-typing walker over every `DFn` body. Maintains a stack of
  `HandleFrame(eff_name, clause_ops, default_ops, line, col)` entries
  pushed at every `EHandle` and consults `find_effect_default_block`
  per call site. Wired in `infer_program_with_protos` so the count
  folds into `TypedProgram.errs` and triggers the existing `errs != 0`
  fail-fast at the top level.
- `effect_default_op_names` (Stage B addition) — returns only the
  `extern_handler`-bridged op names from a `default { }` block, since
  Stage A's emit path is gated on that bridge shape.
- `report_uncovered_op` + `describe_default_status` — four-way
  diagnostic surface keyed off the default-block state (no block,
  partial, kaikai-bodied, extern-bridged-but-masked).
- `check_body_row`, `check_main_row_handlable`, `only_non_absorbed_labels`,
  `effect_has_default_handler` — extended with a `decls: [Decl]`
  parameter threaded through `infer_decl` and `infer_all_loop` from
  `infer_program_with_protos`. The signature widening was the
  cheapest way to give the row-level check access to `default { }`
  state without bloating `InferState`'s 28 constructors.

## Design decisions worth pinning

1. **No `InferState.decls` field.** The first iteration of the lane
   tried to add a `decls: [Decl]` slot to `InferState`; the type has
   28 constructor sites threaded through the typer's pipeline, and
   widening it for one feature is precisely the kind of debt #367
   was opened against. Parameter-threading through three signatures
   (`infer_all_loop`, `infer_decl`, `check_body_row`) was the lower-
   cost option and keeps the type's responsibility surface local to
   inference state, not program structure.
2. **Coverage walker is post-typing, not inside `synth_handle`.**
   Running the walker after `infer_all_loop` returns a typed program
   means the `Expr.ty` annotations are stable, `EVar("Eff.op")`
   callees are resolved, and the walker can key on the dispatch key
   alone without re-implementing op lookup. Inside `synth_handle`
   the row representation does not track which ops were performed
   (only `Label { eff: <name>, ty_args }`), so re-engineering it to
   carry per-op granularity would have been a much larger surface
   change.
3. **`Cont` not needed at the typer level for Stage B.** The walker
   does not touch the resume-type — it operates on op-name strings.
   Stage A's `Cont` reservation stays the only language-level change.
4. **`is_effect_declared` gate.** The walker also fires for stdlib
   effects injected by `inject_builtin_effects`; gating on
   `is_effect_declared OR is_absorbable_stdlib_eff` keeps the check
   surface tight without depending on the user-vs-builtin distinction
   ahead of Stage C's unification.

## Bug found during the lane (none structural)

One transient diagnostic regression — initially `describe_default_status`
returned `does not declare warn` when the default block declared
`warn` but with a kaikai body. Adding the second branch (kaikai-bodied
vs extern-bridged-but-masked) closed it; both branches name Stage C
so a future contributor recognises the planned upgrade path.

## Stdout post-Stage A verification

`make tier1` exercises every demo + every negative fixture. Selfhost
fixed point is byte-identical. Stdout's Stage A `default { print(s,
resume) -> $extern_handler("kai_default_stdout_print") }` continues
to discharge through step 3 of the algorithm; the diagnostic surface
fires on no demo, confirming the walker is silent for the migrated
builtin's coverage.

## Real cost vs estimate

| Item | Estimate (#557 body) | Actual |
|------|----------------------|--------|
| Total wall time | ~4-5 days | ~3 hours |
| Walker over typed AST | ~2-3 days | ~1 hour (mechanical kind-by-kind recurse mirroring `collect_call_labels`) |
| Diagnostic shapes | ~30 min | ~30 min (matched estimate) |
| Fixtures | ~1 hour | ~30 min |
| Doc update | included in fixtures | ~15 min |
| Step 2 codegen-aware compromise | not budgeted | ~30 min of probing the handle emitter before settling on rejection-with-Stage-C-note |

The 4-5 day estimate assumed full clause-merge support (step 2 in
codegen too) plus rewiring `synth_handle` rather than a post-typing
walker. Both were dropped during exploration in favour of the
post-typing walker + Stage C deferral, which shrank the lane to a
few hours of mechanical work plus diagnostic polish.

## Fixtures shipped

Positive (`examples/effects/`):
- `default_block_full_user_handle.kai` + `.out.expected` — partial
  effect (`info` only) with `default { info(...) -> $extern_handler }`
  wrapped in a full clause-coverage handle. Step 1 discharge; the
  default block is unused but present. Runs end-to-end and prints
  `result: 7`.

Negative (`examples/negative/effects_phase2/`):
- `partial_handle_no_default.kai` — `MyLog { info, warn }` handler
  covers `info`, body performs `warn`, no `default { }` block.
  Diagnostic shape: `default { }` block does not exist → step 4
  reject with `declares no default block` note.
- `partial_handle_kaikai_default.kai` — same shape but with a
  `default { warn(...) -> resume(()) }` clause whose body is a
  kaikai expression. Diagnostic shape: clause exists but its body is
  not an `$extern_handler(...)` bridge — Stage C codegen needed.
- `partial_handle_extern_default_masked.kai` — `default { warn(...)
  -> $extern_handler(...) }`. Diagnostic shape: default declares
  `warn` via `$extern_handler`, but the enclosing handle masks the
  main-level default — Stage C will let the handle inherit.

`tools/test-negative.sh` count: 95 (pre-#557) → 98 (post-#557).

## Follow-ups for Stage C (#558)

- **Extend `emit_clause_assignments`** to consult the effect's
  `default { }` block: for every op declared in `effect E { ops }`
  that is NOT in the handle's user clauses, emit an `_ev.<op> =
  &<default_shim>` assignment using the same `$extern_handler` →
  shim path that `default_setups_from_block` already builds. This
  closes step 2 and lets the typer flip its rejection branch into a
  discharge.
- **Extend `default_setups_for` / `default_shims_for`** to walk every
  `DEffect(_, _, _, _, Some(_), _, _)` decl, not just the 17 builtin
  names. This closes step 3 for user-declared effects and removes
  the `is_absorbable_stdlib_eff` gate from `effect_has_default_handler`
  + `check_op_call_coverage`'s `at_main` branch.
- **Drop the unused `decls` parameter** from
  `effect_has_default_handler` once the gate above flips — the
  parameter is threaded today only to keep the API ready.

## Verification summary

- `make tier0` — green (25 OK, 28 demos baseline holds, selfhost
  byte-identical).
- `make tier1` — green (98 negative fixtures pass; `test-negative`
  count +3).
- Stdout extern-bridged default (Stage A) continues to discharge at
  `main` — `examples/effects/extern_handler_user_effect.kai`
  unchanged.
- New positive `default_block_full_user_handle.kai` compiles and
  runs end-to-end (`result: 7`).
- Three new negative fixtures rejected with distinct diagnostic
  shapes that name Stage C where applicable.
- Selfhost fixed point: byte-identical with `main` (verified by
  `make tier0`'s selfhost step).
