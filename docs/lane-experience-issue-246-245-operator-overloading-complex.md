# Lane retro — `issue-246-245-operator-overloading-complex`

Combined fix for #246 (operator overloading via protocol dispatch) and
#245 (`stdlib/math/complex.kai`). Issues bundled because Complex is the
canonical user of the operator-overloading rewrite, and shipping them
together gives an end-to-end exercised shape.

## Objective metrics

- Branch lifetime: ~25 minutes (start `2026-05-04T23:57Z`, end
  `2026-05-05T00:22Z`).
- Code added: typer rewrite (~75 lines in `stage2/compiler.kai`),
  `stdlib/math/complex.kai` (~80 lines), 4 protocols added to
  `stdlib/protocols.kai` (~60 lines including primitive impls), 3 lines
  in `bin/kai`, 1 line in `stage2/Makefile`.
- Fixtures: 4 new (3 stdlib, 1 protocols), all dual-backend.
- Tier gates: tier0 + tier1 + tier1-asan + selfhost (C) + selfhost-llvm
  all green; selfhost byte-identical on first iteration on both
  backends.

## Diagnosis (binop site + protocol resolver)

`synth_binop` (`stage2/compiler.kai:25083`) is the single binop typer
site. It dispatches on the operator string, with three branches that
needed to know about protocol fall-through:

- `*` / `/` — routed to `synth_dim_mul_div` (UoM-aware path).
- `+` / `-` — `else` branch, plain unify-both-sides.
- `^` — out of scope (integer exponent only, no overloading).

The protocol pipeline is a three-step affair documented in
`stage2/compiler.kai:39820`-onwards:

1. `lower_protocols` (pre-typing) collects every `DProtocol` decl,
   synthesises a dispatcher fn `__proto_<op>` per registered op, and
   lowers each `DImpl` body into `__pimpl_<P>_<T>_<op>`.
2. `rename_proto_calls` rewrites bare `EVar(opname)` callees in the
   user program to `EVar("__proto_<opname>")` (arity-aware,
   shadow-aware).
3. `resolve_protocol_calls` (post-typing) rewrites
   `ECall(EVar("__proto_<op>"), [arg0, ...])` to
   `ECall(EVar("__pimpl_<P>_<T>_<op>"), ...)` based on `arg0.ty`.

The cleanest insertion point for binop overloading is *inside*
`synth_binop`: rewrite `EBinop` to `ECall(EVar("__proto_<method>"),
[l, r])` and recurse through `synth_call`. Steps 1 and 3 then apply
without any new machinery.

## Algorithm

```
synth_binop(op, l, r):
  type-check both operands as before
  if op ∈ {+, -, *, /} and binop_should_dispatch_proto(lty, rty):
    return synth_call(__proto_<method>(op), [l, r])
  else:
    keep the existing path

binop_should_dispatch_proto(lty, rty) :=
  ty_is_concrete_nonprim(lty) ∧ ty_is_concrete_nonprim(rty)

ty_is_concrete_nonprim(t) :=
  t = TyInt        → false
  t = TyReal       → false
  t = TyVarT(_)    → false   # generic — keep the existing diagnostic
  t = TyDimT(b, _) → recurse on b
  t = TyRefineT(b, _) → recurse on b
  t = TyCon(_, _)  → true
  otherwise        → false
```

Primitive `Int + Int`, `Real + Real`, and dim-wrapped numeric arithmetic
keep the unify-and-emit fast path — the predicate explicitly returns
false for them. Generic `T + T` bodies (both `TyVarT`) keep the previous
"requires Int/Real" diagnostic — heterogeneous and constrained-generic
arithmetic stay deferred to #180.

## Complex impl shape

`stdlib/math/complex.kai`:

