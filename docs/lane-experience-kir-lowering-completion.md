# Lane experience — KIR lowering completion (interp / closures / effects)

**Scope:** complete the AST→KIR lowering (`kir_lower*.kai`) that Lane 0
shipped with a silent catch-all (`_ -> LRes(KUnitV, st)`) swallowing whole
node families. Close every family so the KIR is a faithful, complete
lowering of the post-perceus AST — the durable asset now that the target
is in-process libLLVM, not textual `.ll` (see below).

## Scope-as-planned vs scope-as-shipped

This lane started as "KIR Lane 1" — write the KIR→LLVM **textual**
translator (`emit_llvm_from_kir`) behind a `KAI_BACKEND=llvm-kir` flag, in
3-way parity with C-direct and LLVM-direct. Mid-lane the plan changed: the
native target became **in-process libLLVM via the C API** (no `.ll` text,
no clang), and C-direct is a sufficient oracle. The textual translator was
abandoned — it polished a throwaway artifact.

What stayed was the discovery the parity work surfaced: **Lane 0's catch-all
mis-lowered far more than the 3 families its retro named.** Measured over
the core: `ERecordLit` (28 sites), `EModCall`-as-value (28), `ETodo` (20),
`ELambda` (12), plus `EHandle` (effects) and interpolation (`EList`). The
lane re-scoped to "complete the lowering" with the binary gate **zero
unhandled-node traps over the corpus + correct dump**.

Shipped (all families lowered, zero traps over 121 core/effects/perceus/
stdlib fixtures):

- **Interpolation / lists** — `EList` → a `kai_cons`/`kai_nil`/append
  spine (was `string_concat_all(())`). Closing `EList` closed interp,
  which desugars to `string_concat_all([parts])` upstream.
- **Mechanical families** — `ETodo`/`EHole` → panic stub; `ERecordLit` →
  `KRecord`; `ERange` → `kai_range`; `EVariantsOf` → nullary cons spine;
  `EModCall`-value → nullary-ctor / fn-value closure.
- **Closures** — split into (a) fn-value: an `EVar`/`EModCall` naming a
  top-level fn → `KClosure` over its thunk; (b) inline lambda → `KClosure`
  + a lifted `KFnLambda` body. Plus the local-vs-fn **call dispatch**
  (`dec(buf,pos)` on a param is `KCallIndirect`, not a direct `@kai_dec`).
- **Effects** — `EHandle` → a double fork (`install` + `setjmp` +
  `condbr` splitting the body path from the longjmp-discard), body inline,
  return clause inline at the merge; op-calls (`Eff.op`) → `KPerform`;
  clauses → lifted `KFnClause`s with `@resume` / `@cap` params;
  `KProgram.install_order` + `handlers` populated (the #1 effect-divergence
  the design cites).

## Design decisions and alternatives considered

1. **`KFnKind` + `KParamOrigin` over a prologue-of-prims** (asu). A lambda/
   clause's thunk ABI (`self, args, n[, k]`) is a *structural* property of
   the symbol — `kind: KFnNormal|KFnLambda|KFnClause`, param `origin:
   PDirect|PCapture|PEvidence|PResume` — NOT a prologue of `kai_arg`/
   `kai_capture` prims the backend re-recognises. The latter re-derives the
   calling convention per backend — the same lowering-duplication that
   killed the textual-LLVM frontier. `is_handler_thunk: Bool` generalised
   to the enum.
2. **Handle = double fork, body inline** (asu). "KStmt with join slot" +
   "body inline as blocks" are one model: the handle materialises blocks
   like `lower_if_blocks`, with the setjmp split as an ordinary `KCondBr`
   both backends read identically. `install`/`setjmp`/`pop` are EXPLICIT
   markers, not folded into one opaque node — keeping the fork target-
   neutral. Clauses = thunks; body = inline. (The §4.4 "body_thunk" was
   written before reading the oracle and is wrong.)
3. **one-shot resume PINNED** (user call). `KResume` models consume-`k`;
   `PResume` enters Perceus use-analysis; a structural uniqueness check
   (`check_resume_one_shot`) turns "resumed twice" from a runtime error
   into a compile-time one. Closing the door to multi-shot now needs a
   deliberate edition bump.
