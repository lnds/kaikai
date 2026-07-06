# Lane experience — #1095 `#[constructor]` sugar

Implement the `#[constructor]` attribute (design in
`docs/constructor-sugar-design.md`, accepted in #1087) so numeric types
construct as `Decimal(10)`, `Rational(2, 3)`, `BigInt(5)`,
`Complex(3.0, 4.0)` — a call to the marked fn, never the raw record
literal. The author marks one `pub fn` returning the type; `Type(args)`
desugars to `home_module.marked_fn(args)`, passing the fn's logic
(canonicalisation, invariants, `priv` fields).

## Scope as planned vs as shipped

**Planned**: parse `#[constructor]`, enforce one per type, desugar
`Type(args)` to a call, mark the five numeric constructors from the
issue table (`rational.make`, `decimal.from_int`, `decimal_big.from_int`,
`bigint.from_int`, `complex.mk`).

**Shipped**: all of that. One structural fact the brief under-stated: the
desugar had to intercept an existing *live-but-broken* path, not fill an
empty one. Today `Rational(2, 4)` (parens, not braces) already parses as
`ECall(EVar("Rational"), args)` and rides all the way to the C backend as
a dangling `kai_apply(kai_Rational, …)` reference to a symbol that does
not exist — a C compile error, not a record literal. The record
positional sugar is brace-form (`Foo{a, b}`, handled by
`desugar_pos_records`); paren-form on a type name was never valid. So the
sugar *rescues* a call shape that otherwise dead-ends. `Rational`,
`Complex` (records) and `BigInt` (sum) all reach the typer uniformly as
`ECall(EVar(TypeName), args)` — no record/sum split needed.

## Design decisions

Three load-bearing choices, decided against a language-architect consult.

**1 — Registry home: a field on `TyEnv`.** The registry maps
`type_name -> (home_module, fn_name)`. Candidates were: a field on
`TyEnv`, a synthetic env entry under a reserved key, or a side-table
threaded through `InferState`. `InferState` has 33 construction sites,
`TyEnv` 18; the reserved-key trick cannot recover `(mod, fn)` from a
scheme (the scheme carries neither). `TyEnv` is the natural home — it
already carries cross-module metadata lists (`unions`, `root_fns`,
`row_kind`) and travels inside `InferState.env`, so no `InferState` site
changes. The field is `constructors: [CtorEntry]`, populated in
`add_decls_loop_rk` where the `mo` (module origin) is already stamped.

**2 — Wrapper lifecycle: a transparent `DConstructor` wrapper.** The
architect leaned toward a bit on `DFn` (a missing arm breaks the build —
loud — whereas a missing wrapper arm falls silent). But a `DFn` field
touches 202 sites across 32 files; the `DUnstable` wrapper precedent is
99 sites across 17. The wrapper is the idiomatic kaikai form (three
attribute wrappers already exist: `DDerive`, `DUnstable`, `DDoc`), and
`DConstructor` mirrors `DUnstable` exactly — same shape, same
pass-through-by-recursion discipline in every walker, same cache tag
scheme (tag 18), same emitter recursion. Choosing the wrapper meant
paying the sweep: a `DConstructor` twin next to every `DUnstable` arm.
The silent-failure risk the architect flagged is real — kaikai's `Decl`
matches carry catch-alls, so a missing arm does not fail the build; it
`panic`s at runtime on the first `#[constructor]` file. The mitigation is
mechanical completeness (mirror every `DUnstable` arm) plus the fixtures,
which exercise the sugar end-to-end and catch a dropped arm as a panic.

**3 — Rewrite point: `try_constructor_call` in `synth_call`.** The
existing `ctor_check_or_synth` rewrites `EVar(cname) -> EModCall(parent,
cname)` but pulls `parent` from the *expected* type — unavailable for a
bare `let r = Rational(2, 3)`. `synth_call` pulls the type name from the
*callee*, so the sugar works unannotated. Inserted between
`try_ufcs_call` and `try_bare_call_narrow`; it looks up the callee name
in `env.constructors`, rewrites to `EModCall(home, fn)(args)`, and
re-runs `synth`. Because the rewrite happens before arg inference, the
arg literals mint against the marked fn's param types (#1091): `Rational
(2, 3)` mints `2`/`3` to `BigInt` via `make(num: BigInt, den: BigInt)`.

## Enforcement

One `#[constructor]` per type. `add_decls_loop` accumulates every marked
binding (duplicates included) — it is a pure fn with no diagnostic
capability. `validate_constructor_uniqueness_decls` (driver-invoked,
alongside the other collision validators) walks `merged`, groups by the
constructed type's bare name, and reports the second occurrence, mirroring
`validate_type_name_collisions`.

## The wrapper-lifecycle trap — five integration bugs the fixtures caught

The mechanical `DUnstable`-mirror sweep produced a compiler that *built*
but *panicked* on the first `#[constructor]` file, then — once that was
fixed — mis-compiled in four further ways. Each was the same root shape:
`DUnstable` and `DConstructor` look identical but have opposite
lifecycles. `DUnstable` is **unwrapped early** (in `expand_derives`,
before the typer) because its info was already collected in the driver;
`DConstructor`'s registry is collected **in the typer**, so it must
**survive** to the typer, then unwrap. Blindly mirroring `DUnstable`'s
"unwrap to inner" arms destroyed the wrapper before the typer saw it.

The five bugs, in the order verification surfaced them:

1. **`expand_derives` unwrapped it.** The `DConstructor(inner) ->
   expand_derives_loop([inner, ...rest], ...)` arm (copied from
   `DUnstable`) flattened the wrapper in the *first* pass of
   `lower_protocols`, so it never reached the typer — the registry stayed
   empty (`nregs=0`). Fix: preserve the wrapper (`-> loop(rest, [d,
   ...acc])`).

2. **`#[doc]` stacking buried the fn.** `#[constructor]` over a
   `#[doc(...)]` parsed as `DConstructor(DDoc(DFn))`; `doc_strip_collect`
   only lifts a *top-level* `DDoc`, so the inner one stayed trapped and
   every walker expecting `DConstructor(DFn)` (registry, exports) missed
   the fn. Fix: the parser hoists the `DDoc` outside —
   `DDoc(DConstructor(DFn))`.

3. **`ModuleEnvDelta` didn't carry it.** The registry is per-module; the
   root file resolves `Rational(...)` against the *inherited* delta. The
   delta had no `constructors` field, so cross-module bindings never
   reached the call site. Fix: add the field + thread it through
   `merge_env_deltas`, the delta build, and `build_ty_env_inherited_rk`.

4. **The resolver dropped the fn name.** `register_one` /`chk_decl` had
   `DConstructor(_) -> e` (no-op, mirroring `DUnstable`), so a
   *same-module* call like `from_real` → `mk(...)` failed with "cannot
   find `mk`". Fix: unwrap to register/check the inner fn.

5. **The emitter emitted no symbol.** Once the wrapper survived to the
   emitter, `kai_complex__mk` was never generated — the fn emitter has no
   `DConstructor` arm. Fix: unwrap `DConstructor` to its typed `DFn` at
   the *end of the typer* (`infer_decl` returns the inner directly), so
   no post-typer pass ever sees the wrapper. This is the clean seam: the
   marker lives from parse through the typer (where its registry is
   read), then dissolves.

The lesson: a wrapper's lifecycle is defined by *when its payload is
consumed*, not by its shape. `DUnstable` and `DConstructor` share a shape
and share zero lifecycle. The sweep must not be blind. Byte-id alone
would NOT have caught bugs 1–4 (they error before codegen); the
end-to-end fixtures did.

## Call-arg minting is not automatic — the sugar mints explicitly

The issue assumed `Rational(2, 3)` would mint `2`/`3` to `BigInt` "via
literal minting (#1091)". But #1091 mints only in *annotated let* and
*record-literal* position — **positional call args are not minted**.
`rational.make(2, 4)` written by hand fails the same way (`(Int, Int)`
vs `(BigInt, BigInt)`). So the sugar mints its own args: after resolving
the marked fn, `try_constructor_call` reads its (concrete) scheme's param
types and applies `mint_literal` to each arg before building the call.
This keeps the issue's promised surface working without depending on a
general call-arg minting pass that does not exist.

## Fixtures

`examples/attributes/` (the `test-attributes` harness — `.out.expected`
positive, `.err.expected` first-line-needle negative):

- `attr_constructor_basic` — `Rational(2, 4)` prints `1/2` (proof the
  call ran `make`'s gcd, not a raw `2/4` literal); `Decimal(10)`,
  `BigInt(5)` route through `from_int`.
- `attr_constructor_dup` — two `#[constructor]` fns returning `Meters`;
  rejected.
- `attr_constructor_arity` — `Rational(5)` (one arg) against
  `make(num, den)`; arity error. Integer-to-rational stays the named
  `rational.from_int(5)`.

## Cost vs estimate

The mechanical `DUnstable`-mirror sweep dominated — ~60 arms across 17
files, most one-liners, but each a silent-panic risk if missed. The
non-mechanical work (registry, rewrite, enforcement, `infer_decl` body
arm) was small and localised.

## Follow-ups

- `#[constructor]` is general; this lane scopes it to the numeric
  surface. Applying it elsewhere (e.g. a `priv`-field ADT) is a later
  opt-in per type.
- The `infer_decl` no-arm-for-wrappers gap exists for `DUnstable` too;
  only `#[constructor]` needed the fix. Worth a dedicated arm audit if a
  future `#[unstable]` fn carries a non-trivial body.