- `add` / `sub` — componentwise.
- `mul(a, b)` — `(a.re*b.re − a.im*b.im, a.re*b.im + a.im*b.re)`.
- `div(a, b)` — `mul(a, conj(b)) / abs_sq(b)` (componentwise scalar
  divide). Stays as a named-only function: the operator path requires
  `Self * Self : Self` and `Self / Self : Self`, so a `Real`-divisor
  intermediate cannot surface through it.
- `abs_sq(a)` — `a.re² + a.im²`. No `abs` (sqrt) until
  `stdlib/math/real.kai` grows one; Mandelbrot uses `abs_sq > 4.0`
  directly.
- No transcendentals, no polar form (also blocked on sqrt / atan2).

## Empirical verification

```kaikai
let z1 = complex.mk(1.0, 2.0)
let z2 = complex.mk(3.0, 4.0)
let z3 = z1 + z2
print(real_to_string(z3.re))   # 4
print(real_to_string(z3.im))   # 6
```

`z1 * z2` prints `(-5, 10)`; `z2 - z1` prints `(2, 2)`; `complex.div(4+2i, 1+i)`
prints `(3, -1)`. Operator and named-call forms agree (pinned by
`complex_operator.kai`). The Mandelbrot escape grid renders as expected
ASCII.

## Friction points

- **Selfhost convergence.** Byte-identical on the first iteration on
  both backends. The typer change is local (one rewrite gate, three
  helper fns) and doesn't perturb the rest of the inference state, so
  the round-trip held without churn — the lane budget assumed up to 3
  iterations of selfhost convergence; spent zero.
- **`Show` collision in fixtures.** First draft of
  `complex_basic.kai` declared a local helper `fn show(label, c)`,
  which `rename_proto_calls` rewrote into `__proto_show` because the
  pre-existing `Show` protocol op claimed the bare name. Renamed to
  `print_complex`. The interaction is by design — the resolver-arity
  pass treats arity-2 `show` as a real protocol-op match — but it is
  worth flagging in the docs that `show` (1- or 2-ary) is reserved
  whenever `protocol Show` is on the prelude.
- **Generic `T : Add` paths.** Not exercised here. The predicate
  deliberately bails on `TyVarT` so generic bodies keep the old
  diagnostic; constraint-bounded generics await #180.
- **Issue #245 spec sanded.** `from_polar`, `arg`, `inv`, and `abs`
  are deferred until `real_sqrt` / `real_atan2` exist. The shipped
  module covers the canonical Mandelbrot use case; the remaining items
  belong to a follow-up tied to a sqrt landing.

## Subjective summary

A small, well-scoped lane that cleanly leveraged the m12.8 protocol
infrastructure already in place. The only non-obvious move was
choosing to do the operator → method rewrite inside the typer (after
each operand has its type inferred) rather than during a separate
pre-typing pass — that placement keeps the primitive shortcut the
default and the protocol path strictly opt-in based on the resolved
operand types. Selfhost convergence on the first iteration suggests
the rewrite touches no part of the inference state that the compiler's
own source relies on for arithmetic.

## Limitations

- No heterogeneous arithmetic (`Complex + Real`); needs #180.
- No constrained generics (`fn f[T : Add](a: T, b: T)`); needs #180.
- `Eq` / `Ord` operator integration (`==`, `<`, `<=`, ...) NOT done in
  this lane. The `==` / `<` paths in `synth_binop` already produce
  `EBinop` ASTs with built-in semantics; routing them through the
  protocol dispatcher would touch more of the runtime emit path. File
  as follow-up.
- `Div for Complex` ships only as a named call, not as an operator
  impl. The algebraic identity (`a*conj(b) / abs_sq(b)`) doesn't
  surface as `Self * Self : Self`.
- No transcendentals; no polar form.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-05T00:14:26-04:00	tier0	OK	-
2026-05-05T00:19:47-04:00	tier1	OK	-
2026-05-05T00:20:47-04:00	tier1-asan	OK	-
2026-05-05T00:21:23-04:00	selfhost	OK	-
2026-05-05T00:22:20-04:00	selfhost-llvm	OK	-
```
