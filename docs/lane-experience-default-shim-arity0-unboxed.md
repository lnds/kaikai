# Lane experience — arity-0 scalar-return impls register RAW (default-shim)

## Scope as planned vs. as shipped

**Planned (from the 2026-06-03 bugfix handoff):** the CI red
`default-shim FAIL: Default_Int_default registered RAW (scalar fn
bitcast as %KaiValue*()* — the clang verifier mismatch that broke
v0.85/0.86 releases)`. Flagged as "hard — emit_llvm ABI territory."

**Shipped:** the root cause was upstream of emit_llvm, in the
unbox-signature classifier (`classify_unbox_sig`, `stage2/compiler/
fnreg.kai`). A one-line gate excluded every arity-0 fn from unboxed
classification, so `Default_Int_default() : Int` never got the
`UFnSig` stamp that drives the boxed-shim path. The emit_llvm
machinery for the shim already existed and was correct; it just never
fired because the classifier handed it `None`.

## Root cause

`classify_unbox_sig` decides whether a fn gets `Some(US(param_tys,
ret_ty, raw_mask))` (unboxed candidate) or `None` (classic boxed
convention). Its eligibility gate was:

```
else if not any_param_unboxable(params) { None }
```

For an arity-0 fn (`params == []`), `any_param_unboxable([])` is
`false`, so `not false` is `true` → `None`. The fn is never classified
unboxed.

Downstream, `pimpl_row_is_unboxed` already checks the *correct*
condition — `any_param_scalar(pts) or ty_is_scalar(rt)`, with the
return-only branch explicitly there for arity-0 `Default` impls — but
it reads the `UFnSig` the classifier stamped. With `None` stamped, it
returns `false`, and `llvm_emit_impl_register_calls` registers the raw
`i64 ()*` fn instead of a `%KaiValue*(...)` boxed shim. The impl table
calls every entry as `%KaiValue*(...)`, so clang's LLVM verifier
rejects the `i64 ()*`→`%KaiValue* ()*` bitcast. (The C backend got an
implicit cast and never caught it — only the LLVM smoke test does,
which is why this class broke release builds, not local C tests.)

## Fix

Widen the gate so an arity-0 fn with a scalar return is also an
unboxed candidate:

```
let arity0_scalar_ret = list_length(params) == 0 and ty_is_unboxable_t(ret_t)
if any_param_unboxable(params) or arity0_scalar_ret { Some(US(...)) } else { None }
```

This matches `pimpl_row_is_unboxed`'s own rule (scalar param OR scalar
return). `Default_Int_default` / `_Real_` / `_Bool_` now stamp a
`UFnSig`, route through the `__boxed` shim, and register a uniform
`%KaiValue*(...)` entry. `Default_String_default` stays RAW — correct,
since `String` is not a scalar, so its signature is already
`%KaiValue* ()` and needs no shim (the gate only asserts Int/Real/Bool).

## Design decisions / alternatives considered

Two scopes weighed (asu architect consult):

- **(A) widen the general gate** — chosen. Any top-level arity-0
  scalar-return fn (`fn answer() : Int = 42` too, not just pimpls)
  becomes an unboxed candidate. Closes the bug-class rather than the
  one fixture.
- **(B) special-case `__pimpl_` names** — rejected as debt: it
  re-introduces the very asymmetry that caused the bug (C tolerates,
  LLVM does not) as a new one (pimpls shimmed, plain fns not), in a
  classifier that is general by design, and the LLVM gate only covers
  pimpls — so a plain `fn answer() : Int` would still register raw by
  some other path.

**Soundness check for (A):** the architect flagged the risk that a
plain arity-0 scalar-return fn taken as a first-class value (function
pointer / closure) would be called with the boxed convention. Verified
that path is already covered: `emit_fn_thunk`'s UFn branch
(`Some(US(...))`) marshals args and wraps a raw return via
`ufn_call_result`. For arity-0 that degenerates to "call with no args,
box the return" — the simplest case of machinery that already runs for
every param-unboxed fn. The classifier also already excludes the cases
that *would* be unsafe (FFI extern, proto-dispatch shims, effectful
bodies, lambda-bearing bodies, tparams, `main`). So both direct calls,
first-class-value thunks, and the impl-table boxed shim handle the
arity-0 unboxed fn uniformly.

## Fixtures

No new fixture — `examples/protocols/default_basic.kai` and the
`default-shim` assertions inside `test-proto-scalar-dispatch` already
encode the bug: emit LLVM, grep that `Default_{Int,Real,Bool}_default`
register via `__boxed`, not raw. Now passes
(`arity-0 Default boxed-shim OK`). Verified end-to-end on LLVM: shims
defined, clang compiles clean, binary prints the golden `0 / 0 /
false / []`. The C backend path (`proto-scalar-dispatch OK (C)`)
already passed and still does.

## Coverage gaps

The gate asserts only `Default_*`. A *user* protocol with an arity-0
scalar-return op would ride the same fixed classifier branch, but no
fixture exercises a user-declared arity-0 scalar op. Low priority — the
mechanism is protocol-agnostic (it keys off the signature shape, not
the protocol name).

## Follow-ups

The widened classifier now stamps `UFnSig` on plain arity-0
scalar-return fns project-wide (e.g. any stdlib `fn k() : Int`). The
full test suite is the guard that none of those regressed under the new
unboxed calling convention; if green, no follow-up. The honesty note:
this slightly grows the set of fns that travel raw, which is a (small)
perf win, not just a correctness fix.