4. **`KSwitch` over `KTagOf`, restored** (design §4.2). A mid-lane change
   to carry the scrutinee directly was a textual-backend artifact (its
   uniform boxed-slot model couldn't hold `KTagOf`'s i64); reverted to the
   design's tag model when the textual backend was dropped.
5. **`LowerSt` tuple → record.** Adding `lams`/`enc_fn`/`effs` for closure
   and effect lowering would have touched every positional destructure;
   the record makes field-adds local. Net A-grade outcome.

## Structural surprises

- **"no panic over corpus" is not a correctness gate.** Lane 0 measured
  "304 examples produce KIR without panic" and shipped a catch-all that
  lowered effects/interp/closures to plausible-but-wrong unit. The real
  gate is correctness vs the mature emitter (`emit_c`) as oracle. Captured
  in memory.
- **The `tv` shadowing trap** (a binder named like a top-level fn resolves
  to that fn) cost a long debug — a `lower_list` `tv` binder aliased
  `fn tv` in `infer.kai`, surfacing as a non-exhaustive panic far away in
  the dumper. Renamed all binders to collision-free names.
- **The SCC is irreducible.** `lower_expr ↔ lower_call ↔ lower_if/match/
  handle/list/...` is one mutual-recursion cycle the import DAG cannot
  split. The fn/program-level orchestration (`lower_to_kir`, fn/lambda/
  clause generation) DOES split out (`kir_lower_fns.kai`) because it drives
  ON `lower_expr` but is never called back. The Expr-SCC walk stays one
  module at **B+** — the genuine floor for a recursive-descent lowering in
  this language (same as Lane 0's walk).

## Code-quality outcome (§7.1 gate)

| file | km | LOC | cogcom avg/max |
|---|---|---|---|
| `kir.kai` | A+ | 223 | 0/0 |
| `kir_lower.kai` | A (92.7) | 388 | 1.2 / 4 |
| `kir_lower_fns.kai` | A+ (93.8) | 218 | 1.4 / 3 |
| `kir_dump.kai` | A | 296 | 1.3 / 3 |
| `kir_lower_walk.kai` | B+ (84.2) | 746 | 2.1 / 6 |

All within the cap (<800 LOC, cogcom avg<5/max<25). The walk is B+ (the
SCC floor); everything else A/A+. The `lower_named_call` if/else chain →
`classify_callee` enum + `lower_list`'s axis-extraction kept cogcom-max at
6 despite the family growth (asu's "fix the two hot functions, don't split
the SCC" lever).

## Fixtures added / coverage gaps

- `examples/kir/{interpolation,closures,effects}.kai` + `.kir.expected` +
  `.kir.fns`, wired into `test-kir`. The harness awk now captures
  `lambda fn` / `clause fn` so the goldens pin `@cap` / `@resume` params.
  The Lane 0 goldens (`atoms_calls`, `control_flow`) were regenerated —
  `Stdout.print` now shows `perform`, not a spurious `con`.
- Gaps left (documented, not silent): FFI extern bodies (an `ETodo` tag)
  lower to a panic stub, not an FFI shim — the lowering does not yet detect
  extern fn bodies; a literal-discriminated match (`match n { 0 -> ...; 1
  -> ... }`) collapses non-variant arms to one default (only wildcard `_`
  is exact); arena regions are not threaded (`region_id` stays None). None
  affect the families this lane closed.

## Cost vs estimate

Larger than "Lane 1": the lane absorbed the lowering completion the textual
backend would have depended on anyway. The textual translator work
(written then deleted) was not wasted — it surfaced every gap by failing to
compile the core, one family at a time, which is what drove the corpus
measurement. Two asu consults (KFn modeling, handle control-flow) and one
user escalation (one-shot resume) resolved the load-bearing semantics
without guessing.

## Follow-ups for next lanes

- The in-process libLLVM backend consumes this KIR; it validates behaviour
  against C-direct (no textual oracle).
- FFI extern lowering, literal-match exact dispatch, arena region threading
  (the documented gaps above).
- `KSetjmp`/`KInstall`/`KPop`/`KPerform`/`KResume` are the single effect-
  semantics definition both backends will read — the divergence the design
  set out to kill.
