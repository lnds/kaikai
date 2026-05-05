# Lane experience — issue-180-protocol-pa

## Objective metrics

- Started: 2026-05-05T08:42:41-04:00
- Ended: 2026-05-05T11:09:15-04:00
- Wall-clock: ~2h 27m (calibrated estimate was 6-7h × 0.6).
- Stage 2 compiler diff: ~+440 / -50 lines.
- Stdlib diff: protocols.kai +18 / -8, complex.kai +40 / -1.
- New fixtures: 4 protocol + 1 sugar.
- All build gates green: Tier 0, Tier 1, Tier 1-ASAN, selfhost-C
  byte-identical, selfhost-LLVM byte-identical.

## Diagnosis (parser + AST + resolver/typer sites)

- Parser: `parse_protocol_decl` rejected `[A]` after the protocol
  name; `parse_impl_decl` already accepted `[u: Measure]` BEFORE the
  protocol name (m12.8 phase 2 impl-tparam list) — distinct slot, no
  conflict.
- AST: `DProtocol(Bool, String, [ProtoOp], Int, Int)` and
  `DImpl(String, TypeExpr, [Decl], Int, Int)` referenced at 59 sites
  (`grep -c DProtocol\\\|DImpl`). Most are `(_, _, _, _, _)`
  catch-alls; ~14 are real consumers.
- Resolver / typer: protocol method scheme generation goes through
  `make_dispatcher_fn` → `substitute_self_in_type`. `Self` becomes
  `proto_self`; uppercase tparams (`A`, `Other`) would NOT be picked
  up by `is_implicit_tparam_candidate` (checks `ch_is_lower(c)`).
  Documentation choice: lowercase `a` for the protocol's `[a]`
  parameter (the issue uses `A` informally; the implementation uses
  `a`).
- `synth_binop` already gated user-type protocol routing on
  `binop_should_dispatch_proto` (both operands non-prim). That
  predicate is the entry point for #180's heterogeneous extension.
- Post-inference rewrite (`try_rewrite_proto_call` in
  `resolve_protocol_calls_expr`) used `args[0].ty` as the only Self
  informant — needed extension for both heterogeneous binops (look at
  args[1] when arg_names are involved) and bidirectional inference
  (consume the call's surrounding return type when args carry no
  Self).

## Algorithm (P[A] representation; bidirectional inference; binop heterogeneous)

### AST representation

`DProtocol` carries an extra `[String]` (tparam list, lowercase per
kaikai convention). `DImpl` carries an extra `[TypeExpr]` (proto-args
monomorphised at impl-site). Empty lists preserve legacy semantics:
`protocol Show { ... }` and `impl Show for Int { ... }` parse and
route unchanged.

### Impl table

`ProtoImplReg` extended with `[String]` (proto-arg head names). Mangled
function name is `__pimpl_<P>_<T>_<op>` for the homogeneous case
(`arg_names == []` or `[T]`) and
`__pimpl_<P>_<T>_<op>__a_<argname>...` for heterogeneous cases. This
keeps existing impls byte-identical at the C level — selfhost stays
green without re-mangling the bootstrap symbols.

`ProtocolReg` gains `tparams: [ProtoTPReg]` recording per-protocol
tparam declarations. `find_proto_impl_for_args` checks this list to
decide whether a 2-arg op's second argument is a proto-arg
(parametrised protocol) or just an op-shape parameter (`Numeric
.pow_int(Self, Int)`).

### Heterogeneous synth_binop

`InferState` carries `proto_impls: [ProtoImplReg]` snapshot installed
at infer-decl time. `binop_dispatch_proto_heterogeneous` returns true
when:

1. The pair is `(prim, nonprim)` / `(nonprim, prim)` / `(nonprim,
   nonprim with distinct heads)`.
2. AND a matching `impl P[a] for T` (or its mirror) is registered.

When true, `synth_binop` routes to `__proto_<op>(l, r)`. The
post-inference rewrite then walks both arguments and the return
type (see below) to pick the correct impl.

### Bidirectional inference

`try_rewrite_proto_call_with_ret` consults the call's
surrounding return type when args alone don't pin Self. The typer's
existing SLet-annotation unify pass already pins the return type by
the time the rewrite runs, so the head name of `e.ty` flows in as
the receiver name. This handles `let eur : Money[EUR] = from(usd)`
without requiring a new check-style typer pass.

`proto_op_self_only_in_return(params, ret)` short-circuits the
arg-driven dispatch when no parameter mentions `Self` (e.g.
`from(x: a) : Self`). Without it, single-Self-in-return ops would
incorrectly bind Self to args[0]'s head and pick the wrong impl
when multiple impls share the source type.

## Stdlib migration shape

`stdlib/protocols.kai`:

```kai
protocol Add[a] { add(self: Self, rhs: a) : Self }
impl Add for Int  { fn add(self: Int, rhs: Int) : Int = self + rhs }
impl Add for Real { fn add(self: Real, rhs: Real) : Real = self + rhs }
```

The legacy `impl Add for Int` parses as `impl Add[Int] for Int`
(arg_names == [] treated as homogeneous default). Same for Sub /
Mul / Div.

