# Lane experience — issue #898: qualified call to a cross-module protocol-bounded generic

## Scope as planned vs as shipped

**Planned (per the brief):** make a qualified call `mod.fn(...)` to a
protocol-bounded generic instantiate its spec like the bare form, so the
protocol op resolves to the right impl instead of panicking as a runtime
dispatcher.

**As shipped — the brief's premise had already been partly closed.** The
brief's *literal* repro (`agg.fold_from(0, [10,20,30])`, a name UNIQUE to one
module, called qualified) already printed `60` on this branch: PR #896's
`73de76a3` ("rewrite qualified calls to a protocol-bounded generic") added the
`EModCall` arm to both the discovery walker and the call-site rewriter, guarded
by `poly_fn_unique_in_module`. Measured before/after on the pre-#896 parent
(`8921dd25`) confirmed: the literal repro panicked there and prints `60` now.

The case that still panicked — and that the brief lists as a mandatory gate
("two modules each defining a `[T:P]` map ... must each resolve to its own") —
is **a name shared across two modules**. `a.reduce` / `b.reduce` (or a shared
PRIVATE helper `collapse_loop[T:P]` that two modules both define) panicked with
`no impl of P.op for runtime head N`, on BOTH the qualified and bare paths, and
already panicked before #896 (a pre-existing hole, not a #896 regression). The
`poly_fn_unique_in_module` guard deliberately DECLINED to rewrite a shared name
(to avoid minting an undeclared symbol), which left the generic dispatcher in
place → panic. This lane closes that hole.

## Which link in the chain was broken

`PolyFn`, `MonoTuple`, and `mono_mangle_name` all keyed specs by BARE NAME,
discarding the module. Two modules' `reduce[Int]` collapsed to one
`MonoTuple("reduce", [Int], [])`, so:

- `generate_specs_iter` emitted ONE spec (the first module's, via the
  first-match `lookup_poly_fn`); the second module got none.
- The rewriter's `poly_fn_unique_in_module` guard saw count==2 and refused to
  retarget, so the call stayed a generic dispatcher → panic.

The module IS preserved into the emitted spec's `mo` slot (emitter mints
`kai_<mo>__<mangled>`), and the qualified callee `EModCall(mod, mangled)` mints
`kai_<mod>__<mangled>` — but the spec keyed on the bare name only ever existed
for ONE of the two modules, so the other module's qualified call referenced a
symbol that was never emitted.

The broken link was therefore the **spec key + the spec-selection lookup**, not
the recording (the typer's `ResolvedCS` already carries a `module_origin`, but
it is the CALLER's module, not the callee's, so it cannot split a shared name).

## The fix and how it mirrors the bare-path oracle

`MonoTuple` gains a callee-module slot: `MT(Option[String], String, [Ty],
[UnitExprT])`. The module is `Some(mod)` exactly when the name is shared across
modules, `None` otherwise (a unique name's bare-name `DFn` lookup already
carries the right `mo`, so no behaviour change for the common case — this keeps
#896's unique-name path and the aggregates untouched).

- **Discovery** (`discover_call_tuple`): at `EModCall(mod, name)` it tags the
  tuple `Some(mod)` when shared; at a bare `EVar(name)` inside a spec body it
  inherits the spec's own module (`host_mo`, threaded through the discovery
  cascade), so a bare call to a shared private helper resolves to the host
  module's helper.
- **Spec selection** (`generate_specs_iter` → `lookup_poly_fn_for`): a
  module-tagged tuple binds to that module's `DFn`; the spec carries
  `mo = Some(mod)` and emits `kai_<mod>__<mangled>`.
- **Rewriter** (`rewrite_bare_call` / `rewrite_qualified_call`): mirrors
  discovery's tagging — a shared bare call retargets to `EModCall(host,
  mangled)`, a shared qualified call to `EModCall(mod, mangled)`, a unique name
  to `EVar(mangled)`. Same `tuple_in` test the bare oracle uses, against the
  same module-tagged tuple set.

The bare path remains the oracle: whatever it records/rewrites/substitutes for a
module-unique generic, the qualified path mirrors. The only new axis is the
module tag, applied symmetrically in discovery and rewrite.

## The infinite-loop trap (and the load-bearing insight)

First attempt re-tagged the discovered `None` tuples to `Some(host)`
POST-hoc in `emit_spec`, after `discover_new_tuples_decl` returned. This
diverged: discovery's internal dedup (`tuple_in(existing, mt)`) compared the
`None`-form against an `all_tuples` that already held the `Some(host)`-form, so
it never recognised the tuple as seen and re-emitted it forever.

The insight (credit: asu consult): **the dedup and the emission must compare the
same canonical form of the tuple.** Canonicalising AFTER the novelty test
guarantees divergence. The fix is to re-tag INSIDE `discover_call_tuple`, before
the `tuple_in` test — which means threading `host_mo` through the discovery
cascade (a pure pass-through parameter; it adds no branches, so cognitive
complexity is unaffected). `mt_retag_bare_shared` is idempotent on an
already-tagged `Some(_)` tuple, so an `EModCall` callee that already carries a
module is untouched.

## The arity-0 case

An arity-0 protocol op (`empty()` / `one()`, `Self` only in return position)
panics with a distinct message ("arity 0 op: caller must annotate Self") because
the dispatcher cannot infer `Self` from arguments. It shares the root cause: the
spec body's `__proto_<op>` → `__pimpl_<P>_<T>_<op>` rewrite only fires inside an
emitted spec. Once the per-module spec is emitted, the arity-0 op resolves
exactly like the binary op. The fixture exercises both shapes (`fold` uses
arity-0 `empty`/`one`; `fold_from` uses binary `combine`).

## Name-shared-across-modules safety

The gate the #896 guard protected (a name two modules share must not silently
pick the wrong module's spec) is now satisfied positively, not by declining:
each module's qualified call reaches its OWN `kai_<mod>__…` spec. A tparam-less
qualified fn (`map.filter`, whose `k,v` come from `Map[k,v]`) is not in `polys`,
fails `poly_fn_has_module`, and stays an ordinary generic call — unchanged.

## Fixtures added

`examples/multi-module/issue-898-qualified-generic/` (agg.kai + prod.kai +
main.kai): two modules both export a `[T:P]` `fold` (shared name, arity-0 op,
over different protocols `Monoid`/`Prod`), plus a name unique to `agg`
(`fold_from`, binary op) called both qualified and bare. Asserts `60 / 24 / 15 /
15`. Wired into `test-multi-module` (abs+rel run) and into `MODULAR_FIXTURES`
(`test-modular-build`), so the per-module specs are exercised through the
split-and-link cross-TU path, not just the bundle.

## Coverage gaps / follow-ups

- A BARE call to a shared name with ambiguous imports (both modules imported
  unqualified, neither call qualified) is left to the typer's resolution; this
  lane targets the qualified surface and the shared-helper-from-a-spec path. No
  regression observed, but it is not a positive gate here.

## Real cost vs estimate

The structural surprise: the brief framed the fix as "finish #896's EModCall
rewrite", but #896 had already finished the unique-name case; the real bug was
the shared-name spec key, a deeper structural axis (module-keyed tuples). The
infinite-loop trap cost one wrong attempt before the dedup-canonicalisation
insight. Refactored `rewrite_callsites_kind_sm` into `rewrite_bare_call` /
`rewrite_qualified_call` to keep per-function cognitive complexity under the
bar (the dispatch was already at the ceiling pre-lane).
