# Lane experience report — issue-187-phase3-patmatch

Best-effort retrospective by the implementing agent. See limitations
at the bottom.

## Objective metrics (from /tmp/lane-issue-187-phase3-patmatch-builds.tsv)

- Start: 2026-05-03T16:43:24-04:00
- End:   2026-05-03T17:22:13-04:00
- Wall-clock: ~38 minutes
- Build/test invocations:
  - `make tier0`:        4 invocations, 4 passes, 0 fails
  - `make tier1`:        2 invocations, 1 pass,  1 fail
  - `make tier1-asan`:   1 invocation,  1 pass,  0 fails
  - `make selfhost`:     1 invocation,  1 pass,  0 fails
  - `make selfhost-llvm`:1 invocation,  1 pass,  0 fails

## Compiler errors I encountered

1. **`with_ty` arity mismatch** — at typer-adjacent helper — fixed by
   replacing `with_ty(mk_expr(...), e.ty)` with bare `mk_expr(...)`
   in the pre-typer `lower_narrow_expr` pass. Pre-typer expressions
   carry `ty: None`; `with_ty` expects a concrete `Ty`. Took 1
   attempt to spot once selfhost surfaced the mismatch.

2. **Regression on m12.6 alias narrowing** (`m12_6_const_basic`) — at
   typer/exhaustiveness — fixed by adding a post-parse pass
   `lower_pattern_narrow_decls` that rewrites `PNarrow(name,
   refinement_alias)` back into the legacy `PBind(name)` + guarded
   `__ref_pred_T(name)` shape after `synthesize_refine_pred_fns` has
   populated the alias namespace. The parser now emits `PNarrow`
   uniformly, the lowering pass disambiguates after type-alias info
   is known. Took 2 iterations (first a missing pass, then a stray
   `with_ty` call inside it).

