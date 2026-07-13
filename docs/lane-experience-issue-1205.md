# Lane experience — issue #1205: closure specialisation

## Scope as planned vs as shipped

**Planned.** The idiomatic Vec pipeline (`[1..n] | (k => Point{...})` fused
collect + `vec.foldl(pts, 0, (acc,p) => acc + p.x)`) paid a per-element
indirect closure call inside the monomorphised stdlib loop — ~10x slower
than hand-written tail recursion on native. Fix: specialise higher-order
calls on known lambdas, inlining the stage body into the loop so the
`call ptr(env, args)` per element disappears. Gate: repro at N=10M within
~1.2x of the manual variant.

**Shipped.** A new pre-monomorphisation pass (`closure_spec`, three
modules) that specialises a root-file higher-order call on a **pure,
env-empty** lambda or top-level fn:

- Beta-reduces every `f(args)` application into the callee body.
- Retargets the callee's own recursion onto the spec so a self-recursive
  loop (`foldl_loop`) stays a self-recursive spec — TRMC re-forms the loop
  rather than emitting a one-iteration wrapper per recursion site.
- Closes the spec's effect row to empty and prunes the now-unused row
  tparam, so **mono grounds the remaining tparams and unbox lands the
  loop's scalars**: a fold's accumulator (`r` from `init: r`) becomes an
  i64 loop identical to hand-written `sum`.

Measured N=10M native, min of 3 runs (`--release`):

| Variant | Before | After |
|---|---:|---:|
| Manual (tail-rec fill + sum) | 0.07 s | 0.07 s |
| Fused collect + manual sum | 0.37 s | 0.36 s |
| Manual fill + `vec.foldl` | 0.36 s | **0.07 s** |
| Fully idiomatic (collect + foldl) | 0.71 s | **0.39 s** |

The `vec.foldl` path reaches **parity with manual (1.0x)** — the fold's
accumulator (`r` from `init: r`) unboxes. The **collect declines
specialisation**: its element type `b` is a return-only tparam, and
specialising it is not merely boxed-but-slow — a combinator whose body
resolves an arity-0 protocol op through that tparam (`map_sum`'s
`zero()`) traps at runtime once the tparam stays a tyvar. The gate that
protects that (see the traps section) also declines the pure `collect`,
so the fully-idiomatic repro lands at **0.39 s = 5.6x manual** (10x →
5.6x, a 1.8x improvement), not the ≤1.2x target. All variants
byte-identical.

## Design decisions and alternatives considered

**Mono-level (AST), not the KIR inliner.** Decided by spike + evidence.
The KIR beta pass (`kir_inline_beta`) can only reduce a `KCallIndirect`
against a `KClosure` in the SAME function; here the closure is built in
`main` and the indirect call lives in `foldl_loop` across a fn boundary,
and `foldl_loop` carries `KTcrecGoto` which `kin_lam_admissible` rejects.
Inlining the higher-order into the caller (the other option) destroys the
TRMC shape — a known prior conflict. Specialising at the AST level, before
perceus and TCO, sidesteps both: the reduced lambda lands as a
sub-expression of the tail-call's argument, tail position preserved, and
perceus/TCO recompute over the already-inlined body.

