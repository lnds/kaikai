# Lane experience — issue #893 (multi-protocol op-name collision segfault)

## Scope as planned vs as shipped

**Planned (per #893):** make a bare protocol-op call inside `fn f[T : P](...)`
stop segfaulting when the op name is declared by more than one protocol —
specifically the `add`/`mul` case that blocks the `sum`/`product` →
`[T : Numeric]` half of #891. The issue's diagnosis pointed at
`try_rewrite_proto_candidates` picking the wrong impl and at the boxed-vs-raw
ABI mismatch.

**Shipped:** the root cause was one layer earlier than the issue's writeup,
and there were TWO distinct bugs, not one. Both are fixed:

- **Bug A (dominion of the shadow guard).** The bare name `add` was bound in
  the type env to a *concrete* `(Complex, Complex) -> Complex` scheme, not to
  the polymorphic `__proto_add` dispatcher. `stdlib/math/complex.kai` defines
  free helpers `pub fn add` / `pub fn mul` (named-arithmetic API beside the
  `impl Add for Complex`). The Hanga Roa shadow guard
  (`filter_shadowed_ops` + `collect_local_fn_arities`) dropped the `add` op
  from the rewrite-to-dispatcher pass whenever ANY top-level fn shared its
  `(name, arity)` — but it collected fns across the WHOLE program, including
  prelude modules. So `complex::add` shadowed the `add` op program-wide; the
  user's `pair_add[T:Add]` body bound the concrete `Complex` helper, `T`
  unified to `Complex`, and the post-mono rewrite emitted a call into the
  `Complex` impl with `Int` 3/4 → raw-untag-as-pointer fault. The issue's
  "`try_rewrite_proto_candidates` picks `Add`'s impl" framing was a downstream
  symptom of this env binding, not the cause.

- **Bug B (one dispatcher per name, not per signature).** A name declared by
  two protocols at DIFFERENT arities (`P { f(x) }` + `Q { f(a, b) }`) produced
  a single `__proto_f` dispatcher (`generate_dispatchers` deduped by name),
  whose signature was whichever POR registered first. A 1-arg call then failed
  typecheck ("`__proto_f` expects 2 arguments, got 1"). The issue named this
  obliquely ("structurally distinguishable") but its repro only exercised the
  same-arity `add`/`mul` case, which Bug A masked.

## Fix shape chosen and why

asu's consult (recorded in session) framed the choice. Two independent fixes,
NOT one "overload-aware resolver" — that path is constraint propagation, which
Tier 1.3 forbids.

**Bug A — scope the shadow to the declaring module (the preferred shape, not
the fallback).** `collect_local_fn_arities_mo` keeps each fn's `mo`; the rename
pass filters the shadow set per-decl so a fn shadows the op only inside its own
module. `complex::add` shadows `add` in complex's bodies; the user's root-file
`add(a,b)` reaches `__proto_add`. Root file is `mo == None`, matching only
other `None` decls — which is exactly the `fn show(o: Option)` case the guard
was built to protect, so that case strengthens rather than breaks
(`test-shadowing` stays green). This is the issue's "leave the runtime
dispatcher" outcome reached structurally: once the bare name resolves to the
dispatcher again, the existing monomorphisation re-run specialises it to the
right raw-ABI `__pimpl_<P>_Int_<op>` exactly as the single-candidate `cmp`
oracle does.

**Bug B — dispatcher per `(name, arity)`, suffixed only when ambiguous.**
`proto_dispatcher_name_at` appends `__a<N>` only when a name is declared at
two different arities (`op_name_arity_ambiguous`). The common case (every
stdlib op name is unique to one protocol; same-arity collisions like
`Ring.add` vs `Add.add` share one dispatcher and disambiguate by
`head_tag(self)` at runtime) keeps the bare `__proto_<op>` symbol, so selfhost
stays byte-identical. The post-rewrite candidate search
(`try_rewrite_proto_candidates`, `try_rewrite_ret_candidates`) gained an arity
guard so a 1-arg call never lands on the 2-arg impl.

Rejected alternatives (asu): a runtime shim branching on arity (pollutes the
`O(1)` dispatch, moves compile-time work to runtime); the typer choosing among
candidate schemes by arg count (constraint propagation). Confirmed kaikai has
no free-fn arity overloading, so two `__proto_f` DFns are impossible — the
arity suffix is what gives each its own symbol.

## ABI: how the raw-vs-boxed mismatch closed

Verified in the emitted C, not just at the source. Before: `pair_add__mono__Int`
called `kai_complex__add(kai_a, kai_b)` (the wrong impl, boxed shim, raw args).
After: it calls `kai_int(kai_protocols____pimpl_Add_Int_add(kai_intf(kai_a),
kai_intf(kai_b)))` — the raw `int64_t` impl with `kai_intf` unbox / `kai_int`
rebox, matching the impl's emitted signature. The `__proto_add` dispatcher is
present in the binary again. The fix is in the typer/rename; it lands before
backend selection, so C-direct and native both inherit it (both segfaulted
identically pre-fix, both return the scalar post-fix).

## Secondary gaps (named in #893) — both needed, both closed

- **`Numeric` absent from the emitter allowlist.** Added `KAI_PROTO_NUMERIC 12`
  to both `stage0/runtime.h` and `stage2/runtime.h` (the two-runtime-copy trap),
  and to `stdlib_proto_id_c` / `stdlib_proto_id_int` / `pimpl_split_proto`. The
  unique-name `abs[T:Numeric]` case worked without it (static rewrite never
  touches the impl-table for concrete types), but a Numeric op that stays
  polymorphic past mono would have had `proto_id = -1` and never matched in
  `kai_lookup_impl`. Closing it makes Numeric a first-class dispatchable
  protocol like Add/Mul.

- **`validate_proto_call_at` single-result blind spot.** Replaced
  `find_impl` (which fixes the protocol via single-result `proto_op_lookup`)
  with `find_impl_any_proto`, which accepts an impl under ANY protocol
  declaring the op. Without it the post-rewrite validator could flag a spurious
  "no impl" by checking the wrong protocol. The negative fixture
  (`String` has no `Ring` impl) confirms the diagnostic still fires correctly
  and names the right protocol.

## General case proven beyond add/mul

The fix is not special-cased to `add`/`mul`. Three fixtures exercise the
general multi-protocol-op-name machinery:

- `multi_proto_op_pair_add` — the #893 repro (`pair_add[T:Add]` /
  `pair_mul[T:Mul]`) over Int and Real.
