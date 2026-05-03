# Lane experience report — unions-cleanup-post-187

Best-effort retrospective by the implementing agent. See limitations
at the bottom.

## Objective metrics (from /tmp/lane-unions-cleanup-post-187-builds.tsv)

- Start: 2026-05-03T19:06:23-04:00
- End:   2026-05-03T19:29:22-04:00
- Wall-clock: ~23 minutes
- Build/test invocations:
  - `make tier0`:        6 invocations, 6 passes, 0 fails
  - `make tier1`:        1 invocation, 1 pass, 0 fails
  - `make tier1-asan`:   1 invocation, 1 pass, 0 fails
  - `make selfhost`:     1 invocation, 1 pass, 0 fails
  - `make selfhost-llvm`: 2 invocations (first attempt at repo root failed because target lives in stage2/Makefile), 1 effective pass, 0 fails

## Compiler errors I encountered

1. **`type-narrowing pattern targets X, which is not a component of Y`** — at typer (check_narrow_pattern) — fixed by reverting my first attempt at #197 (skip ctor registration unconditionally) and switching #198 from Option B to Option A so `variants_of_type` can fall back to the `[UnionInfo]` summary. Took ~2 attempts.
2. **`cyclic effect alias: AccountNotFound`** — at resolver — fixed by restructuring the new fixture `diagnostic_d3_mismatch.kai` to declare composite unions (the lone form `type X = X` got parsed as a self-referential effect alias). Took 1 attempt.
3. **`No rule to make target 'selfhost-llvm'`** — at make — fixed by running it from `stage2/` rather than the repo root. Took 1 attempt.

## Friction points

- **Brief was wrong about #197 being free** — the brief said "fan-out desugar covers the pattern matching path" so unconditionally skipping the ctor registration would be safe. It is not: `variants_of_type` is the single source of truth for "is X a component of Y?" both for D3 upcast and for `check_narrow_pattern`'s exhaustiveness side. Skipping registration without a fallback breaks every existing fixture that composes unions (`ddd_ledger_demo`, `upcast_construction`, etc.). The fix was to make #198 do real work first (Option A: thread `UnionInfo` into `TyEnv`) so the fallback path exists, *then* implement #197.
- **Recommended sequencing held up** — once I realized #198 had to be Option A to unblock #197, the original "do #198 first, then #197" ordering was correct. The brief flagged this possibility ("Option B unless Option A unlocks something for #197") but I had to walk into the failure to see why.
- **Dual representation IS easy to navigate, once you see it** — the variant table is the runtime/value-level representation; the new `env.unions` is the structural/component-level representation. Each has one job. The original Phase 2 attempted to put both jobs on the variant table, which is what produced the dead `[UnionInfo]` smell.

## Spec ambiguities or interpretive choices

- **Picked #198 Option A (thread UnionInfo into TyEnv)** — Option B (delete) broke #197. Concretely: I added a third field `unions: [UnionInfo]` to `TyEnv`, populated it in `build_ty_env` via `collect_unions(decls)`, and made `variants_of_type` merge the entry-side walk with `union_components_of(env.unions, tycon)`. `union_upcast_ok` consults `variants_of_type` and inherits the fallback for free. New helpers `union_components_of` and `union_parents_of` are also wired through to power #199's diagnostic.
- **Sequencing**: 198 → 197 → 199 worked after the Option A pivot. No reordering needed.
- **D3 diagnostic format adjusted from the brief's example.** The brief showed a parenthetical `expected: U (= A | B | C)` block. Real `st_unify` failures often surface at function-type granularity (the unifier reports `(U) -> Int` vs `(T) -> ?t0 / ?e0`, not `U` vs `T` directly), so I split the work into two passes: `emit_union_aware_notes` first prints the generic expected/found notes, then walks the structure to find the first divergent `TyCon`-vs-`TyCon` pair and emits focused notes (`union: X = ...`, `T is a component of P`). The hint discriminates the no-chain case (`expected union already lists found's parent` → suggest lifting through the parent) from the disjoint-unions case (suggest extending the expected union or narrowing). This matches the spirit of the brief's example without re-engineering `unify_env` to bubble narrower failure points.

## Subjective summary

- Confidence in correctness: **high**. tier0, tier1, tier1-asan, selfhost, selfhost-llvm all green; new + existing fixtures pass; selfhost byte-identical on both backends.
- Hardest sub-task: **#198 + #197 entanglement.** Realizing that #197 needed the structural `unions` side-channel that #198 was supposed to introduce was a small "stop and think" moment. Once seen, the fix was obvious.
- Easiest sub-task: **#199 diagnostic.** Once the `env.unions` lookup was in place, emitting structural notes was straight-line code.
- Did the previous milestone retros help? **Yes, especially Phase 3's retro.** Phase 3 explicitly named the dead-code smell on `[UnionInfo]` and the Phase 4 retro flagged the diagnostic gap — those two pointers led me directly to the right architectural shape. The Phase 2 retro's note that "the dual rep was deliberate to keep pattern matching unchanged" was also load-bearing: it told me NOT to rip the variant-table side out wholesale (which I almost did with Option B). What's missing from those retros: a pointer that `variants_of_type` is the single fan-in point, and that anything that subtracts from it must add a fallback. I'd add that pointer to the milestone-rollup retro.

## Limitations of this report

- Self-report bias acknowledged.
- Context truncation: counts and error lists exclude anything that fell out of my visible context window.
- Single agent (Claude). Not generalisable across LLMs.

## Raw build log

```
timestamp	cmd	outcome	elapsed_s
2026-05-03T19:10:08-04:00	tier0	OK	41
2026-05-03T19:16:29-04:00	tier0	OK	39
2026-05-03T19:18:29-04:00	tier0	OK	39
2026-05-03T19:20:07-04:00	tier0	OK	40
2026-05-03T19:21:10-04:00	tier0	OK	39
2026-05-03T19:22:42-04:00	tier0	OK	39
2026-05-03T19:23:30-04:00	selfhost	OK	19
2026-05-03T19:23:36-04:00	selfhost-llvm	OK	0
2026-05-03T19:24:12-04:00	selfhost-llvm	OK	27
2026-05-03T19:28:17-04:00	tier1	OK	239
2026-05-03T19:29:16-04:00	tier1-asan	OK	47
```
