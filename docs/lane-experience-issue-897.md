# Lane experience — issue #897

Bound-blind monomorphiser call-site rewrite breaking #748 root-fn
shadowing when a stdlib `pub fn` is generalised from monomorphic to a
protocol-bounded generic. Unblocks #896 (#891).

## Scope as planned vs. as shipped

**Planned:** in `synthesize_inst_for_decl` / `collect_subst_tys`,
return `None` when a type-tparam is left unbound after unifying the
generic's formals against the call's actual args, rather than
defaulting the tparam to `TyAny` (the verified root-cause at the old
`monomorph.kai:992`).

**Shipped:** the planned fix, **plus** an arity guard discovered to be
necessary while building the regression fixture. Two complementary
guards in `synthesize_inst_for_decl`:

1. **Arity guard** — `list_length(formal_pts) != list_length(args_)`
   declines (`None`). A user `fn tally(p: Point)` (arity 1) shadowing
   an imported `tally[T:Ord](seed, xs)` (arity 2) bound to the spec by
   its first formal `seed: T` (a bare `TyVarT`, so `T = Point` bound
   cleanly), then emitted a 1-arg call to a 2-arg spec — a C compile
   error, not a panic. The unbound-tparam guard alone does not catch
   this because the tparam *does* bind.
2. **Unbound-tparam guard** — `collect_subst_tys` now returns
   `Option[[Ty]]`, yielding `None` the moment any id is unbound. Its
   only caller propagates the `None`. This is the #897 repro class:
   `fn sum(t: Tree)` shadowing `sum[T:Numeric]([T])` leaves `T`
   unbound (a `TyCon` actual never unifies with the `[T]` formal).

Both guards encode the same principle: if the call is not a real
instantiation of this generic (wrong shape, wrong arity), decline the
synthesis so `resolve_call_inst` returns `None`, the callee stays
`EVar("name")`, and emit's `efn_resolve` falls back to the user's
monomorphic root fn — #748 preserved exactly as before the
generalisation.

## The bound-aware stricter variant — NOT needed, not deferred

#897 floats a stricter variant: also reject a `T` that *does* bind to
a concrete type lacking the required `impl` (e.g. `sum([Foo])` where
`Foo` has no `Numeric`), which would need the protocol-impl registry
threaded into `resolve_call_inst`.

Verified this is **unnecessary**: the typer already rejects bound
violations before the monomorphiser runs. `collapse([Bar, Baz])` over
a `Foo` with no `Monoid` impl produces a clean typer diagnostic
(`error: no impl of Monoid for type Foo (operation combine)`), not a
panic and not a mono-stage misroute. The monomorphiser only ever sees
calls the typer has already accepted; the only gap it could introduce
was the *shape* mismatch (where the typer correctly bound the call to
the user fn but the bound-blind rewriter overrode it). That gap is now
closed. No registry threading, no follow-up issue for the stricter
variant.

## Shapes verified beyond `Tree`

- **`Tree`** (TyCon vs `[T]`) — unbound-tparam guard. User fn wins.
- **`Point`** (TyCon, arity-1 user fn vs arity-2 generic) — arity
  guard. User fn wins.
- **Genuine generic** `bag([10,20,30])` (bare call, `T` binds to
  `Int`) — synthesises and instantiates correctly: `60`.
- **Bound violation** `collapse([Foo])` — typer rejects before mono.

All four in the single fixture `examples/multi-module/issue-897-shadow/`
(the bound-violation one verified manually; it is a negative-compile
case, not wired as a positive golden).

Negative proof: on pristine main (fix stashed) with the imported
generic in place, the `Tree` shadow case panics with the exact #897
message `no impl of Monoid.empty for runtime head (arity 0 op: caller
must annotate Self)`, and the `Point` case mis-compiles to a 1-arg
call of a 2-arg spec. With the fix both print correctly.

## #748 shadowing + genuine instantiation both hold

- Shadowing: `collapse(Tree)` and `tally(Point)` resolve to the user
  root fns. Selfhost stays byte-identical on **C and native**, the
  strongest evidence that no legitimate internal generic instantiation
  in the compiler regressed.
- Genuine generic: `bag([Int])` (and, verified against a temporary
  #896-shaped `sum[T:Numeric]` stdlib, `sum([1,2,3])==6` /
  `sum([1.0,2.0,3.0])==6.0`) still synthesises and rewrites to the
  spec with the right ABI.

## Structural surprise the brief did not anticipate

The arity axis. The brief framed the fix purely around the unbound
tparam. The minimised regression fixture — deliberately using a
second colliding shape with a *different arity* generic — exposed that
a tparam can bind via a leading `seed: T` formal even when the call
shape is wrong, slipping past the unbound-tparam guard into an
arity-mismatched spec. The arity guard is the other half of "the
formal shape did not match the call" and belongs in the same fix.

## Separate bug found and filed (not fixed inline) — #898

While building the fixture, found that a **qualified** call
(`mod.generic(...)`) to a cross-module protocol-bounded generic skips
spec instantiation entirely: the protocol op in the body stays a
runtime dispatcher and panics, while the **bare** call to the same
generic works. Reproduces on pristine main with no #897 changes —
orthogonal to this lane. Filed as #898 (`bug,compiler,typer`) with a
minimal repro and the qualified-name-keying hypothesis. The #897
fixture's genuine-generic assertion uses a bare call (the path
#891/#896 stdlib aggregates take), so #898 does not block this lane.

## Fixtures added

`examples/multi-module/issue-897-shadow/` (`agg.kai` + `main.kai` +
`main.out.expected`), wired into `test-multi-module` (tier1). The
fixture is self-contained — a local `Monoid` protocol with an arity-0
`empty()` op reproduces the exact bug shape without depending on
stdlib `sum` being generic (i.e. without depending on #896 landing
first). `agg.kai`'s `empty()` arity-0 op is the precise construct that,
pre-fix, degraded to the arity-0 dispatch panic stub under `T = Any`.

## Follow-ups

- #898 — qualified-call cross-module generic dispatch.
- Once #896 lands, its `examples/perceus/reuse_diagonal_guard.kai`
  (a user `fn sum(Tree)` over the now-generic stdlib `sum`) is the
  same bug shape against the real stdlib; it goes green on this fix.