- `multi_proto_op_ring` — the #891 collided-name ring (`add`/`mul`/`zero`/`one`),
  `sum`=10 / `product`=24 over Int AND Real, nullary ops via Self-in-return.
- `multi_proto_op_arity_disambig` — a synthetic `P { f(x) }` + `Q { f(a,b) }`
  with no name shared with stdlib, proving arity disambiguation in isolation.
- `multi_proto_op_no_impl` (negative) — the collision-aware validator's
  clean rejection.

## Structural surprises the brief did not anticipate

1. The bug fired even with NO bound (`fn pair_add[T](a, b) = add(a, b)`):
   the `[T : Add]` bound from #890 was irrelevant. The env binding of the bare
   name is the whole story, so the fix lives in name resolution / the rename
   pass, not in bound handling or monomorphisation.
2. The issue said the call was rewritten to the boxed `__pimpl_Add_Int_add`
   shim; it was actually rewritten to `kai_complex__add` (the wrong impl
   entirely). The boxed-shim detail was a plausible reconstruction; the real
   emitted symbol was worse and pinpointed the env binding.
3. `KAI_USER_PROTO_ID_BASE` exists but no dynamic user-proto-id assignment is
   wired — non-builtin protocols genuinely get `proto_id = -1`. So Numeric had
   to join the static allowlist; there was no "just register it dynamically"
   path.

## Real cost vs estimate

Diagnosis was the bulk: `--env` / `--dump-typed` isolated the concrete-scheme
binding quickly once compared against the `cmp` oracle. Two small, independent
code changes plus three mechanical allowlist edits. No InferState threading, no
new pass, no constraint propagation.

## Follow-ups left for next lanes

- #891's `sum`/`product` → `[T : Numeric]` half is now unblocked: the clean
  `add`/`mul` names dispatch correctly. That lane can resume.
- The arity-suffix naming (`__proto_<op>__a<N>`) only appears when a program
  actually declares an arity-ambiguous op name; no stdlib name triggers it
  today, so it is dormant until user code needs it. If a future stdlib protocol
  ever reuses an existing op name at a new arity, the suffix activates
  automatically.