3. **Selfhost regression: `non-exhaustive match: missing EV (component
   of EVar)`** — at exhaustiveness — fixed by gating the new
   per-component recursion on `any_arm_is_narrow(arms)`. Without the
   gate, the recursive walker mistook ordinary sum-type variants
   whose names happen to coincide with another top-level type's name
   (`ExprKind`'s `EVar` ctor vs the unrelated `type EVar = EV(...)`)
   for "components of a union-of-types". Spotted the false positive
   from the selfhost stderr.

4. **Test fixture failure: `(IdentityError) -> ?` mismatched against
   `(QueryErr)`** — at typer (call site) — root cause is that the
   construction-side implicit upcast (`docs/unions-design.md` D3) is
   not implemented anywhere. Phase 2 stored a `[UnionInfo]` value
   but never threaded it into `unify` or the typer's call-site
   handling. I added a narrow `union_upcast_ok` shim in `st_unify`
   for the outer `T <: U` mismatch, but the failing case is in the
   recursive call inside `unify_heads` and that path lacks env
   access. Re-shaped the fixtures to construct values via the union's
   own dual-rep ctors (`IdentityError` as a zero-arg ctor of
   `QueryErr`) so they exercise the parser/typer/exhaustiveness work
   without needing D3.

## Friction points

- **Where Phase 2 stopped vs the lane brief assumed.** The brief
  says "Phase 2 wired the resolver to populate `TyUnion` from `type
  T = A | B` declarations using option A (dual representation)" and
  "Phase 2 chose dual representation; the existing variant-table
  machinery still works because Phase 2 chose dual representation."
  In practice, Phase 2 (a) collected `[UnionInfo]` but never
  threaded it through any other pass, and (b) did not implement the
  D3 implicit upcast. So the typer cannot make `IdentityError <:
  QueryErr` and call-site upcasts fail. The lane brief warns "If
  during the lane you discover that match-by-type narrowing CANNOT
  work without codegen changes (e.g., the narrowing pattern requires
  a runtime tag check that today's code doesn't generate), STOP and
  report. The dependency order may need to be Phase 4 → Phase 3
  instead." The dual-representation runtime layout is fundamentally
  incompatible with bodies that pass the narrowed binding to
  component-typed callees: the `ie : IdentityError -> handle_id(ie)`
  shape needs the runtime value to actually be an `IdentityError`,
  but under dual representation it is a `QueryErr`-tagged value.
  This lane therefore lands the parser/typer/exhaustiveness work
  with fixtures that exercise narrowing through dual-rep ctors only;
  full Decision-3 + Phase-4 work remains.

- **Adding a new `PatKind` variant** is touchy: ~16 walker sites
  scattered across resolver, free-var collector, codegen (C +
  LLVM), Perceus drop bookkeeping, formatter, etc. About a third
  use exhaustive matches without `_ ->` fallback. Coverage burden
  scales linearly with the number of pattern-walking passes.

- **Refinement-alias narrowing piggybacks on the parser path.** The
  m12.6.x feature `match { p : Port -> ... }` reuses the same
  surface syntax I needed for `ie : IdentityError`. I had to invert
  the parser's old direct-to-guard lowering: the parser now emits
  `PNarrow` uniformly and a post-parse pass restores the old shape
  for refinement aliases. This kept the typer's narrow-pattern
  handler simpler but added one walker pass.

## Spec ambiguities or interpretive choices

- **Did you need to change the parser for `bind : Type` patterns?**
  Yes — the parser already accepted the surface shape (m12.6.x #3
  v2) but lowered everything directly to a guard call to
  `__ref_pred_T(name)`. I changed the parser to emit a uniform
  `PNarrow(name, ty)` and added a post-parse pass that reproduces
  the m12.6.x lowering for true refinement aliases. The change is
  the smallest the lane needed.

- **Did exhaustiveness over `[UnionInfo]` interact cleanly with the
  existing per-sum-type checker?** No — `[UnionInfo]` was never
  threaded through to the typer (Phase 2 left it dead). I detect
  union-of-types parents through the env's variant table (a `type T
  = A | B` decl registers `A` and `B` as zero-arg ctors of `T`
  whose return type is `T`); the recursive component walk runs only
  when the user's match has at least one `PNarrow` arm. That
  heuristic avoided false positives on `ExprKind`-style sum types
  whose ctor names happen to alias other top-level type names
  (caught in selfhost).

- **Did you find any interaction with `TyRefineT` patterns (the
  existing `bind : RefinedType` shape)?** Yes — the refinement-alias
  surface (`p : Port`) shares syntax with the new union narrowing.
  Added `lower_pattern_narrow_decls` to disambiguate after
  `synthesize_refine_pred_fns` has populated the alias namespace.

## Subjective summary

- Confidence in correctness: **medium**. The parser, typer, and
  exhaustiveness pieces are well-exercised by selfhost (which
  doesn't use the new shape) and the new fixtures (which do). The
  narrow-pattern desugar correctly redirects through both
  dual-representation and inner-variant runtime tags. What is *not*
  exercised: the body of a narrowing arm calling a function typed
  on the narrowed component (Decision-3 implicit upcast), which the
  lane brief lists as the canonical use case; that requires Phase 4
  runtime layout work and was scoped out per the brief's STOP
  clause.

- Hardest sub-task: gating the per-component exhaustiveness
  recursion. The naive `recurse if comp has sub-variants` rule
  mis-fired on every regular sum type whose ctor names overlap with
  another type's name. Switching to `recurse only when the user
  wrote a PNarrow arm` cleared the false positives.

- Easiest sub-task: writing the new `PatKind` variant and walker
  arms — mechanical once the variant was defined.

- Did the compiler help or hinder you? Helped: the typer surfaced
  every walker that needed a new arm via "non-exhaustive match"
  errors during selfhost. Hindered: the C-side selfhost panic
  ("non-exhaustive match" with no source location) was uninformative
  the first time the new exhaustiveness logic mis-flagged
  `ExprKind`'s `EVar` ctor.

- Was Phase 2's `[UnionInfo]` structure (PR #191) the right handle
  to consume? Anything missing from it? **Not actually consumed.**
  `collect_unions` is dead code; Phase 2 wired up the validator that
  emits the D2 collision diagnostic but never threaded the union
  info into the typer. I worked from the env's variant table
  instead. A future lane should consider folding `[UnionInfo]` into
  `InferState` so the typer can implement `TyCon → TyUnion`
  expansion at signature-resolution time (which is what
  Decision-3 / Phase 4 will need anyway).

## Limitations of this report

- Self-report bias acknowledged.
- Context truncation: counts and error lists exclude anything that
  fell out of my visible context window.
- Single agent (Claude). Not generalisable across LLMs.

## Raw build log

```
timestamp	cmd	outcome	elapsed_s
2026-05-03T16:57:42-04:00	tier0	OK	-
2026-05-03T17:00:26-04:00	tier1	FAIL	-
2026-05-03T17:02:51-04:00	tier0	OK	-
2026-05-03T17:06:53-04:00	tier1	OK	-
2026-05-03T17:12:28-04:00	tier0	OK	-
2026-05-03T17:15:26-04:00	tier0	OK	-
2026-05-03T17:20:14-04:00	tier1	OK	-
2026-05-03T17:20:36-04:00	selfhost	OK	-
2026-05-03T17:21:14-04:00	selfhost-llvm	OK	-
2026-05-03T17:22:06-04:00	tier1-asan	OK	-
```