**Row-close, NOT a manual concretise engine.** The first attempt at
unboxing the loop re-implemented monomorphisation: derive concrete types
from the call args, `build_subst_map` + `subst_decl` on the callee body.
It WORKED for the benchmarks (collect and foldl both reached 1.0x) but
**segfaulted the self-host** — a second substitution engine desyncs from
the real one, and applying it to the compiler's own elaborate
higher-orders produced malformed specs. Reverted per the language
architect's verdict: close the effect row + prune the row tparam (a local
scheme transform, no value substitution — sound and cheap) and let **mono**
ground the rest. The frontier this leaves is exact: a tparam a data
argument pins concretises; a return-only tparam (a collect's element type)
stays polymorphic and boxed. Accepting boxed-on-return is the natural
boundary of what mono infers without a return annotation — not a
half-feature.

**Purity gate by the resolved row, not by scanning the body.** An
effectful stage in a `map`/pipe observes per-element effect ordering that
inlining would reorder (the exact soundness gate pipe fusion applies).
The first purity check scanned the AST for `EField` capability reads —
but post-typer effect ops are `ECall`s to dispatchers, not visible
`EField`s, so it let `Stdout.print` through and reordered #1134's fixture.
The fix reads the closure's resolved `TyFnT` row and reuses
`pipe_fusion_labels_pure` (pure iff labels ⊆ {Mutable}).

## Structural surprises the brief did not anticipate

1. **Bundle binder-shadow miscompile (the big one).** A top-level fn in a
   bundled compiler module whose name matches a *local pattern binder* in
   another module (`decl_mo`, `decl_name`, `param_names` are `let`/pattern
   binders in `infer.kai`) makes kaic1 miscompile the WHOLE bundle: the
   resulting kaic2 panics `non-exhaustive match` compiling *anything*,
   hello-world included. Diagnosed by bisecting the module down to the
   single offending fn. Fix: `cs_`-prefix every helper. Same trap bit a
   record field named `args` (a stage0 builtin) → renamed to `vals`.
   File-top `#[doc("""` on a bundled module is another miscompile vector —
   lead with a `#` comment instead.

2. **Transitive specialisation needs discovery before rewrite.** A spec's
   body still carries the raw `inner(..., L)` call whose request the next
   fixpoint round must find. Rewriting inside the loop retargets it away
   before it is seen, stranding the transitive spec (`foldl → foldl_loop`).
   Fixed by discovering to a fixpoint over un-rewritten decls, then
   rewriting once at the end.

3. **`mo == None` gate keeps the compiler safe.** Specialising the
   stdlib/compiler's own internal higher-order call sites (`mo != None`,
   elaborate types) is what breaks the self-host. Gating discovery to
   root-file call sites confines specs to user code; a generated spec is
   itself `mo == None`, so transitivity still walks. This also fixed the
   `test-tco-rule3-basic` fragility for free (stdlib `sort_by`/`merge_by`
   specs no longer pollute the grepped C).

4. **Pipe-fusion `__fusev` binders are locals, not globals.** A multi-stage
   fusion hoists stages to `let __fusev_N = lambda`. Classifying the
   composed stage's captures treated `__`-prefixed names as globals (they
   are not — they are locals), so a capturing lambda slipped through and
   beta left a dangling reference → `unbound register` on native. Fix:
   `name_is_global` only treats capital-initial names (type ctors) as
   env-free; everything else is a possible capture and declines.

