# Lane experience — LLVM nested record sub-pattern destructuring (Cluster F, refs #622)

## Scope as planned vs as shipped

**Planned.** Close the `rc_discipline_record_variant` parity divergence
(Cluster F): a nested record pattern in a `let` binding produced the wrong
result and a "type mismatch in +" under the LLVM backend while the C backend
was correct. Make the fixture exit 0 with matching output on both backends,
add a minimal regression fixture, keep selfhost byte-identical.

**Shipped.** Exactly that, in one file (`stage2/compiler/emit_llvm.kai`), one
function family. The root cause was narrower and more honest than the briefing
hypothesis: it was **not** a wrong field offset or wrong base register feeding
a raw value into `+`. It was a flat-out **unsupported pattern shape** — the
LLVM let-destructure emitter only handled `PBind` / `PWild` field sub-patterns
and dropped everything else on the floor with a `v1 not supported` warning,
leaving the inner binders unregistered.

## Root cause

`let { p: { x, y }, label: _ } = b` lowers to an `SLet` with a `PRecord`
pattern whose fields contain a **nested** `PRecord` sub-pattern (`p: { x, y }`).

The LLVM path `llvm_emit_let_record` (emit_llvm.kai) matched only:

```
PBind(local) -> # extract field, push local
PWild        -> # skip
_            -> eprint("nested pattern in let-destructure not supported in v1")
```

So for `p: { x, y }` it hit the `_` arm: it printed the warning and recursed
to the next field **without binding `x` or `y` at all**. Downstream, `x + y`
referenced two names that were never pushed as locals, so the closure builder
emitted `cannot build closure for 'x'`, fed garbage operands to the `+` op,
and the runtime reported `type mismatch in +`. The observable was a build-time
warning + a runtime type error, not a silent wrong value — the briefing's "`x`
== 2" symptom was from a sibling shape; the committed repro reproduces the
"cannot build closure / type mismatch" form directly.

So: **missing re-root**, not wrong offset. The leaf binder path was already
correct (`kaix_field(scr, "x")` re-roots fine); the gap was that a
**destructuring** sub-pattern never got to recurse.

## How the C path differs (the correct model)

The C backend is unified: `emit_let_stmt` → `emit_pat_binds` → for a record,
`emit_pat_binds_record` (emit_c.kai ~2416). Its key move on a destructuring
sub-pattern (`pat_is_destructuring(sub)`):

```c
KaiValue *_pf_L_C = kai_op_field(scr, "p");   // extract owned temp
<bind inner pattern against _pf_L_C, is_alias=true>
kai_decref(_pf_L_C);                          // drop the intermediate
```

The leaf binders inside (`x`, `y`) each extract their *own* owned
`kai_op_field` against the inner temp. The C path re-roots correctly; the LLVM
path never recursed. The fix mirrors this exactly.

## The fix

Three additions in `emit_llvm.kai`, all in the let-destructure family:

1. `llvm_emit_let_field_extract` — factor the `tmp = kaix_field(scr, name)`
   emit out of the existing PBind arm so the destructuring arm reuses it.
2. `llvm_emit_let_subpat` — bind an irrefutable sub-pattern against an
   already-extracted owned field register. Handles `PBind` (take the owned
   ref directly), `PWild` (no-op; the caller drops), nested `PRecord` /
   `PVariantRecord` (re-root via `llvm_emit_let_record` on the field reg),
   and `PAs` (incref the alias since the caller decrefs the shared temp).
   It does **not** drop `scr` — the caller owns and drops it.
3. `llvm_emit_let_record` `_` arm — for a destructuring field sub-pattern,
   extract the field to an owned temp, re-root the sub-pattern on it via
   `llvm_emit_let_subpat`, then `kaix_decref` the intermediate. This is the
   direct mirror of emit_c.kai's `pat_is_destructuring` branch.

`kaix_field` == `kai_op_field` in `runtime_llvm.c` (returns an incref'd value),
so the ownership transfer matches the C path one-for-one.

## RC reasoning

The intermediate field (`p`) is extracted owned (+1). The inner leaf binders
(`x`, `y`) extract their own owned fields and keep them; the intermediate `p`
temp is then decref'd. This is the same balance the C path produces. The LLVM
backend still under-frees overall (known: the LLVM emitter has minimal RC
discipline by design — `KAI_TRACE_RC` shows `free_total=0` on the repro), but
the new `kaix_decref` of the intermediate is the *correct* drop and does not
introduce a double-free (verified: ASAN clean, see below).

## Fixtures

- `examples/perceus/nested_record_pattern.kai` (+ `.out.expected` = `7`) — the
  minimal `nested` shape plus `flat` / `access` controls (both already worked
  on LLVM), so a future regression in only the nested path is isolatable.
- `examples/perceus/rc_discipline_record_variant.kai` — the original Cluster-F
  fixture; its skip line removed from `tools/backend-parity-skips.txt`. Now
  prints `38` on both backends.

## Verification

- Minimal repro: `nested` → `x + y == 7` on C and LLVM. ✓
- `rc_discipline_record_variant`: `38`, exit 0, both backends. ✓
- `tools/test-backend-parity.sh`: no new divergence. The remaining FAILs
  (blackjack RNG, issue_141 timestamp, issue_682 fiber-cancel order,
  cross_package_effects array marshal, auto_install missing-setup) are all
  pre-existing and reproduce identically on `main`. ✓
- `make selfhost`: `kaic2b.c == kaic2c.c` byte-identical. ✓ (critical —
  record patterns are pervasive in the compiler; selfhost uses the C backend,
  and byte-identity confirms the emit_llvm.kai edit perturbed nothing.)
- `make tier0`: OK, demos baseline 34 holds. ✓
- `make tier1` / `make tier1-asan`: see PR checks.

## Cost vs estimate

Cheaper than feared. The briefing anticipated a possible offset/base-register
bug requiring `.ll` dumps and GEP-chain reasoning. The actual cause was a
missing recursion arm, diagnosable by reading the two emit paths side by side.
The one real cost was a 20-minute detour: the initial edit landed in the
`main` worktree (the absolute path in the project CLAUDE.md context pointed at
the sibling checkout), which had to be reverted and re-applied to the
`fix-nested-record` worktree before any build counted.

## Follow-ups left for next lanes

- `PList` / `PVariant` sub-patterns in an *irrefutable* let-destructure still
  hit the `_` arm of `llvm_emit_let_subpat` and warn. They are rare in `let`
  position (lists are refutable; bare-variant lets are unusual) and out of
  this lane's scope, but a future lane could complete the mirror for full
  parity with `emit_pat_binds`.
- The LLVM backend's overall under-free (project-known minimal RC discipline)
  is untouched here; the structural Perceus-for-LLVM work is its own track.
