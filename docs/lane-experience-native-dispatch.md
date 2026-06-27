# Lane experience — native-dispatch-fix (#929 #930 #931 #932)

Four native-backend dispatch/lowering crashes confirmed by the native-truth
audit, all reached by following the primary docs verbatim, all on the DEFAULT
(native) backend. Each produced a host SIGSEGV or a silent-wrong result on
idiomatic code.

## Scope as planned vs as shipped

Planned: fix the four crashes, diagnosing a shared root cause before patching
four times. Shipped: exactly that, plus one correctness bug the #931 root
cause also covered (`<` / `>` / `==` on a derived-Ord type were silently wrong
through the same broken table key). No scope drift.

## Did the four share a root cause?

No single root cause, but a clear split:

- **#930 and #932 share a layer** (`kir_lower_walk.kai` match dispatch) but are
  **distinct bugs**:
  - #932 (union-narrow always-first-arm): the top-level arm-dispatch
    predicates (`arm_tag`, `arm_is_default`, `guard_pat_always_matches`,
    `guard_emit_pat_test`) did not look *through* a `PAs(name, PVariant(..))`
    arm — the exact shape the typer rewrites `a:A` into. Native's nested-slot
    layer (`kir_lower_variant.kai`) already unwrapped `PAs`; only the
    top-level layer did not. Fix: unwrap `PAs` in those predicates +
    `var_arm_is_tag_catchall` / `var_emit_subtests`, and bind the as-binder
    to the whole scrutinee in `bind_pattern_fields`.
  - #930 (literal-head + guard-shadowing arm): a match-arm binder
    (`n if n < 0`) emitted `KLet("n", SBoxed, ..)` with no shadow check, so it
    collided with the raw-`SInt64` scrutinee param `n` in the name-keyed
    native register table, widening its alloca to `ptr` and corrupting the
    scrutinee — `kai_eq_raw` then dereferenced a raw Int as a pointer. The
    `let`-path (`lower_let_pbind`) already had the shadow-rename guard; the
    match-arm binders did not. Fix: a shared `bind_scr_alias` mirroring
    `lower_let_pbind`, plus a per-arm `renames` snapshot in `lower_guard_arms`.

- **#929 (resume-as-value)** is in the typer: a clean reject was missing.
  `resume` is an ordinary identifier (the clause's last param); the whole
  compiler treats the literal name `"resume"` as magic and only handles it in
  *call* position. A value-position use (`let k = resume`) fell through to the
  backends, which have no value representation for it (C emits an undeclared
  `kai_resume`, native loads a NULL reg and crashes on the call). Fix: an
  `in_clause: Bool` flag on `InferState`, set on clause entry, cleared inside
  lambdas (mirroring `clause_resume`), consulted by a guard in the `synth`
  `EVar("resume")` arm — the natural choke point, because call-position
  `resume(...)` is intercepted upstream by `try_resume_call`. The one subtlety:
  for *stateless* clauses `clause_resume` is `None`, so `resume(...)` calls
  there were *not* intercepted upstream and would have hit the new guard;
  `try_resume_call` now also handles the stateless case
  (`synth_resume_call_stateless`) so the legal call keeps working.

- **#931 (bare protocol op as value)** is in the runtime/codegen **shared
  dispatch ABI** — the widest-surface fix and the only ABI change. The impl
  table keyed on `(proto_id, head_tag)` ONLY, with no operation discriminant.
  A multi-op protocol (`Ord` = cmp/min/max, `Numeric` = add/mul/...) registers
  N impls with the SAME key; the open-addressing table treats them as
  duplicates and keeps only the last. A bare-op-as-value dispatch (`sort_by(xs,
  cmp)`) goes through `__proto_cmp` → `kai_lookup_impl(ORD, head)` and resolves
  to whichever op won — `Ord.max`, returning a raw-Int reinterpreted as a
  pointer. Direct calls `cmp(a,b)` work because they are rewritten to a
  concrete `__pimpl_*` call that never touches the table.

## #931 design decision — shared vs split, A vs B

Considered keying per-op two ways:

- **(B)** give each op its own `proto_id` (no struct change). Rejected: there
  are four hardcoded `kai_lookup_impl(KAI_PROTO_ORD/EQ, head)` call sites in
  `runtime.h` (`kai_op_lt`/`kai_op_gt`/`kai_op_eq`×2) that assume "the
  protocol's canonical op". B fragments the id space and still has to touch
  them, losing its only advantage. Those same call sites are why `<`/`>` on
  derived-Ord types were already broken.
- **(A)** add an `op_id` axis to the key `(proto_id, op_id, head_tag)`.
  Chosen: it fixes the hardcoded consumers legibly and keeps `proto_id`
  one-per-protocol. Surface is wider (struct + hash/lookup/insert/register in
  both `runtime.h` copies, the C-API register/dispatch shims in
  `runtime_llvm.c`, both emitters, the `PimplRow` op-name field, the KIR
  dispatcher args) but every change is mechanical. A single shared
  `stdlib_proto_op_id_int` in `emit_shared.kai` is the source of truth so C
  and native compute identical keys.

## Structural surprises

- The typer rewrites `a:A` (`PNarrow`) into `PAs("a", PVariant("A", []))`
  BEFORE codegen (`expand_narrow_arms`), so codegen never sees `PNarrow` — the
  defensive `PNarrow` arms in the lowering are dead paths. This is why #932
  lived entirely in the `PAs` handling.
- The native register table is flat and name-keyed; a same-name binder with a
  different slot silently widens and corrupts (#930). The hazard was already
  documented in `kir_lower.kai` for `.tN` temporaries and the `let`-path, but
  the match-arm binder paths never got the guard.
- The #931 bug was masked for so long because the table-dispatch path is ONLY
  reached by bare-op-as-value; every ordinary `cmp(a,b)` is statically
  resolved. The audit reached it by following `docs/protocols.md:204`.

## Fixtures added

- `examples/negative/effects/resume_escape_as_value.kai` (+`.err.expected`) — #929 reject.
- `examples/effects/match_guard_shadows_scrutinee.kai` (+`.out.expected`) — #930.
- `examples/effects/match_union_narrow_multi_tag.kai` (+`.out.expected`) — #932.
- `examples/stdlib/sort_by_bare_ord_op.kai` (+`.out.expected`) — #931 (bare op + derived-Ord operators).

## Verification

Each repro confirmed native == C oracle (or a clean reject for #929) on the
freshly-built `bin/kai`. Anti-regression corpus: `test-match`,
`test-effects`, `test-effect-runtime`, `test-protocols`,
`test-proto-scalar-dispatch`, `test-unions`, `test-bare-narrow-assert` all
PASS; `test-negative` 133 PASS / 0 FAIL. Selfhost byte-id green on both
stages. Serial backend parity (`BACKEND_PARITY_JOBS=1`) clean. ASAN on the
dispatch/lowering path clean.

## Follow-ups

- `min` / `max` as a bare value still resolve to the stdlib list/option `min`
  fn by name before the `Ord` op — a name-resolution ambiguity, identical on
  both backends, NOT a dispatch bug. Out of this lane's scope.
- The `op_id` map in `stdlib_proto_op_id_int` is a static stdlib table; a user
  protocol with >1 op would need the same per-op indexing. Pre-1.0 the table
  covers every shipped multi-op protocol.
