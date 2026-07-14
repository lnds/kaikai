# Lane experience — issue #1208: ground a return-only tparam from the call site

## Scope as planned vs as shipped

**Planned.** The #1205 closure-spec lane reached parity on `vec.foldl` but
left the fully-idiomatic Vec pipeline at ~1.6x manual. The residual was
the **collect**: `let pts: Vec[Point] = [a..b] | stage` desugars to
`range_map_collect_vec[b, e](...) : Vec[b]`, whose element type `b` is a
**return-only** tparam — no argument pins it. Two things kept the loop
boxed: (1) `closure_spec` **declines** the collect (its `any_return_only`
gate), so the per-element indirect closure call survives; (2) even the
generic spec mono synthesises leaves `b` a tyvar, so the element stays
boxed. Fix: mono grounds a return-only tparam from the call site's
resolved return type, so `closure_spec`'s decline can relax and the
collect specialises to parity.

**Shipped.** Three coordinated changes, all inside the existing machinery
(the third surfaced only after the first two, see the surprises section):

- **Mono (the load-bearing half).** `synthesize_inst_for_decl` now unifies
  the callee's formal return against the call site's resolved return type
  (`ECall.ty`) in the SAME binding set the arguments pin — one fresh
  scheme instance, one merge. A return-only `b` in `Vec[b]` grounds to
  `Point` from an annotated `let v: Vec[Point] = …`. The `ret_ty` threads
  through `discover_call_tuple → resolve_call_inst → synth_inst_pair` and
  through the callsite rewriter (`rewrite_bare_call` /
  `rewrite_qualified_call`) so discovery and retarget mint the same
  mangled symbol. Purely additive: an unresolved (tyvar) `ret_ty` adds no
  binding.

- **Closure-spec gate refinement (not relaxation).** `hofn_specialisable`
  no longer declines on return-only alone. A return-only tparam is fine —
  mono grounds it. The unsound case is narrower: a return-only tparam the
  body resolves through an **arity-0 protocol op** (`zero()`/`one()`),
  whose result type IS that tparam and cannot be pinned from any datum. A
  new `body_has_arity0_proto_op` walk keys on an `ECall` with zero args to
  a `__proto_*` dispatcher. The collect (uses `vec_push`/`vec_reserve`,
  element opaque) specialises; `map_sum`'s `zero()` still declines.

- **Conditional bind-once in the beta-reducer.** Relaxing the gate let a
  wider class of closures inline — including one that reads a boxed
  parameter more than once (`lift: p => Pair { fst: p.fst, snd: JStr(p.snd) }`).
  `inline_apply` substituted the argument into every `EVar(p)` occurrence,
  so perceus emitted one `drop` per read and the second read hit freed
  memory (the value silently vanished). Fix: `inline_apply` now binds a
  parameter to a fresh `let` and reads that binder — but ONLY when the
  parameter is read more than once AND its value carries RC. A raw scalar
  (the collect's `k`) or a once-read parameter (a fold's `acc`) substitutes
  directly, keeping the unbox. RC-carrying is decided by `ty_is_unboxable_t`
  — the same representation predicate perceus uses for dup/drop, not a
  local scalar whitelist.

Measured N=10M native, min of 3 runs (`--release`, all three runs within
±0.01 s):

| Variant | Before | After #1208 |
|---|---:|---:|
| Manual (tail-rec fill + sum) | 0.09 s | 0.09 s |
| Fully idiomatic (collect + fold) | 0.46 s | **0.09 s** |

The idiomatic form drops from ~5x manual to **parity (1.0x)**. The
specialised collect loop reads `i64` indices, inlines the record ctor
(`kai_vec_push_rec_raw`), and carries **zero** per-element indirect calls
(KIR-verified).

## Design decisions and alternatives considered

**Extend mono's binding table, do not re-implement it.** The #1205 lane's
rejected "concretise engine" (a second `build_subst_map` + `subst_decl`
inside closure-spec) reached 1.0x but segfaulted the self-host — a parallel
substitution engine desyncs from the real one. This lane adds exactly one
binding pair (`formal_ret ~ ECall.ty`) to the binding set mono already
computes, reusing `extract_bindings_ty` (which is unify-or-ignore, so a
non-matching site adds nothing). Same table, same fixpoint, same
`collect_subst_tys` decline path. Self-host stays byte-identical.

