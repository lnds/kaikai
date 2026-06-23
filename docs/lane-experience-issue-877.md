# Lane experience — issue #877: protocol bounds on free-fn type params

Form 3 of the retired #341 umbrella: let a free fn carry a protocol
bound on a type parameter (`fn pick_larger[T : Ord](a: T, b: T)`) and
dispatch that protocol's ops in its body. Forms 1 and 2 shipped earlier.

## Scope as planned vs as shipped

Planned (from #877): parse the bound, thread it as a constraint
witness through InferState, monomorphise the witness per instantiation;
generalise `sum`/`product`/`max`/`min`/`sort`/`uniq`; migrate one
compiler dedup site.

Shipped — the **mechanism**:
- Parser accepts protocol bounds on free-fn tparams.
- A bound op call in the body dispatches via the monomorphisation
  re-run (no InferState threading; see below).
- `import_hole_dedup_paths` migrated to a generic `list_uniq` helper
  in `compiler/util.kai` — the smoke test of the generic mechanism on
  the compiler itself.
- Five regression fixtures, all green on both backends; selfhost
  byte-identical.

Not shipped — the stdlib aggregate generalisation. The lane's goal is
the mechanism, not generalising any specific aggregate. Two of them
hit blockers outside the mechanism's scope, so stdlib is left
untouched:
- `sum`/`product` need a polymorphic monoid identity (`zero : Self`)
  that no stdlib protocol exposes (design discovery — see below).
- `max`/`min`/`sort` are generalisable in principle but a **bare**
  `max[T:Ord]` works while a **qualified** `list.max(...)` does not:
  the qualified call lowers to the unspecialised polymorphic body
  whose `cmp` stays the `__proto_cmp` dispatcher (`max` returns the
  last element, `min` the first). Root cause: the monomorphiser does
  not treat an `EModCall` (the qualified-call shape `rqc` produces)
  as a specialisable call site — it only specialises `ECall(EVar)`.
  This is a **pre-existing** monomorpher/qualified-call gap that was
  invisible while the aggregates were monomorphic `[Int]`; the brief's
  own guidance is "if the resolver interaction is hairy, land resolver
  hardening first — don't force it." A separate lane should fix the
  `EModCall` specialisation, after which these aggregates generalise
  cleanly.

## The witness needs no InferState threading — it already exists

The brief's central assumption (thread the bound into InferState as a
constraint witness) turned out to be unnecessary. The impl-header bound
mechanism it pointed at as prior art does **not** thread anything: the
parser strips the bound off impl tparams before the typer sees them
(`prepend_impl_tparams_to_methods`), and dispatch falls out of a single
existing pass — `monomorph.kai` re-runs the protocol-call resolver
(`resolve_protocol_calls_decl`) on each spec body once its type args are
concrete. A call `cmp(a: T, b: T)` stays the dispatcher stub
`__proto_cmp` while `T` is a tyvar (the resolver declines on a
tyvar-headed arg), and specialises to `__pimpl_Ord_Int_cmp` when
`T → Int`.

So the whole feature for free fns reduces to: parse the bound like an
impl header does, strip it from the stored tparam string, and let the
existing monomorph re-run dispatch the body ops. The diff in the typer
is **zero** — the change is one parser call swap plus a `map(_,
tp_strip_bounds)`. The integration trap the recon flagged (the typer's
`tp_strip_kind` only strips `#Unit`, never `#b:...`, so a `T#b:Eq`
string would fail to match `T`) is sidestepped by stripping at the
parser, exactly as the impl path does.

## Monomorphisation order resolved itself

The risk was that the free-fn witness collapse must run before #174's
polymorphic-impl validator. It does, structurally: the validator
(`validate_resolved_decls`) runs in the driver *after* `monomorphise`,
which is where the re-resolution happens. The mixed fixture
(`free_fn_eq_bound_wrapper`: `same[T : Eq]` applied to a `Wrapper[T]`
whose own `impl[T : Eq] Eq for Wrapper[T]` recurses on the inner `T`)
exercises this — nested `eq(Wrapper) → eq(inner)` dispatch resolves at
both levels and the validator does not misfire.

## Why the stdlib aggregates were left untouched

The mechanism handles `max`/`min`/`sort` (`[T : Ord]`, dispatching
`cmp(a, b)` with `T` in argument position) and `uniq` (`[a]`, body uses
the runtime `==` via `contains`) — verified working on `Int` and
`String` when called bare. But generalising the stdlib definitions
broke `test-stdlib`'s `list_extrema`, and the diagnosis was a
**pre-existing monomorpher gap, not a flaw in the mechanism**:

- A **bare** `max([3,1,9,...,6])` lowers to the monomorphised spec
  `kai_list__max__mono__Int` and returns `9` correctly.
