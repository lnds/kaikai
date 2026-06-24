# Lane experience — issue #891: generalise stdlib aggregates to protocol-bounded type params

## Scope as planned vs as shipped

**Planned (per #891 + brief):**
- Tier 1: `max`/`min`/`sort` → `[T : Ord]`, `uniq` → `[T : Eq]` (mechanical, protocols already carry the ops).
- Tier 2: extend `Numeric` to a ring (`add`/`mul`/`zero`/`one`), then `sum`/`product` → `[T : Numeric]`.
- Tier 3: migrate one compiler ad-hoc dedup site to the generic `uniq` as an in-compiler smoke test.
- Fixtures: arity overload, Numeric over Int+Real, uniq over a non-Int type, the mixed `Eq` + polymorphic `impl Eq for Wrapper[T]` (#877 monomorphisation-order risk).

**Shipped:**
- Tier 1 + Tier 2 in full. All six aggregates generalised; `Numeric` is a ring.
- Tier 3 **dropped as structurally infeasible** (see below) — the in-compiler smoke test cannot be done; the `Wrapper[T]` fixture covers the same monomorphisation-order risk instead.
- Five fixtures, all green on C **and** native.
- **Five** compiler bugs surfaced. Three were fixed upstream before the lane resumed (#894 op-name collision, #899 bound-blind shadowing, #900 interpolation collision); **two are fixed in this lane** — qualified-call monomorphisation (`fix(monomorph)`) and root-fn shadowing in value position (`fix(typer)`) — because both make a clean generalisation impossible without them (the canonical `list.sum(...)` form, and any user fn named like a ring op).

## Five compiler bugs the brief did not anticipate

The brief assumed the work was mechanical once #890 shipped. It was not — the generalisation surfaced **four** distinct compiler bugs. Three were fixed upstream (#894/#899/#900); the fourth (qualified-call monomorphisation) is fixed in this lane. The honest arc: the first three were each found by a spike, but those spikes used only the **bare** call form. The qualified `list.sum(...)` form — the canonical one documented in `stdlib-layout.md` — was not exercised until CI (tier1 `corec_corrupt_blob`, which calls `list.sum`) went red, and even then the first read attributed all of CI red to #899/#897. Only re-running every red job and diffing the qualified vs bare emitted C against pristine main isolated bug 4. Verification gap, recorded so it is not repeated: **a stdlib fn has more than one call form (bare, qualified, UFCS, pipe); a generalisation must be verified on each, not just the bare one.**

### Bug 1 — multi-protocol op-name collision (blocker, fixed via #894)

`Numeric` was to gain `add`/`mul`. But `protocol Add[a]` / `Mul[a]` already declare `add`/`mul` (with Int/Real impls) in `protocols.kai`. With the same op name declared by two protocols, a bare `add(a, b)` inside a `[T : Numeric]` body took the resolver's multi-candidate path (`proto_ops_lookup_all`), picked `Add`'s impl, and statically rewrote the call to a boxed-only impl symbol invoked with the unboxed ABI → segfault. Verified the contrast: `cmp` (unique to `Ord`) and `abs` (unique to `Numeric`) dispatched fine; a uniquely-named ring (`Ringy { rplus; rzero }`) worked end to end (`sum=10`, `product=24`). So the ring design was sound; only the name collision broke. Filed as the multi-protocol-op-collision bug; the integrator fixed it via #894 (`disambiguate protocol ops sharing a name across protocols`), after which the clean `add`/`mul` names work.

**Lesson:** when extending a protocol with an op, grep `protocols.kai` for the op name first — kaikai has no protocol namespacing, so a shared op name across two protocols is a latent dispatch hazard.

### Bug 2 — interpolation call-site collision (fixed upstream via #900)

After #894, `sum`/`product` over Int and Real in one program still produced garbage / segfaults — but only inside string interpolation. Root-caused (verified against emitted C + `--dump-mono`): `parse_interp_expr` reparses each `#{...}` segment in isolation starting at line 1, col 1, so two calls to the same `[T:P]` fn at different types both record their call-site instantiation at `(1,1)`; `find_mono_inst` returns the first match, so both sites resolve to the first spec → wrong ABI. Real is incidental — it is just the only type whose wrong-ABI read is visibly catastrophic (Int+String survive the collision). Reproduced with a plain user protocol, no stdlib involvement. Filed as #895, fixed upstream via #900 (`disambiguate interpolation call-site position collision by arg type`).

### Bug 3 — bound-blind shadowing regression (fixed upstream via #899)

A root-level user `fn sum(t: Tree)` that cleanly shadowed the old monomorphic `sum([Int])` started panicking (`no impl of Numeric.zero`) once `sum` became `[T:Numeric]`. The typer resolved to the user fn correctly, but the monomorphiser's call-site rewriter collected the stdlib `sum[T:Numeric]` as a poly by bare name, failed to unify `[T]` against `Tree`, defaulted the unbound tparam to `TyAny` (classified concrete), and emitted a bogus `sum__mono__Any` whose `zero()` panicked. Filed as #897, fixed upstream via #899 (`decline poly call-site synth on shape/arity mismatch`).

### Bug 4 — qualified-call monomorphisation (fixed in THIS lane, `fix(monomorph)`)

After rebasing onto #899/#900, `list.sum([7,8])` (the canonical **qualified** form) still panicked `Numeric.zero` and `list.sort([3,1,2])` returned `[2,1,3]` unsorted — while the **bare** forms worked. Root cause (mapped + verified against emitted C): the monomorphiser's call-site rewriter (`monomorph.kai` `rewrite_callsites_kind_sm`) matched only `ECall(EVar(name))` callees; a qualified call is `ECall(EModCall(mod, name), …)`, which fell to the pass-through arm and was never retargeted to its `__mono__` spec. The typer recorded the instantiation correctly (under the bare name) and the spec was emitted, but `main` kept calling the un-instantiated generic `kai_list__sum`, whose nullary `zero()`/`one()` left runtime dispatchers (panic) and whose `sort` comparator stayed generic. `max`/`min`/`uniq` only *appeared* to work qualified because their binary ops dispatch dynamically — the call site was equally un-rewritten. General to any qualified call of a `[T:P]` fn, not stdlib-specific.

Fix: add an `EModCall` arm to the rewriter mirroring the `EVar` arm (mangle the bare name, keep the module prefix on the callee so emit mints `kai_<mod>__<mangled>`), plus the parallel arm in the spec-discovery walker so transitive qualified poly calls seed their specs. No typer change. Regression fixture: `examples/stdlib/aggregate_qualified_call.kai`.

**The `EModCall` arm needed TWO rounds of guarding — each regression caught by a different full tier, not by the lane fixtures.** The typer records a call-site inst for *every* qualified callee, and the spec worklist keys tuples by **bare name** (`generate_specs_iter` → `lookup_poly_fn`), so the bare-name `MonoTuple` cannot distinguish two same-named generics in different modules.

Round 1 (`map_round_out`, caught by `test-stdlib`): `collections.map.filter` is tparam-less (its `k,v` come from `Map[k,v]`, NOT in `polys`), but `core.list.filter[a,e]` IS. A bare-name rewrite of `map.filter(...)` minted `filter__mono__String__Int`, a symbol the generator (which would have used `list.filter`) never emitted → undeclared. First guard tried: `poly_fn_in_module` — rewrite only when a generic of that name originates in `modn`.

Round 2 (`stream_early_stop`, caught by `test-effects`): that guard was insufficient. `stream.map[a,b,e]` and `list.map[a,b,e]` BOTH have tparams and BOTH genuinely live in their modules, so `stream.map` passed `poly_fn_in_module` and got rewritten to `stream__map__mono__Int__Int` — but the generator's bare-name `lookup_poly_fn("map")` returns `list.map` (first registered), emitting `list__map__mono__...` instead → undeclared `stream__map__...`. The compiler even printed `did you mean 'kai_list__map__mono__Int__Int'?`, naming the culprit.

Final guard: `poly_fn_unique_in_module` — rewrite a qualified call ONLY when exactly one module defines that name as a generic, and it is `modn`. My aggregates (`sum`/`product`/`sort`/`uniq`, each unique to `core.list`) qualify and monomorphise; `map`/`filter` (shared by `list` + `stream`) and tparam-less `map.filter` fail the guard and stay ordinary generic calls — which is exactly how they behaved on main (verified: `stream.map` qualified worked un-monomorphised before this lane). The rewriter and the discovery walker share the guard so they stay in lockstep. The deeper truth: a `MonoTuple` carries no module, so cross-module same-named generics are fundamentally ambiguous to the spec worklist; until the tuple is module-keyed, the only sound qualified-call rewrite is the unambiguous (unique-name) case. Lesson: each new compiler guard needs the *full* relevant tier (`test-stdlib`, `test-effects`, `test-kir`), not just the lane's own fixtures — two distinct regressions hid behind the first green fixture run.

### Bug 5 — root-fn shadowing breaks in value position (fixed in THIS lane, `fix(typer)`)

Adding the nullary `zero()`/`one()` ring ops to `Numeric` broke `examples/effects/issue_668_map_large_in_fiber.kai`, which defines its own `fn one(n: Int) : [Int]` and passes it as a value: `list.flat_map(xs, one)`. The bare `one` resolved to the protocol dispatcher `__proto_one` (`() -> Self`) instead of the user's `fn one(n)`, so the callback type mismatched. `--dump-typed` named it: `ident __proto_one : () -> ?t`. The #748 root-fn-shadows-stdlib invariant held on the *call* path (arity-aware) but not the *value* path. Root cause (precise): the bare-name → `__proto_<op>` rewrite happens *before* the typer, in `rename_proto_calls_kind` (`protos.kai`); its `EVar` value-position arm rewrote on a plain protocol-op-name match with no shadow check. Because the user's `fn one` is arity 1 and `Numeric.one()` arity 0, the call-path's arity disambiguation never filtered it, and the value path had no check at all. Fix: thread the module's top-level fn names (any arity) into the rename pass and skip the value-position rewrite when the name is locally shadowed — mirroring #748. The call path is untouched, so `Ord.min(a,b)` still reaches the dispatcher next to a local `fn min(xs)`; and with no same-named root fn the proto op still resolves normally (`test-protocols` no-shadow cases stay green). This is the only fix in the lane that touches resolution before the typer rather than the monomorphiser. Lesson: a protocol op named like a common user fn (`zero`/`one`/`add`/`mul`) is a shadowing hazard in *every* position the name can appear, not just at the call site.

**Workaround in fixtures:** bind every `[T:P]` call (the aggregates and the `show` renders) to `let` and interpolate only plain variables. With that, Int+Real coexist correctly — verified outside interpolation: `sum`/`product` give `10`/`5`/`24`/`3`. The fixtures document the workaround inline so they are not silently fragile.

**Lesson:** a fixture that interpolates `#{generic_fn(x)}` at two types is a #895 tripwire, not a clean feature test. Until #895 lands, multi-instantiation fixtures must avoid interpolating the polymorphic call directly.

## How monomorphisation handled each aggregate

All six are the existing #890 mechanism: strip the bound, type the body with `T` abstract leaving proto-op calls (`cmp`/`eq`/`add`/`mul`/`zero`/`one`) as dispatcher calls, then the monomorphiser re-runs `resolve_protocol_calls_decl` per concrete instantiation. No typer threading.

- `max`/`min`: loops route through `cmp(h, best) > 0` / `< 0` instead of `>`/`<` on Int. `Option[T]` wrapper preserved (empty → `None`).
- `sort`: `sort_by(xs, (a, b) => cmp(a, b))` — the lambda captures the `Ord` witness from the enclosing `[T:Ord]` scope; the mono re-run specialises it. (`int_cmp` is no longer the `sort` comparator but stays a public helper.)
- `uniq`: a new `eq_member` routes membership through `eq(h, x)` (not the builtin `==` that `contains` uses), so the witness monomorphises — required for types like `Wrapper[T]` where structural `==` is not the intended equality.
- `sum`/`product`: tail-recursive accumulator loops seeded from `zero()` / `one()` and folding `add` / `mul`. The nullary `zero`/`one` (Self only in return position) resolve by the expected return type, same mechanism as `Default.default()`; verified they work at Int and Real in one program (outside interpolation).

## The Numeric ring extension

Extended `protocol Numeric` with `add`/`mul`/`zero`/`one` and added them to all three impls: `for Int` (`+`/`*`/`0`/`1`), `for Real` (`+`/`*`/`0.0`/`1.0`), and `for Decimal` (delegating to the module's existing `add`/`mul`/`zero` + `from_int(1)`).

**`decimal.kai` was NOT optional — the typer checks impl completeness at import, not at dispatch.** The first build looked clean (selfhost byte-identical), but `test-stdlib-modules` (real import path, not the bundle-concat self-host) immediately failed three modules: `decimal.kai` (and `fx.kai`/`money.kai` which import it) reported `impl Numeric for Decimal is missing required method add/mul`. The self-host masks this because the compiler bundle does not include `decimal.kai`; only a real `import decimal` triggers the completeness check. This is exactly the "bundle-concat hides what real imports catch" trap — the module gate is what caught it. Completing the Decimal impl fixed all three and, as a bonus, makes `sum`/`product` work over `[Decimal]` too.

Updated the module `#[doc]` (it previously asserted `fn min[T:Ord]` "does not exist" — true before #890, now stale).

## Resolver-shadowing interaction — no hardening needed

The arity overload (`max(xs)` arity-1 vs `max(a,b)` arity-2) needed nothing: verified the provided fixture prints `a = 3 y b = 2` unchanged. The #877 monomorphisation-order risk (witness collapse must precede the #174 polymorphic-impl validator) is covered by `aggregate_uniq_wrapper_eq.kai` — `uniq` over `[Wrapper[Int]]` with `impl[T:Eq] Eq for Wrapper[T]` drops the duplicate via nested `eq(Wrapper) → eq(inner)`; it works, so no hardening was required.

## Tier 3 — structurally infeasible, dropped

Migrating a compiler dedup site to the generic `uniq` cannot be done, for two independent reasons discovered by trying:

1. **stage1 does not parse `impl`.** Migrating `collect_distinct_tuples` (dedup over `MonoTuple`) needed `impl Eq for MonoTuple`. The stage1 compiler (kaikai-minimal, which compiles the stage2 bundle) rejects `impl` declarations outright — `expected fn, type, test, or import`. The compiler has zero `impl` blocks in its own sources, by design.
2. **The compiler bundle does not link stdlib `core/list`.** Migrating `dedup_strs` (dedup over `[String]`, which would not need a new impl since `Eq for String` is in stdlib) failed with `undefined name uniq` — the self-hosted compiler is deliberately self-contained (`bundle.kai` documents "NOT taken from stdlib/core"), so `uniq` simply does not exist in that translation unit.

So the compiler is a closed world that consumes neither the stdlib being generalised nor the protocol-dispatch surface the generalisation relies on. The in-compiler smoke test the brief wanted is not achievable; the `Wrapper[T]` fixture exercises the same monomorphisation-order risk in a real program instead. This is worth a roadmap note so the next lane does not re-attempt it.

## Fixtures added

- `aggregate_overload_arity.kai` — provided verbatim; arity-1 `max(xs)` vs arity-2 `min(a,b)` coexist. Golden `a = 3 y b = 2`.
- `aggregate_numeric_ring.kai` — `sum`/`product` over Int and Real + empty-list identities; `let`-bound to dodge #895.
- `aggregate_ord_eq.kai` — `max`/`min`/`sort` over Int+String, `uniq` over Int+String; `[T:P]` calls `let`-bound.
- `aggregate_uniq_wrapper_eq.kai` — `uniq` over `[Wrapper[Int]]` with polymorphic `impl Eq` (#877 risk).

All four wired into `test-stdlib` (auto-globbed), green on C and native.

## Cost vs estimate

The "mechanical" half (Tier 1) was mechanical. Tier 2 was two compiler-bug investigations before a single line of the ring extension could be trusted to run — the brief's "now they work, the bug is fixed" held only for bug 1; bug 2 (#895) was new. Tier 3 was a dead end discovered only by attempting both candidate migrations and hitting two separate bootstrap walls.

## Follow-ups for next lanes

- **#895** (interpolation call-site collision) — open; once fixed, the fixtures can interpolate the aggregate calls directly and drop the `let`-binding workaround.
- **In-compiler dedup reuse** — not achievable while the compiler avoids stdlib and stage1 lacks `impl`; do not re-attempt without lifting one of those constraints.
