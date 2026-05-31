# Lane experience — issue #86 piece 2 (contract-violation panic shows the offending value)

## Scope as planned vs as shipped

**Planned (brief):** when a `requires`/`ensures` contract assert fails at
runtime, the panic should include the *runtime value* of the binding that
violated the predicate (`argument b was: 0.0`), on top of the piece-1
predicate-aware context (predicate text + kind + fn + decl span) that was
already shipped. Piece 3 (the `help: narrow b to Real where != 0.0`
suggestion) was explicitly out of scope — it needs the binding's base type
from the typer, which is not available at the assert-insertion site.

**Shipped:** exactly piece 2, restricted to `requires` predicates of the
simple `<ident> <cmp> <literal>` shape, under **both** backends (C and
LLVM), byte-identical. New runtime helper `kai_assert_check_with_value`;
two new `kaix_` forwarders + IR declares; emitter branch in both
`emit_assert_stmt` (C) and a newly-live LLVM `SAssert` arm. Two new
regression fixtures plus four existing goldens gaining the value line. Doc
sidebar trimmed. Piece 3 left documented as the remaining follow-up.

## Design decisions and alternatives considered

### Ownership contract of the value arg: CONSUME, not BORROW

The brief specified a BORROW contract (`val` not incref'd, helper does not
free it). That is correct only in an all-boxed world. The Phase-3 unbox
repr breaks it: when the binding is a raw scalar (`kair_b`, a `double`),
the call site must box it (`kai_real(kair_b)`) to produce a `KaiValue*` —
a fresh OWNED temporary with no natural owner. Under BORROW that temporary
leaks on the hot OK path and on the failure path.

Inverted to CONSUME after an asu consult: the helper decrefs `val` on every
exit (ok / panic / longjmp). The call site passes a fresh owned ref —
`kai_incref(kai_b)` for a boxed local, `kai_real(kair_b)` for a raw one.
Both reprs unify, no leak on any path, no second helper. The boxed
`kai_incref` is the soundness condition, not avoidable overhead: it
transfers a *new* reference the helper consumes, leaving the local's own
reference (and any Perceus drop) untouched.

### The repr oracle: which C local name exists

A `requires b != 0.0` assert reads `b`. In C the binding is either
`kai_b` (boxed `KaiValue*`) or `kair_b` (raw scalar) depending on whether
the enclosing fn is a UFn. The emitter must pick the *same* name
`emit_expr` would, or it references a nonexistent local and the C compiler
rejects it.

First attempt used the *comparison literal's* `mode` as the oracle
(literals are never rewritten by Perceus, so their mode/ty survive). This
was **wrong**: `requires c != '_'` on a `Char` param lowers the comparison
raw (literal mode `MUnboxed`) even though the `Char` local is stored
**boxed** (`kai_c`). The literal's mode reflects how the comparison was
lowered, not how the binding is stored. The char fixture crashed the C
compiler with `use of undeclared identifier 'kair_c'`.

Fix: the oracle is the **ident node's own `mode`** in C (the exact value
`emit_expr` consults for that node, guaranteeing agreement), and the
**`locals` table** in LLVM (`LRaw` vs `LL` — the definitive storage repr,
more robust than any AST mode). The literal's `ty` is kept only as the
box-type fallback for the raw case (a dup-wrapped ident node may have a
blanked `ty`).

### Evaluation order: incref before the condition

The biggest surprise. The condition `kai_op_ne_v(kai_b, kai_real(0.0))`
**consumes** both operands (`kai_op_*_v` decref their args). So evaluating
the condition frees the boxed `kai_b` (rc 1→0). A `kai_incref(kai_b)` for
the value arg that runs *after* the condition then increfs a freed object
→ bus error. C's argument-evaluation order is unspecified, so it crashed
non-deterministically (the char fixture happened to survive; the boxed-real
fixture reliably crashed).

Fix: pin the order so the incref runs **before** the condition.
- C: wrap in a statement-expression — `({ KaiValue *_av = <valarg>;
  kai_assert_check_with_value(<cc>, msg, "id", _av); })`. `_av` (the
  incref) is the first statement; the condition `<cc>` is evaluated only
  when the call's args are built, after `_av`.
- LLVM: emit the value (incref/box) **before** `llvm_emit_expr(cond)`,
  capturing `val_reg` first, then emitting the condition, then the call.

### `requires` only (not `ensures`)

A `requires` assert runs at function entry, before the body consumes any
param, so the binding's local is guaranteed live. An `ensures` assert runs
after `let result = body`, where the referenced param may already have been
consumed (its local freed) — referencing it for the value would be a
use-after-free. `ensures` keeps the value-less form (piece-1 context only).
Documented as a follow-up. This is a conservative correctness call, not a
laziness one: the alternative is liveness analysis the lane did not need.

## Structural surprises the brief did not anticipate

