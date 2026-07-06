# Lane experience — #1099 (typer: enforce free-fn protocol bounds at the call site)

## Scope as planned vs as shipped

**Planned (issue #1099):** a `[T: P]` bound on a free fn is honoured only
indirectly today — when the body dispatches the bounded op on a concrete
type, the post-mono validator rejects a missing impl. If the body never
reaches the op, the bound is silently ignored (`f1[T: Eq]([Q])` with `Q`
lacking `Eq` compiled). Enforce the bound at the concrete call site,
regardless of whether the body dispatches, without reintroducing
constraint propagation (Tier 1 #3).

**Shipped:** exactly that. A free fn's declared bounds now ride a
parser-captured side table to the monomorphiser, which checks each
concrete instantiation tuple against the callee's bounds using the same
impl-table lookup the dispatch validator already trusts. A concrete slot
with no impl of `P` is rejected at the call site; a tyvar slot defers to
its own instantiation, so a generic caller forwarding its tparam
propagates nothing.

Touched: `ast.kai` (2 side-table types), `parse.kai` (capture on
`PDecl`/`PParser`), `driver.kai` (re-key + report), `monomorph.kai` (the
check), `protos.kai` (three lookups made `pub`). New code is A-grade by
cogcom (all new fns ≤ 10, avg ~5). Four fixtures under
`examples/protocols/`.

## The load-bearing decision: where the bound survives

The bound is encoded as a `#b:Eq` suffix on the tparam string at parse
time and, pre-#1099, stripped in `parse_fn_decl` before the DFn is built
(typer diff zero). The whole lane is a single question: **how does that
bound reach a phase that can check the call site, without touching the
mechanism that already works?**

Three routes were weighed:

- **Keep the bound on `DFn.tparams`, strip at the ~6 typer/mono
  consumers.** Rejected: `mk_tpbinds` and `collect_implicit_tparams_in_decl`
  key on tparam *names* by exact string equality. A `T#b:Eq` string
  silently mis-binds `EVar("T")` and mis-collects implicit tparams —
  corruption with no fixture to catch it. This is the one route that can
  break the hot typer path invisibly.

- **A `DFnBounds` satellite decl in the stream.** Rejected on cost: 448
  sites in the compiler match on `Decl` variants; a new variant is a large
  sweep even if the compiler shouts on non-exhaustive matches.

- **A parallel side table captured at the parser (chosen).** `PDecl`
  gains a `bounds: [FnTPBound]` slot filled only by `parse_fn_decl` (2
  constructor sites, 157 `ok_d`/`err_d` callers untouched);
  `parse_program_loop` seeds `[FnBoundsRec]` onto `PParser`. The DFn stays
  byte-for-byte as before — every downstream pass and every load path
  (root, prelude, cache) sees clean tparam strings. Selfhost byte-id
  proves the typer is untouched.

## The trap the first attempt hit: multiple decl-entry paths

The first cut did NOT strip in the parser — it left `#b:Eq` on the DFn
and ran a single `strip_fn_bounds_decls` in the driver over
`expanded_decls`. It broke the stdlib immediately: `max[T: Ord]` /
`sort[T: Ord]` failed with `no impl of Ord for type T`. Cause: **preludes
load through a separate path** (`load_prelude` / the prelude cache), never
passing through the driver's single strip point, so their raw `#b:Ord`
tparams reached the typer. Centralising a strip in the driver cannot
cover a compiler with more than one decl-entry path (root via
`expand_imports`, preludes via `load_prelude`, both via the KAB2 cache).

The fix is to strip at the parser — the one point every path funnels
through — and carry the bound out-of-band. This is why the side table,
not an in-DFn survival, is the honest design.

## Scope narrowing that made the side table root-only

Measured before threading module bounds anywhere: every stdlib bounded
aggregate (`sum`/`product`/`uniq`/`max`/`min`/`sort`) delegates to a
`_loop` helper that DOES dispatch the bounded op. So `sum([Q])` with `Q`
lacking `Numeric` already errors today (`no impl of Add for Q`, at the
user's call site) via the existing post-mono validator. The only fns that
need the new check are those whose body never dispatches — in practice
the user's own root-file fns. The parser captures bounds for every fn, but
the driver only feeds the root file's records (`parsed.fn_bounds`,
re-keyed to the target module) to `monomorphise`. This sidesteps the KAB2
cache entirely: a cached module's stripped decls carry no bound, and none
is needed, because a module bound that matters is one whose body
dispatches, and that path is already covered.

## Transitive coverage: the check lives in mono, not the driver

The direct case (`f([Q])` in `main`) is in `tp.insts` with the caller's
span. The transitive case (`g[Q]` forwarding to `f[T: Eq]`) is NOT: inside
`g`, the call to `f` has a tyvar arg, so `f@Q` is materialised only when
mono specialises `g__mono__Q` and walks its body. That tuple surfaces in
`generate_specs_iter`'s final tuple set, never in `tp.insts`. So the check
runs over `all_items` (the post-fixpoint tuple set), which contains both
direct and transitive instantiations. A pre-mono driver check would miss
the transitive case — that was the RED/GREEN gate the design turned on,
and the `free_fn_bound_transitive` fixture is it.

Location for a transitive tuple falls back to the callee decl's own
line/col (`decl_loc_of`) because no inst carries its span. The direct case
gets the exact caller span from `inst_loc_for`. Both name the violated
`(type, bound, fn)` triple.

## The satisfies oracle — reuse, do not invent

"Does head H satisfy protocol P" is the same trio the derive validator
already trusts: a user impl OR a derive-synthesised impl (both land in
`reg.impls` because `expand_derives` lowers `#[derive(P)]` to synthetic
`impl P for T` before `lower_impls` registers the PIR) OR a structural
builtin (`derive_builtin_impl`, Int/Real/... for Eq/Show/Ord/Hash).
`head_satisfies_proto = prc_reg_has_impl(reg, P, H) or
derive_builtin_impl(P, H)`. Verified consistent with the live dispatch: a
tparam instantiated to `[Int]` (head `List`) is accepted by both the new
check and an `eq`-dispatching body — the check is neither stricter nor
laxer than the mechanism that already runs. Selfhost byte-id (thousands of
bounded instantiations) is the proof of no false positives.

## Fixtures

Under `examples/protocols/` (glob-discovered by `test-protocols`,
`.err.expected` ⇒ negative):

- `free_fn_bound_unenforced_callsite` — the issue's central case: `f[T:
  Eq]` no-eq body, called with `Q` (no Eq) → error at the call site.
- `free_fn_bound_transitive` — `g[Q]` forwarding to `f[T: Eq]` → error at
  the concrete instantiation (the transitive-coverage gate).
- `free_fn_bound_satisfied` — `f[T: Eq]` over a user-`impl` type, a
  `#[derive(Eq)]` type, and `Int` → compiles and runs.
- `free_fn_bound_generic_forward` — `g[T]` forwarding to `f[T: Eq]`,
  instantiated at `Int` → compiles (deferral does not over-reject).

Multi-tparam (`f[A, B: Eq]`, index 1) and multi-bound (`f[T: Eq + Ord]`,
only the unsatisfied one reported) were verified interactively; the
type-kind indexing skips unit-kind tparams so the tuple `tys` alignment
holds.

## Follow-ups

- Transitive error location is the callee decl, not the originating
  `g([Q])` call site. Recovering the true origin would mean threading the
  instantiating inst's span through mono's transitive tuple discovery —
  more plumbing than the diagnostic quality warrants for now.
- The direct-case message says "on a type parameter of `f`" without
  naming the tparam (`T`). The side table drops the tparam name (only the
  type-kind index survives, which is what aligns with `tys`); naming it
  would mean carrying the name too. Low value.