**Refine the gate, do not relax it (architect verdict).** Relaxing
`any_return_only` unconditionally reintroduces the `zero()` trap: a
`map_sum` in a call site with no return annotation has a tyvar `ECall.ty`,
mono cannot ground `b`, and the arity-0 `zero()` falls to runtime dispatch
and panics (`no impl of Numeric.zero for runtime head`). The property that
actually traps is not "`b` is return-only" but "`b` is return-only AND the
body resolves an arity-0 protocol op through it". The gate now tests that
precise condition — independent of whether the call site annotated. This
is the distinction Rust's monomorphisation draws between a typaram that
only moves (always monomorphisable) and one resolved through a trait bound
(needs the instance).

**Ground from `ECall.ty`, never from the inlined body literal.** A tempting
shortcut is to infer `b` from the `Point{...}` the closure-spec beta-inlined
into the body. That is inference-from-body — a weak form of the second
substitution engine that segfaulted. Mono grounds tparams by structural
unification over signatures (params, and now ret-vs-call-site), never by
reading the body. `ECall.ty` is the correct, cheap source: the typer
already populated it, and it is the same channel a fold's accumulator uses.

## Structural surprises the brief did not anticipate

1. **Both halves are required for parity; neither alone suffices.** Gate
   refinement alone kills the indirect call but leaves `b` a tyvar in the
   `__cspec__` spec (the functional param that pinned `b` is dropped), so
   the element stays boxed — marginal win. Mono grounding alone unboxes the
   generic collect's element but the surviving `kai_apply` per element
   dominates. Only (a)+(b) reaches parity. Confirmed by reading the emitted
   C and KIR, not by guessing.

2. **The rewriter needs `ret_ty` too, not just discovery.** Discovery seeds
   the spec tuple with `b=Point`; the callsite rewriter independently
   re-resolves the instantiation to mint the retarget symbol. Without the
   same `ret_ty`, the rewriter's `resolve_call_inst` leaves `b` unbound →
   `collect_subst_tys` returns None → no retarget, and the discovered spec
   is emitted but never called. `rewrite_callsites_kind_sm` had to grow a
   `ret_ty` param carried from the enclosing `Expr.ty` (point-free callees
   pass `None`, correctly — they have no call-site return).

3. **Relaxing the gate exposed a latent RC bug in the beta-reducer (the
   biggest arc).** `test-stdlib`'s `jwt_encoder` regressed: `map(pairs, lift)`
   with `lift: p => Pair { fst: p.fst, snd: JStr(p.snd) }` produced an empty
   value for the first pair. Root cause read straight off the emitted C:
   `inline_apply` substitutes the argument expression into EVERY `EVar(p)`
   occurrence, so a parameter read twice duplicates the value; perceus emits
   one `drop` per occurrence, and the first `p.fst` frees the record before
   `p.snd` reads it. #1205 never hit this because a fold reads each param
   once and the collect's `k` is a raw `i64` (no RC). The fix is bind-once —
   but the FIRST cut (bind-once unconditionally) regressed the collect from
   0.09s to 0.74s: the `let k = i` binder blocked the unbox pass from
   propagating raw `i64` through, re-boxing the arithmetic. Only a
   CONDITIONAL bind-once — read-count > 1 AND value carries RC — fixes the
   boxed multi-read while leaving the scalar collect on direct substitution.
   RC-carrying must reuse perceus's own representation predicate
   (`ty_is_unboxable_t`), not a local `TyInt/TyReal/…` whitelist: a whitelist
   is a second source of truth that desyncs (a false "scalar" is a UAF).