- A **qualified** `list.max([3,1,9,...,6])` lowers to the
  *unspecialised* polymorphic body `kai_list__max`, whose `max_loop`
  calls the dynamic dispatcher `__proto_cmp` — and the generated C
  reads its boxed result through `kai_intf`, yielding garbage. `max`
  returns the last element, `min` the first.
- Root cause: `rqc_kind` rewrites a qualified call to `EModCall(mod,
  fn)`, and the monomorphiser never specialises an `EModCall` — it
  only walks `ECall(EVar(name))`. So the qualified path keeps the
  polymorphic body. A pre-existing polymorphic stdlib fn like
  `list.contains` (`[a]`) is unaffected because its body uses the
  runtime `==`, not a protocol op left as a dispatcher stub.

This was invisible while the aggregates were monomorphic `[Int]` (one
body, no spec, no `cmp`). Generalising them is sound *once the
`EModCall` specialisation gap is fixed* — a separate
monomorpher/resolver lane, exactly the "land resolver hardening first,
don't force it" path the brief prescribes. `sum`/`product` carry the
additional, orthogonal blocker of needing a polymorphic monoid
identity (`zero : Self`); see below.

So this lane ships the mechanism and the compiler-internal smoke test
(`list_uniq`), and leaves `stdlib/core/list.kai` exactly as it was on
main.

## sum/product — the monoid-identity blocker

`sum`/`product` cannot be generalised even after the `EModCall` fix.
The issue named `[T : Numeric]`, but `protocol Numeric`
(math/numeric.kai) exposes `abs`/`sign`/`pow_int`/`clamp` — no `+`/`*`
and no identity. `Add`/`Mul` give a polymorphic `add`/`mul` but no
polymorphic `zero`/`one`. A generic `sum` needs an identity for the
`[]` case, and that identity is a nullary-in-`Self` op (`zero() : T`).
kaikai's only return-position dispatch (`Default.default`,
`Serialize.from_string`) is annotation-driven — `Self` is pinned by a
concrete annotation at the call site (`let x : T = default()`).
`sum`'s `zero()` has no such annotation; the `T` comes from the
enclosing fn's tparam. Resolving it needs both a new protocol surface
(a monoid-identity op) and a dispatch mechanism that does not exist
today — both outside #877's stated scope. Left for a follow-up
"monoid-identity protocol surface" lane.

## Resolver shadowing — no hardening needed by the mechanism

The feature adds a new way for bare-name calls to resolve, so the
worry was the first-arg narrowing (#235) and arity-aware dispatch
(#594) regressing. They did not: `test-shadowing` stays green. (The
distinct, pre-existing `EModCall` monomorpher gap above is a
qualified-call issue, not a bare-name shadowing one, and surfaced only
when the aggregates were made polymorphic — which this lane reverted.)

## Fixtures

In `examples/protocols/`:
- `free_fn_ord_bound` — `fn[T : Ord]` calling `cmp` (the core case).
- `free_fn_eq_bound` — `fn[T : Eq]` calling `eq` (dedup shape).
- `free_fn_eq_bound_wrapper` — the mono-order mixed case.
- `free_fn_default_return` — Form-1 compose (return-position `default()`
  under a tparam return type).
- `free_fn_ord_missing` — negative: a type with no `Ord` impl rejected
  post-mono with `no impl of Ord for type Box (operation cmp)`.

## Compiler dedup migration

The compiler does not link the stdlib (its bundle concatenates only
`compiler/*.kai`), so the migration could not reuse stdlib `uniq`.
Instead a generic `list_uniq` was added to `compiler/util.kai` and
`import_hole_dedup_paths` (modules.kai) — a first-occurrence dedup over
`[String]`, structurally identical — was deleted in favour of it. The
other two ad-hoc dedup sites are not clean `uniq` calls:
`union_dedup_sorted` is adjacent-only on a custom `inf_ty_eq`
predicate, and `collect_distinct_tuples` fuses map/filter/dedup. They
are left alone.

## Cost

The recon (three parallel explorers) was the load-bearing investment —
it overturned the brief's InferState-threading plan before any typer
code was written. A four-case spike (`Ord` positive, `Ord` negative,
`Eq` body dispatch, mixed mono-order, Form-1 compose) confirmed the
mechanism worked with the minimal parser change before committing. The
selfhost needs ~9 GB heap; the `KAI_MAX_HEAP=4g` host-safety default is
too low for it (fine for user programs).

## Follow-ups

- `EModCall` monomorphisation: teach the monomorphiser to specialise a
  qualified call (`list.max(...)`) the same way it specialises a bare
  `ECall(EVar)`. Until then a polymorphic stdlib fn that dispatches a
  protocol op cannot be called qualified. This is the gate for
  generalising `max`/`min`/`sort`/`uniq` in stdlib.
- Monoid-identity protocol surface (`zero`/`one`) to generalise
  `sum`/`product` — needs nullary-in-`Self` dispatch by the enclosing
  fn's tparam, a mechanism that does not exist yet. Orthogonal to and
  on top of the `EModCall` fix.