`stdlib/math/complex.kai` adds 7 heterogeneous impls:
- `Add[Real] for Complex`, `Add[Complex] for Real`
- `Sub[Real] for Complex`, `Sub[Complex] for Real`
- `Mul[Real] for Complex`, `Mul[Complex] for Real`
- `Div[Real] for Complex`

`Div[Complex] for Real` is intentionally omitted (would need
conj/abs_sq; not exercised by the headline test).

## Empirical verification

```kai
fn main() : Unit / Console = {
  let z = 2.0 + complex.mk(0.0, 3.0)        # Real + Complex
  let w = complex.mk(0.0, 3.0) + 2.0        # Complex + Real
  let v = complex.mk(1.0, 2.0) * 3.0        # Complex * Real
  print(real_to_string(z.re))   # 2
  print(real_to_string(z.im))   # 3
  print(real_to_string(w.re))   # 2
  print(real_to_string(w.im))   # 3
  print(real_to_string(v.re))   # 3
  print(real_to_string(v.im))   # 6
}
```

Output: `2 3 2 3 3 6` — matches the issue's headline expectation.

Currency fixture (`examples/protocols/proto_pa_currency.kai`):

```kai
let u = Usd { cents: 1000 }
let e : Eur = from(u)        # 920 (USD * 0.92)
let g : Gbp = from(u)        # 790
let u2 : Usd = from(e)       # 1002
```

`Self` resolves from the let-binding annotation; output: `920 790
1002`.

## Friction points

1. **Method-name shadowing inside the registry.**
   `__pimpl_Add_Complex_add` exists for both `impl Add for Complex`
   and (hypothetically) `impl Add[Complex] for Complex`. The
   `proto_impl_mangled_with_args` distinguishes the heterogeneous
   case via the `__a_` suffix; the homogeneous + legacy case shares
   one symbol. This was deliberate (preserves selfhost byte-identity)
   but it surfaced in `find_impl_for` returning the WRONG entry when
   the heterogeneous impl was registered first. Fix: explicit
   `is_homogeneous_args` predicate plus separate Pass 1a / Pass 1b
   in `find_proto_impl_for_args`.

2. **Op-shape parameters confused with proto-args.**
   `Numeric.pow_int(Self, Int)` is single-dispatch with a normal `Int`
   second parameter. Without `proto_has_tparams`, `find_proto_impl_for_args`
   tried to interpret `Int` as a proto-arg and panicked at the
   dispatcher. Adding the per-protocol tparam-arity registry was the
   minimum precise fix.

3. **Selfhost convergence.** Each AST signature change ripples
   through the bootstrap chain (stage1 reads compiler.kai, then
   stage2 reads compiler.kai through stage1's emitter). Three full
   `make tier0` cycles were enough — no fixed-point divergence
   showed up. The `proto_impl_mangled_with_args` design (legacy mangle
   preserved) was load-bearing for this convergence.

4. **Bidirectional inference scope.** The implementation only honours
   let-binding annotations (the typer's existing unify hook).
   Function return positions (`fn convert(u: Usd) : Eur = from(u)`)
   would need a similar hook in `infer_decl`. This is documented as
   a follow-up in `docs/protocols.md`.

5. **The diagnostic "no impl of P for type T" still points at args[0]
   even when the failure is bidirectional-inference-related.** A
   user who writes `let e = from(u)` (no annotation) sees "no impl of
   `From` for type `Usd`" rather than "Self could not be inferred at
   this call site; add an annotation". Improving the diagnostic is a
   follow-up.

## Subjective summary

The lane shipped cleanly within the 0.6× calibration estimate. The
design pin "impl table key remains `(P, Self)`" was crucial — it
meant the dispatch infrastructure barely changed, and the heaviest
lifting moved to the registry and the rewrite passes that were
already touching every call site.

The biggest surprise was how much the existing typer infrastructure
already supports bidirectional inference: the SLet-annotation unify
hook had been carrying the expected type into return-type position
for years; we just needed to read it after the fact.

## Limitations

- Polymorphic impl in `a` (`impl[T] From[T] for X`) is not supported.
  Issue #174 tracks this.
- Bidirectional inference only consults let-binding annotations.
  Function return positions and explicit type ascriptions on
  sub-expressions are follow-ups.
- `#derive` does NOT support parametrised protocols in v1.
- The diagnostic for "Self could not be inferred" still
  reports as "no impl of P for arg-type" — needs a dedicated
  diagnostic pass.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-05T10:06:41-04:00	tier0	OK	-
2026-05-05T10:09:46-04:00	tier0	OK	-
2026-05-05T10:17:29-04:00	tier0	OK	-
2026-05-05T10:28:06-04:00	tier0	OK	-
2026-05-05T10:32:06-04:00	tier0	OK	-
2026-05-05T10:56:38-04:00	tier0	OK	-
2026-05-05T11:06:52-04:00	tier1	OK	-
2026-05-05T11:07:51-04:00	tier1-asan	OK	-
2026-05-05T11:09:03-04:00	selfhost-llvm	OK	-
```