4. **Grounding from the return type must be restricted to GENUINELY
   return-only tparams, or it steals a local fn's call site (#748, the
   CI-red one).** The first cut unified the formal return against
   `ECall.ty` for every tparam. That broke #748 shadowing: `list.sum[T:
   Numeric](xs: [T]): T` has `T` in BOTH a parameter and the return. A
   fixture's own `fn sum(t: Tree): Int` calling `sum(l)` — where `l: Tree`
   does NOT unify with `[T]` — used to decline (arg mismatch ⇒ unbound `T`
   ⇒ `None` ⇒ local `sum` wins). With the return binding, `T` grounded to
   `Int` from the `: Int` return, so mono synthesised `list.sum__mono__Int`
   and stole the call site — the tree got summed as a list, and
   `reuse_diagonal_guard` failed under tier1/tier1-asan. Fix:
   `extract_bindings_from_ret` records a binding ONLY for a tparam absent
   from every parameter (`collect_ty_tvar_ids` over `formal_pts`). A tparam
   a parameter mentions stays the args' job — if the arg mismatched, the
   call declines exactly as before. The collect's `b` (return-only after
   the stage param is dropped) still grounds; `list.sum`'s `T` no longer
   does.

5. **The gate refinement now specialises a handler-body combinator, so an
   effects fixture's symbol-grep had to widen.** `m4c_handler_in_body`'s
   `process_with_log[a, b](init: a, transform: (a) -> b): b` was declined by
   the old return-only gate (`b` is return-only); the refined gate lets it
   specialise on the named `bump`/`lengthof` transforms. Its call site's C
   symbol shifts from `kai_process_with_log__mono__Int__Int` to
   `kai_process_with_log__cspec__Fbump__mono__Int__Int` — same behaviour
   (asserts pass, clause symbols still collision-free, no polymorphic call),
   different mangle. The recipe's grep now accepts an optional
   `__cspec__F<fn>__` infix, which *strengthens* the check (both forms
   accepted, polymorphic still rejected) rather than weakening it.

6. **LL(1) parser rejects `not (…)` and `… or …` split across lines.** A
   multi-line boolean inside parens (`not (a\n  and b)`) and a match-arm body
   whose `or` chain wrapped to the next line both failed to parse in the
   single-pass parser. Fixed by hoisting to a named `let` / a one-line
   helper (`arity0_proto_call`). A reminder that the compiler's own surface
   is stricter about line breaks than it looks.

## Fixtures added and coverage gaps

- `examples/perceus/collect_ground_1208.kai` + `.out.expected`, wired as
  `test-collect-ground-1208` (native, tier1-native.yml). Asserts the sum
  AND greps the specialised collect loop (`rmcv_loop__cspec__…__mono__Point`)
  for an `i64` index signature and zero `call_ind` — the structural proof
  that the return-only element grounded and the per-element indirect call
  is gone.
- The `zero()` decline is pinned by `test-issue-668` (map/filter/flat_map in
  a fiber, the same fixture that pinned the #1205 return-only decline) and
  by the `map_sum` path in `test-stdlib` — both run C and native. Verified
  by hand that a `range_map_sum` call still declines cspec and runs without
  the `Numeric.zero` panic on both backends.
- `collect_multiread_1208` pins the conditional bind-once (a boxed
  multi-read stage param round-trips every field; the spec carries the
  `__csb` binder).
- The #748-shadowing regression is pinned by the pre-existing
  `reuse_diagonal_guard` (issue118) golden: it has a local `fn sum(t: Tree)`
  that the un-restricted return grounding rerouted to `list.sum` — the
  fixture fails without the `formal_pts` restriction, passes with it. Also
  covered across `test-stdlib` / `test-demos-core`, which are dense with
  local fns named like stdlib generics.
- Gap: no fixture exercises grounding from a **non-annotated** call site
  (where `ECall.ty` is still a tyvar). That path is the additive no-op
  branch of `extract_bindings_from_ret` — it declines exactly as before, so
  it is covered by every pre-existing return-only decline golden.

## Real cost vs estimate

The design half (asu consult) was the pacing item: the naïve read of the
issue ("`b` is return-only, mono can't see it") was half-right — mono
already grounded `b=Point` from the `stage` argument, but closure-spec
declined and the surviving `kai_apply` was the real cost. Reading the
emitted C early corrected the model. The parser line-break traps cost two
self-compile cycles (~6-8 min each, the dominant constraint).

## Follow-ups left for next lanes

- **Autovectorization.** With the collect loop inlined, call-free, and
  unboxed, the residual gap to hand-tuned native is autovectorization —
  the second stacked layer #1205 already named, still out of scope.
