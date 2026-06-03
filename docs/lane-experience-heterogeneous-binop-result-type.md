# Lane experience — heterogeneous binop result type (Real + Complex → Complex)

## Symptom

`examples/stdlib/complex_heterogeneous.kai` aborted at runtime with
`kai: field access on non-record` on BOTH backends (C and LLVM — so
not a backend bug). The trigger: `let z = 2.0 + complex.mk(0.0, 3.0)`
followed by `z.re`. `Real + Complex` is a heterogeneous protocol op
whose impl returns `Complex`; reading `z.re` crashed.

## Root cause — a latent typer bug the unboxing pass turned fatal

The protocol is `protocol Add[a] { add(self: Self, rhs: a) : Self }` —
return `Self`. The typer desugars `2.0 + complex` to a call of the
generic dispatcher `__proto_add(self: Self, rhs: a) : Self`; unifying
`self = Real` pins `Self = Real`, so the binop is typed **Real**. But
the selected impl `impl Add[Complex] for Real { add(...) : Complex }`
returns **Complex**. The typer always mis-typed this — `--check` has
reported `type mismatch in arithmetic operator '+'` since the fixture
was authored (#180, commit `08b3bd2`).

It was **harmless until unboxing landed**. Verified by rebuilding
`08b3bd2`: with `--prelude` (the era's invocation) the fixture printed
the golden `2 3 2 3 3 6`. The emitted C stored `z` as `KaiValue*
kai_z = kai___pimpl_Add_Real_add__a_Complex(...)` — boxed. The typer
believed "Real" but everything was a boxed pointer, so `z.re` read a
real record by accident and worked.

Then Phase 2/3 unboxing (post-#180) started trusting the inferred
type to choose representation: "z is Real, Real is unboxable" →
`double kair_z = (...)->as.r`. Now the Complex record is read as a raw
`double`, and `z.re` looks for a field on a scalar → crash. The
unboxing pass converted a latent type-inference bug into a memory bug
— the same shape as the LLVM-TCO lane (a value mistyped/raw where it
should be boxed).

Two layers, both real:
1. The fixture had no `import` (it relied on the retired `--prelude`),
   so the module did not even load — added `import math.complex`.
2. The typer mis-typed the result (this lane's fix).

## The principle: promotion is what the impl already declares

The owner's framing: an op between two compatible types yields the
"higher" type (`Int → Real → Complex`). The key realisation is that
**this hierarchy is already encoded in the heterogeneous impl** — `impl
Add[Complex] for Real` returns `Complex` precisely because Complex is
the promotion target. No new type-hierarchy concept is needed; the
typer just has to honour the impl's declared return instead of forcing
`Self`. So the fix reads existing data, it does not redesign the
language surface.

## Where the fix lives, and why not in the typer

First attempt: override the result type inside `synth_binop_proto`
(re-run the impl lookup, take its return). It kept panicking with
`non-exhaustive match` deep in the pipeline. lldb traced it to a
malformed list reaching `eff_op_names_eq` via the lookup chain —
`synth_binop` runs at a point where re-running the dispatch lookup hits
data in a state that breaks it. Wrong place.

The right place is the **post-inference protocol-call rewrite**
(`resolve_protocol_calls_expr`, m12.8). Verified phase order in the
driver: `typer → proto-rewrite → monomorph → unbox`. The rewrite
*already* finds the concrete impl and redirects the call to its
`__pimpl_*` symbol — in a phase with clean data. It just was not
correcting the node's type. The fix: after the rewrite produces the
`__pimpl_*` call, look the impl's declared return head up by its
mangled symbol and re-stamp the node's `.ty` when it differs from the
dispatcher-derived type. Because this runs **before** unboxing, the
unbox pass then sees `Complex` (boxed) and emits a `KaiValue*` load,
not a raw `double`.

## Implementation

- `ProtoImplReg` (`ast.kai`) gained a 6th field `ret_head` — the head
  TyName of the impl method's declared return. Populated in
  `lower_impl_methods` (`protos.kai`) from the impl's `ret` TypeExpr
  via `proto_type_name`. ~6 PIR match sites updated to the new arity.
- `resolve_protocol_calls_expr` (`protos.kai`) now computes
  `proto_rewrite_corrected_ty`: when the rewritten kind is
  `ECall(EVar(sym), _)` and `sym` is an impl's mangled name whose
  `ret_head` differs from the node's current head, the node's `.ty`
  becomes `TyCon(None, ret_head, [])`. Helper `impl_ret_head_by_sym`
  finds the impl by symbol.

Homogeneous ops are untouched: when the impl's return equals the
receiver head (`Int + Int → Int`, `Complex + Complex → Complex`),
`ret_head == cur_head` and the type is left as-is.

## Fixtures

- `examples/stdlib/complex_heterogeneous.kai` — added the missing
  `import math.complex`; the fixture now produces its golden
  `2 3 2 3 3 6` on both backends (it had never passed since
  `--prelude` was retired).

## Verification

- C and LLVM both print `2 3 2 3 3 6`.
- No regression on `Int + Int`, `Real + Real`, `Complex + Complex`.
- Selfhost deterministic (kaic2b.c == kaic2c.c).
- `make -k test`: complex_heterogeneous now passes; full suite checked.

## Follow-ups

- The `--check` path (`check_program` in resolve.kai) reports
  `proto_impls` as empty during binop inference, so `--check` alone
  still shows the old `type mismatch` for heterogeneous ops. The build
  path (which carries the impls) now types and runs correctly. Aligning
  `--check`'s impl table with the build path is a separate, smaller
  lane — it affects only the `--check` diagnostic, not compiled output.
