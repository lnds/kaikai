# Lane experience — LLVM↔C parity lane D (invalid IR build-fails)

Refs #622. Plan: `docs/llvm-parity-plan-2026-05-26.md` (Cluster D).
Sequenced first by unanimous review (asu/linus/eric): cheapest, most
isolated, oracle is `clang` accept/reject — no runtime semantics.

## Scope as planned vs as shipped

Planned: fix the two LLVM-emit build-failures that make
`examples/perceus/reuse_record_basic.kai` and
`examples/effects/trace_prefix.kai` produce invalid `.ll` that clang
rejects; remove their two skip lines; confirm parity.

Shipped: exactly that. Two emitter patches in
`stage2/compiler/emit_llvm.kai`, two skip lines removed. Both fixtures
now build + run byte-identical stdout / exit code on C and LLVM. No
scope creep, no runtime touched (`runtime_llvm.c` untouched, so no
tier1-asan needed).

## Root cause of each bug

### Bug 1 — `reuse_record_basic`: phi references nonexistent `%entry`

Symptom: `%t6626 = phi %KaiValue* [ %t6622, %entry ]` → clang "use of
undefined value '%entry'".

Root cause: the emitter initialises `LlvmEmit.cur_label` to the string
`"entry"` for every function (`llvm_emit_new`, and the per-fn `e0` in
`llvm_emit_fn`), so any phi whose incoming value was produced in the
function's first basic block records `%entry` as its predecessor label.
But the normal DFn emit path (`llvm_emit_fn`) went straight from
`define ... {` to the body **without emitting an `entry:` header** —
LLVM's implicit first block is unnamed, so `%entry` is undefined.

Why it only surfaced here: `EIf` phis join freshly-opened
`if.then.N`/`if.else.N` blocks, never the entry block. The match
lowering (`llvm_emit_match_arms`) captures each arm's `body_end =
eb.cur_label`; for the **first** arm, the body is emitted before any
`match.next.N` block is opened, so its label is still `"entry"`. Perceus
single-arm record-reuse (`kaix_reuse_or_alloc_record`) produces exactly
this shape: one match arm, body finishing in the entry block, phi at
`match.end`.

Fix: emit `entry:\n` right after `define ... {` in the normal DFn path,
matching the convention the FFI shim (`llvm_emit_ffi_shim`, line ~4486)
and proto-dispatch shim already follow. Makes the label the emitter
already tracks a real block. Minimal, no change to match/if logic.

### Bug 2 — `trace_prefix`: redefinition of `@eff.disp.name.6865`

Symptom: `@eff.disp.name.6865` emitted twice with different content
(`"Trace\00"` and `"Stdout\00"`) → clang "redefinition of global".

**Not** a name-dedup problem — `Trace` and `Stdout` are genuinely
distinct effects. The bug is **id-counter collision across emit
phases**. `emit_program_llvm` runs three top-level emit phases that each
mint fresh globals from a per-phase id counter:

1. `llvm_emit_decls_loop(... 0 ...)` → returns `decls_out.next_id`.
2. `llvm_emit_lambda_bodies(... decls_out.next_id ...)`.
3. `llvm_emit_clause_bodies(... decls_out.next_id ...)`  ← bug.

Phases 2 and 3 **both started at `decls_out.next_id`**, so the lambda
phase and the clause phase mint overlapping `@eff.disp.name.N` ids.
`trace_prefix` has both a lambda body (the `Trace` dispatch) and a
handler-clause body (the `Stdout` dispatch) that landed on the same id
`6865`.

The reason phase 3 couldn't chain off phase 2: `llvm_emit_lambda_bodies`
returned only a `String` (the IR text), **dropping its final id**. So
there was no `next_id` to thread.

Fix: change `llvm_emit_lambda_bodies` / `_loop` to return `LlvmDeclOut`
(text + next_id), then start clause bodies at `lam_bodies.next_id`.

## Did the two bugs share code?

No. Independent. Bug 1 is in the per-function body framing
(`llvm_emit_fn`); bug 2 is in the program-level phase sequencing
(`emit_program_llvm`). They only shared the symptom "the `.ll` won't
compile" and the underlying theme that the LLVM emitter's
SSA-name/block bookkeeping is looser than the C backend's (which never
emits phis or shares a global-id namespace across functions).

## Fixtures / regression coverage

The two fixtures **are** the regression coverage: removing their lines
from `tools/backend-parity-skips.txt` wires them into
`tools/test-backend-parity.sh`, which builds both backends and asserts
stdout + exit-code parity. Verified:
- `reuse_record_basic`: C exit 0 / LLVM exit 0, stdout identical.
- `trace_prefix`: C exit 0 / LLVM exit 0, stdout identical.
- Parity harness: pass 322→324, fail 9→9 (same 9 pre-existing), skip
  117→115. Confirmed by stashing the fix and re-running baseline: the
  9 failures are identical and pre-existing (scheduler nondeterminism,
  log timestamps, package-path harness gaps) — none are LLVM-emit
  build failures and none touch phi/global emission.

`trace_basic` (Cluster E, "stale skip — already passes") left skipped:
it's another lane's call per the plan.

## Cost vs estimate

Estimate: "~2 patches in emit_llvm." Actual: 2 patches, ~1.5 h
including build/selfhost cycles. The triage doc nailed both root causes
at the symptom level; the only investigative work was confirming bug 2
was an id-collision (counter) rather than a missing name-dedup (the
plan offered both hypotheses — it was the counter).

## Selfhost

`make selfhost` → `kaic2b.c == kaic2c.c` byte-identical. The emit
change shifts no stage-2 self-compilation output (the C backend is
unaffected; only LLVM emit paths changed).

## What lanes A/B/C should know about emit_llvm internals

- **`cur_label` is a string the emitter threads, not derived from the
  IR.** It starts at `"entry"` and only becomes truthful now that the
  DFn path emits `entry:`. If you add any new phi site, its incoming
  labels come from `cur_label` at the end of each predecessor's emit —
  make sure each such block was actually opened with `llvm_open_block`
  (which emits the `<label>:` header), or it'll be another phantom-block
  bug like bug 1.
- **Global ids are minted from a single per-phase `LlvmIdGen` counter
  threaded as `start_id`/`next_id`.** All three top-level phases
  (decls → lambdas → clauses) now chain strictly. If you add a fourth
  emit phase that mints globals, chain it off `clauses_out.next_id`, not
  off an earlier phase — otherwise you reintroduce bug 2's collision.
  `install`/`teardown`/`proto_init` are safe today because they use
  fixed/own-scheme labels, not the threaded counter.
- **The LLVM emitter is structurally looser than the C backend** on
  exactly the dimensions the C backend never exercises: SSA phi nodes
  and a shared cross-function global namespace. eric's follow-up
  (symbol-coverage script) won't catch these — they're emitter-internal
  invariants, not shim drift. A cheap future guard: pipe every emitted
  `.ll` through `clang -fsyntax-only`/`llvm-as` in a tier (the parity
  harness already does this implicitly by building, but only for
  un-skipped fixtures).
- Lane A (Char→String) is `runtime_llvm.c`, a different file entirely;
  no overlap with this lane.
