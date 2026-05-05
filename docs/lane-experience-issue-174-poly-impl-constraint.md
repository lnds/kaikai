# Lane retro — issue #174 polymorphic impl bounded constraint

## Objective metrics

| metric | value |
| --- | --- |
| Issue | #174 — polymorphic impls panic at runtime when dispatching on a type variable |
| Branch | `issue-174-poly-impl-constraint` |
| Wall-clock | ~35 min (start `2026-05-05T12:38:56-04:00`, end `2026-05-05T13:14:00-04:00`) |
| Diff (kaikai) | ~190 net lines in `stage2/compiler.kai` |
| Fixtures added | 4 (3 positive + 1 negative) under `examples/protocols/` |
| Docs touched | `docs/protocols.md` (one new subsection) |
| Tier gates | tier0 / tier1 / tier1-asan / selfhost / selfhost-llvm — all green |
| Selfhost convergence | byte-identical, 1 round on each backend |

## Diagnosis (where the panic originated)

The panic message `__protocol_dispatch_Show_show` came from the
runtime body of the dispatcher stub `__proto_show` synthesised by
`generate_dispatchers` (`stage2/compiler.kai:39844`). The stub's
body is `panic("__protocol_dispatch_Show_show")`. It is intended to
be unreachable — the post-inference `resolve_protocol_calls` rewrite
should retarget every reachable `__proto_show(x)` call into the
matching `__pimpl_Show_<T>_show`. The rewrite uses
`ty_head_name(arg.ty)` to extract the concrete head constructor;
when the receiver's type is `TyVarT(_)` (the case inside a
polymorphic impl body where `x : T`), the extractor returns `None`,
the rewrite falls through, and the dispatcher's panic fires at
runtime.

So the runtime panic was a **monomorphisation gap, not a typer
bug**: the typer correctly accepted the impl body (it type-checks
because the dispatcher has scheme `forall a. (a) -> String`), but
the post-inference rewrite cannot resolve `T` until the impl is
specialised to a concrete `T'`. By the time `monomorphise` ran and
substituted `T → T'` in `Expr.ty`, the rewrite walker had already
finished — and `rewrite_callsites_decl_sm` (the per-spec walker)
only retargets calls to **user-poly** functions, not to dispatcher
stubs.

## Family chosen + why

**Family 2 — impl-site bounded constraint.**

```kai
impl[T : Show] Show for [T] { fn show(xs: [T]) : String = ... }
```

Why Family 2:

- **Tier 1 #2 (runtime efficiency)**: kept. Bounds inform compile-
  time validation only; the runtime artifact is identical to a
  non-bounded poly impl — direct call to the per-`T'` impl, no
  dictionary, no hidden parameter.
- **Tier 1 #3 (no constraint propagation, no HKT)**: respected.
  Constraints exist only inside the impl-site brackets; they do NOT
  propagate into ordinary function signatures. `fn foo[T : Show](x :
  T)` remains unparseable.
- **Issue #174 acceptance** (the cryptic runtime panic must go):
  met. The post-monomorphisation dispatch validator catches the
  case where a polymorphic impl body recurses on `T` and is
  instantiated for a concrete type without the corresponding impl,
  reporting a typed compile-time error pointing at the offending
  `show(x)` call.

Family 1 (reject + diagnostic) was the documented fallback. It
would have closed off `impl[T] Show for [T]` entirely, forcing
stdlib explosion. Family 2 keeps the natural shape; the bound
documents the dependency and lets the post-mono validator
distinguish "impl body needs `Show` on `T`" from "impl body never
recurses on `T`."

## Algorithm (parser, resolver, monomorphiser changes)

### Parser

A new helper `parse_impl_type_params_loop` (only used by
`parse_impl_decl`) parses each impl-site tparam with an optional
suffix:

- `: Type` / `: Measure` — kind annotation (legacy m12.5).
- `: Show + Eq + Ord` — protocol bound list.

Bounds are encoded into the legacy `String` slot via
`tp_make_with_bounds`:

```
"T"            no bounds
"T#Unit"       Measure kind (legacy)
"T#b:Show"     single bound
"T#b:Show,Eq"  multi-bound
```

The `#` is collision-free with kaikai identifiers (lexer rejects
it), so the encoding piggy-backs on the existing `tparams: [String]`
plumbing without an AST extension.

### AST flow

`prepend_impl_tparams_to_methods` strips the bound suffix via
`tp_strip_bounds` before injecting tparams into the method's tparam
list. The rest of the pipeline (type resolution, monomorphisation,
mangling) sees plain `T` strings and is unchanged.

The bounds themselves are intentionally **not** carried further:
they exist only to communicate intent at the impl-site and to
unlock the cleaner missing-bound diagnostic. The post-mono
validator does not need them — it operates on the residual
`__proto_<op>(x : ConcreteT)` calls in the spec body.

### Monomorphiser