5. **Return-only tparam + arity-0 protocol op traps (the CI-red one).**
   A callee whose element/return type is a tparam that appears ONLY in the
   return (`b` in `map_sum: (a) -> b : b`, a collect's element type) is not
   grounded by mono — mono infers tparams from argument types. For a
   collect that is merely boxed-but-correct; but `map_sum`'s body resolves
   an arity-0 ring op (`zero()`) through `b`, and once `b` stays a tyvar
   the op falls to runtime dispatch and *panics* (`no impl of Numeric.zero
   for runtime head`). Surfaced as two CI reds of the same class: a
   `Numeric.zero` panic on a `| map | sum` pipe, and a `poker` demo ASAN
   failure downstream of the same boxed spec. Fix: `hofn_specialisable`
   declines any callee with a type tparam that is return-only (appears in
   the return's `TyName`s but no surviving parameter's). This costs the
   pure `collect` its specialisation too (back to indirect), which is why
   the idiomatic number is 5.6x rather than 6.5x — soundness over the last
   layer of speed. The effect-row tparam is exempt (it lives in the row,
   not a `TyName`, and the spec closes it). NOTE: the return-only check
   must cover IMPLICIT tparams too (`flat_map(xs: [a], ^f: (a)->[b]) : [b]`
   declares no `[a, b]` — `b` is implicit), so it keys on lowercase-initial
   names in the signature, not the declared `tparams` list.

6. **Cross-fn value-pass forks RC across specialisations (a C-backend
   UAF).** `group_by(xs, key) = group_by_loop(t, key, key(h), ...)` both
   APPLIES `key` and passes it as a VALUE to a *different* fn. Specialising
   forces a second, independent specialisation of `group_by_loop` that
   re-inlines the same closure; the two specs' RC bookkeeping does not
   reconcile and `group_by(xs, (x) => x)` segfaults on the C backend
   (native happened to survive — a backend-specific UAF, exactly the RC
   risk the brief flagged). A fold is safe because its loop passes the
   closure only to its OWN recursion, which `retarget_self` folds into the
   same spec. Fix: `functional_param_safe` declines a callee that both
   applies the functional param AND passes it as a value to a fn other than
   itself. Rejected the alternative (a bind-once let around the arg) on the
   architect's verdict: it fixes duplication inside one body but not the
   cross-spec fork.

7. **Protocol ops reified as `__proto_*` / overloaded names.** A bare op
   passed as a value (`sort_by(xs, cmp)`) reifies into a dispatcher
   (`__proto_cmp`) whose one body dispatches over the runtime type. Inlining
   it froze one impl and panicked (`todo: __kai_proto_dispatch__:Ord:cmp`)
   for every other type. Fix: `classify_closure` declines a `__`-prefixed
   name (compiler-synthesised) and `lookup_named_fn` declines a name with
   more than one tparam-free definition (an overloaded op). A user's single
   named fn still specialises.

The five decline gates (effectful, capturing, return-only, cross-fn
value-pass, synthesised/overloaded name) share one shape: the pass only
specialises where the closure's flow is *local and total* — applied in the
body, or threaded only through self-recursion, over a callee whose result
types mono can ground. Everything else stays indirect and correct.

## Fixtures added and coverage gaps

- `examples/perceus/closure_spec_1205.kai` + `.out.expected`, wired as
  `test-closure-spec-1205` (native, tier1-native.yml). Asserts the sum AND
  greps the emitted specialised loop for zero `call_ind` — the structural
  proof that the per-element indirect call is gone.
- The decline gates are covered by existing stdlib goldens, which is how
  each surfaced: `test-issue-668` (map/filter/flat_map in a fiber) pins the
  return-only + cross-fn declines; `test-stdlib`'s `group_by` /
  `sort_by_bare_ord_op` goldens pin the cross-fn-value-pass and the
  `__proto_*` declines; `test-perceus-1134-pipe-fusion-ordering` pins the
  effectful decline. All run C AND native, and under ASAN via tier1-asan.
- Verified by hand (not fixtured, low value): capturing lambda stays
  correct+boxed, named pure fn specialises, multi-stage fusion declines.
- Gap: no fixture pins the *boxed collect residual* — it is a known
  frontier (#1208), not a regression, so no golden guards it.

## Real cost vs estimate

Dominated by two long debugging arcs the brief flagged as traps but that
still cost the most wall time: the bundle binder-shadow miscompile
(diagnosed by whole-module bisection, several self-compile cycles) and the
concretise-engine self-host segfault (built, measured to 1.0x, then
reverted on architect verdict). The self-compile cycle (~6–8 min each) is
the pacing constraint for any compiler-touching change.

## Follow-ups left for next lanes

- **Return-only tparam grounding (issue #1208).** The idiomatic repro sits
  at 5.6x, not ≤1.2x, because a collect's element type is a return-only
  tparam that closure-spec must now *decline* (specialising it either boxes
  the loop or, when the body has an arity-0 protocol op, traps). Landing it
  needs mono to ground a tparam from the *expected return type* at the call
  site (or a return annotation), not from arguments — a mono enhancement,
  not a closure-spec one. Once mono can do that, the `hofn_specialisable`
  return-only gate here can relax and the collect specialises. The manual
  concretise engine that reached 1.0x is documented there as rejected
  (segfaulted the self-host).
- **Autovectorization.** With the loop now inlined and call-free, the
  residual gap to Rust (`0.07 s` vs `0.01 s`) is autovectorization — the
  second stacked layer the issue named, out of scope here.