1. **Recon paths were one reorg stale.** The brief cited
   `stage2/refinements.kai`, `emit_c.kai`, `stage2/llvm_shim.c`. The actual
   tree is `stage2/compiler/*.kai` (modular extraction, PR #677 era) and
   the LLVM forwarder lives in `stage0/runtime_llvm.c`, not
   `stage2/llvm_shim.c`. The stringify symbol is `kaix_to_string`, not
   `kaix_to_str`. The file:line offsets were otherwise accurate.

2. **`examples/refinements/` already existed.** The brief said it did not;
   piece 1 had created it with seven fixtures + goldens. The lane updated
   four goldens (the simple-shape `requires` ones) rather than creating a
   directory.

3. **Variant-name collision in the flat bundle.** Named the result type's
   constructor `CI`, which the stage2 bundle flattening collided with
   `ClauseInfo`'s existing `CI(...)` constructor — selfhost caught it, not
   `make kaic2` (the modular build hid it). Renamed to `CtrId`. This is the
   recurring "compiler module name / constructor must be unique across the
   flat bundle" trap.

4. **The LLVM `SAssert` arm was a no-op.** Contracts never fired under
   `--backend=llvm` at all — a latent correctness bug the lane closes as a
   side effect. Wiring it required two forwarders (`kaix_assert_check` and
   `kaix_assert_check_with_value`) so plain asserts also fire under LLVM.

## Fixtures added and coverage gaps

- New: `examples/refinements/requires_value_boxed_real.kai` (+ golden) —
  pins the **boxed Real** repr path (`describe(b: Real) : String`, so `b`
  is boxed, exercising `kai_incref(kai_b)` not `box_wrap(kair_b)`). This is
  the exact shape that crashed before the evaluation-order fix, so it
  guards that regression directly.
- New: `examples/refinements/requires_value_reused_binding.kai` (+ golden)
  — the offending binding is read again in the body, so Perceus wraps the
  predicate read in `__perceus_dup(b)`. Guards that
  `expr_pred_ident_node` unwraps the dup layer and still forwards the
  value (raw-unboxed UFn repr under a dup-wrapped read).
- Updated four goldens to carry the value line:
  `requires_violation_diagnostic` (Int), `requires_violation_real` (raw
  Real UFn), `requires_violation_char` (boxed Char), `requires_named_binding`
  (Int, negative value).
- `test-violations` now runs **every** fixture under both C and LLVM,
  diffing each against the same golden — byte-parity is the gate. Catches
  any future drift between the IR declare and the `runtime_llvm.c`
  forwarder ABI (the #732/#736 bug class).

Coverage of reprs: Int (raw + boxed via cache), raw Real (UFn), boxed Real,
boxed Char. Negative controls that must NOT gain a value line: String
literal (non-scalar), `and`-compound predicate (not a single cmp), and
`ensures` (liveness). All verified.

The dup-wrapped read case (ident read again later in the body, forcing a
`__perceus_dup` wrapper around the ident node) is now covered by
`requires_value_reused_binding`, which verified `expr_pred_ident_node`
unwraps the dup layer correctly under both backends.

## RC discipline / leak analysis

`KAI_TRACE_RC` on a contract failure *inside a test block* shows a nonzero
`leaked` count. Measured carefully it is **not** from the new helper:
- A plain failing user `assert` inside a test leaks 0 — the longjmp does
  not inherently leak.
- A failing contract with a value line leaks N for the first test and
  grows by exactly the *test body's* allocation (e.g. +2 for two `z()`
  calls) per additional failing test — identical to the value-less
  `kai_assert_check` path. The growth is the body's temporaries that the
  longjmp jumps over (the assert is the first statement, so Perceus has not
  yet dropped them), a pre-existing property of the longjmp-in-test model.
- The helper's own temporaries (`vs`, `m0..m3`, `full`, `val`) are all
  released before the longjmp/panic; the one-time intern-table allocation
  of the short literal pieces (`"\nargument "`, `" was: "`) is immortal by
  design (rc = INT32_MAX), counted as "leaked" but never freed for any
  `kai_str("...")` literal in any program.
- On the OK (passing-contract) path the helper's value box is alloc+freed
  (`kai_incref` then `kai_decref(val)`), net zero. Verified by diffing the
  trace against an identical no-contract program: +1 alloc, +1 free.

ASAN + UBSan (the lane's actual gate, `detect_leaks` unsupported on macOS)
run the failing-contract-in-test path with **no** memory-error diagnostics
for both the raw and boxed reprs — the ordering fix means no use-after-free
on the `kai_incref`.

## Real cost

Three real iterations beyond the happy path: (1) the BORROW→CONSUME
ownership inversion, (2) the literal-mode→ident-mode/locals-table repr
oracle after the char fixture failed to compile, (3) the
evaluation-order/statement-expression fix after the boxed-real fixture
bus-errored. Each was caught by an *additional* fixture or backend, which
is the argument for exercising both backends and both reprs up front.

## Follow-ups left for next lanes

- **Piece 3** (issue #86 stays open): the `help: narrow b to Real where
  != 0.0` suggestion. Needs the binding's base type from the typer threaded
  to the assert-insertion site. Out of scope here by design.
- **Call-site span caret** (`--> file:line:col`): needs the caller's span
  threaded into the assert; orthogonal to the value piece.
- **`ensures` value support**: would need liveness analysis to know whether
  the referenced binding's local is still alive at the post-body assert.