`monomorphise` now takes the `ProtocolReg` and threads it through
`generate_specs_iter` → `emit_spec`. Inside `emit_spec`, after
`subst_decl` rewrites `Expr.ty` with concrete types, a new step
runs `resolve_protocol_calls_decl(spec1a, reg)` so any `__proto_op(x
: ConcreteT)` retargets to the matching `__pimpl_<P>_<T>_<op>`.
This is the smallest possible change that lets the rewrite see the
post-substitution types.

`compile_source` then runs `validate_resolved_decls` over the post-
mono `[Decl]` to catch any `__proto_<op>(x : ConcreteT)` that
survived. Such a call is the residue of a polymorphic impl
instantiated for a type that lacks the required impl; the validator
emits the same `no impl of <P> for type <T>` diagnostic that the
pre-mono validator already emits for the typed-AST case.

## Empirical verification

Three cases from the lane brief, all running locally on
`stage2/kaic2` after the fix:

1. `impl[T : Show] Show for [T] { ... }` with recursive body, used
   on `[Int]` → prints `[1, 2, 3]`. ✅
2. `impl[T] Show for [T] { ... }` (no bound) with recursive body,
   used on `[Box]` (no `Show for Box`) → compile-time error
   `error: no impl of Show for type Box (operation show)` with
   source location pointing at the `show(x)` call inside the impl
   body. ✅
3. `impl[T] Show for [T] { ... }` with non-recursive body, used on
   `[Int]` → prints `list-of-something`. ✅

Plus a multi-bound case: `impl[T : Show + Eq] Show for [T] { ... }`
parses, the second bound is captured but inert, runs identically to
the single-bound case. ✅

## Friction points

- **The bound suffix encoding via `String`** turned out cleaner
  than extending `DImpl` with a new field. Every walker that
  pattern-matches `DImpl` would have needed a touch otherwise; the
  string trick localised the change to parser + a strip helper.
  Trade-off: the encoding is a small parsing convention rather
  than a typed AST node. The `tp_make_with_bounds` /
  `tp_bounds_of` / `tp_strip_bounds` triple is the documented
  surface for future readers.
- **Selfhost convergence was clean** (1 round on each backend), in
  contrast to the lane brief's "if not byte-identical in 3
  iterations, STOP" warning. The change is well-localised: the
  parser path is touched only inside `parse_impl_decl`, and the
  monomorpher's new step is a re-application of an existing pass.
- **The fixture naming convention** is `<name>.kai` paired with
  `<name>.err.expected` for negative cases (NOT `<name>.err.kai`).
  The first attempt used `.err.kai`; the test runner did not
  recognise the matching `.err.expected` because it derives the
  pair name via `basename $$f .kai`. Renamed once tier1 reported
  it.
- **The bounded-vs-unbounded boundary held tight**. The bounds are
  stripped at the parser/AST boundary, so they never touch the
  typer or the monomorpher — making slip into full constraint
  propagation impossible by construction. CLAUDE.md Tier 1 #3 is
  preserved.

## Subjective summary

Family 2 turned out simpler than the lane brief estimated. The
heavy lift was in the monomorpher (one `resolve_protocol_calls_decl`
re-application after `subst_decl`); the parser change is bookkeeping.
The bound itself is documentation-as-syntax: the runtime artifact
is identical to a non-bounded poly impl. What makes Family 2 work
is the **post-mono dispatch validator** — that is the piece that
upgrades the runtime panic into a compile-time error.

The acceptance gate was met without falling back to Family 1.

## Limitations (out of scope)

- **Function-level constraints stay prohibited.** `fn foo[T : Show](x :
  T)` is not parseable. Constraints exist only inside `impl[...]`
  brackets. This boundary is the line CLAUDE.md draws against
  Haskell-style typeclasses.
- **No HKT, no type families, no functional dependencies.** Same
  reason.
- **The bound is presently inert beyond syntax** — i.e., declaring
  `T : Eq` does not statically check that the impl body is allowed
  to call `Eq` ops on `T`. The body still type-checks against the
  dispatcher's polymorphic scheme; the missing-impl diagnostic
  fires at monomorphisation time, not at impl-site validation time.
  Tightening this is a future increment if the diagnostic ever
  needs to be earlier; for the v1 the post-mono check is precise
  enough (it points at the offending `show(x)` call).
- **Multi-method polymorphic impls** (`impl[T : Show + Eq] MyP for
  [T]` where `MyP.op` calls both `show` and `eq` on `T`) inherit
  the same machinery and were not separately tested. The
  monomorpher's per-spec rewrite handles each call independently;
  no shared state.

## Builds

```
timestamp                       cmd            outcome  elapsed_s
2026-05-05T12:50:09-04:00       tier0          OK       47
2026-05-05T12:55:56-04:00       tier1          OK       235
2026-05-05T13:09:39-04:00       tier1          OK       235
2026-05-05T13:10:37-04:00       tier1-asan     OK       -
2026-05-05T13:11:16-04:00       selfhost       OK       -
2026-05-05T13:12:15-04:00       selfhost-llvm  OK       -
```

(Two tier1 entries — first run flagged a fixture-naming mistake,
second run after rename was clean.)
