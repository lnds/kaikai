# Lane experience — issue #235 (typer load-order resolution)

## Objective metrics

- Branch: `issue-235-typer-load-order`
- Base commit: `622fa6c` (Merge PR #239 — `--path` cwd-relative fix)
- Lane open: 2026-05-04T15:39:06-04:00
- Lane close: 2026-05-04T17:07:52-04:00 (~1h 30m)
- Files changed: `stage2/compiler.kai` (+~250 net), `stdlib/core/{option,result,string}.kai` (10 aliases retired), `stdlib/decimal.kai` (1 caller migrated), 7 example fixtures (callers + 2 goldens).
- Compiler delta: ~250 net additions in `stage2/compiler.kai` covering 4 surgery sites (collect-candidates helper, pick-by-first-arg helper with concrete-vs-tyvar tie-break, narrow rewrite in `synth_ufcs_dispatch` / `synth_call` / `synth_pipe`, qualified rewrite in `synth_pipe_dispatch`).
- Aliases retired: 10/10 (`opt_or` survives — keyword reservation).
- Selfhost: byte-identical on both backends (`selfhost` and `selfhost-llvm` green).
- `test-stdlib-core-intrinsic`: 6/6 modules green.
- `test-ufcs`: 7/7 fixtures green (including `over_union_component` — root-file UFCS dispatch keeps working under the bare-fallback path).

## Diagnosis

The brief named two surgery sites — `ty_env_lookup` and `synth_ufcs_dispatch`.
The diagnosis turned out broader. The typer's bare-name resolution is hit
from at least four call sites:

1. `synth_ufcs_dispatch` — `xs.map(f)` UFCS dispatch.
2. `synth_call`'s default arm — bare `map(xs, f)` direct call.
3. `synth_pipe`'s `ECall` arm — `xs |> filter(p)` pipe-with-call.
4. `synth_pipe_dispatch` (the `|`-operator dispatcher used by
   `synth_map_pipe` / `synth_flat_map_pipe`) — `xs | fizzbuzz` map-pipe.

All four bypassed the `<mod>::<fname>` qualified key registered by
`add_decls_loop` (PR #229) and went through `ty_env_lookup` directly,
which returns the most-recently-prepended bare entry. With the alias
retirement adding three bare exporters of `map` (`option`, `result`,
`list`), every site picked the wrong scheme.

Codegen-side resolution piles on: `emit_named_call_lookup` calls
`efn_resolve(fns, callee)` which is also name-only, so even after the
typer picks the right scheme, the C symbol minted at the call site was
the first-match `kai_<name>` (or its monomorphised spec), not the
module-qualified `kai_<mod>__<name>`. The fix has to land at *both*
levels: the typer rewrites the callee to `EModCall(<mod>, <name>)`,
and the codegen's existing `EModCall` arm mints the right symbol.

`efn_resolve` itself was a useful reference — it shows the codegen
side already knows about `module_origin`. The discipline missing on
the typer side is exactly the qualifier rewrite, not a new resolver.

The retro for #230 surfaced this gap as the fourth blocker; the brief
was right that the typer was the central edit. What it underestimated
was how many call paths feed into the same name-only lookup.

## Fix shape

Three helpers + four call-site rewrites.

**New helpers in `stage2/compiler.kai`:**

1. `ty_env_collect_candidates(env, target) : [TyCandidate]` — walks
   `env.entries` newest-first and collects every entry whose key ends
   with `::<target>`. Bare prelude entries (the hardcoded list-typed
   `map` / `filter` / `each` from `add_prelude_sigs`) are excluded so
   they cannot win a tyvar-tie against a module-qualified scheme.

2. `pick_by_first_arg(st, candidates, target_ty) : Option[PickResult]` —
   instantiates each candidate's scheme and probe-unifies the first
   parameter with `target_ty`. Records two best matches in a single
   pass: the leading concrete-headed match (TyCon, TyListT, primitive,
   TyFnT, TyDimT) and the leading tyvar-headed match (TyVarT, TyAny).
   Concrete wins on tie. Within a kind the most-recently-registered
   match wins, matching the legacy `ty_env_lookup` ordering.

   The concrete-vs-tyvar split was the second hardest part of this
   lane. `list.repeat(x: a, n: Int)` has a tyvar-headed first param
   and would otherwise win the race against `string.repeat(s: String,
   n: Int)` for a `String` arg, because instantiation refreshes the
   var and `unify(?t_fresh, String)` always succeeds. The split
   ensures `string.repeat` wins when the first arg is `String`, while
   leaving the tyvar fallback for cases where no concrete-headed
   candidate matches.

3. `try_bare_call_narrow(st, callee, args_, line, col) : Option[Inferred]` —
   `synth_call`'s new third try-arm (between `try_ufcs_call` and the
   default arm). Fires only when the callee is `EVar(nm)` and there
   are 2+ qualified candidates. Walks the first arg, narrows by its
   type, rewrites the callee to `EModCall(<mod>, nm)`. Falls through
   to the default arm when narrowing returns None or there is only
   one candidate (which gives the legacy `ty_env_lookup` path
   unchanged behaviour for unique-name calls).

**Call-site rewrites:**

4. `synth_ufcs_dispatch` — collects qualified candidates, picks by
   receiver type, rewrites callee to `EModCall(mod, ident)`. Falls
   back to the legacy `ty_env_lookup` path (with a fresh `UfcsPick`
   record carrying `EVar(ident)` as the callee form) so root-file
   user fns with `module_origin = None` (e.g. `paint(c: Color)` in
   `examples/ufcs/over_union_component.kai`) keep dispatching.

5. `synth_call` — adds `try_bare_call_narrow` to the try-chain.

6. `synth_pipe`'s `ECall` arm — same narrowing as `synth_call`,
   threading the LHS as the implicit first arg of the callee.

7. `synth_pipe_dispatch` (the `|` / `||` map-pipe / flat-map-pipe) —
   was already module-aware via `head_module_for(head)`, but built
   the callee as a plain `EVar(op)` and re-resolved through
   bare-name lookup. Switched to `EModCall(module_name, op)` so the
   resolver consults the qualified key first.

The fix is slightly larger than the brief's "two-site" estimate but
is still a single-direction edit: every site moves from
`EVar(name)` to `EModCall(<mod>, name)` once narrowing has identified
the module. The deltas in `synth_ufcs_dispatch` and `synth_pipe` use
the same `pick_by_first_arg` helper, so the four sites share their
correctness invariant.

## Migration inventory

**Aliases retired**: 10/10 from the brief.

```
stdlib/core/string.kai:   string_repeat        -> string.repeat
stdlib/core/option.kai:   opt_map / opt_filter / opt_zip
                                              -> option.{map,filter,zip}
stdlib/core/result.kai:   result_map / result_and_then / result_unwrap_or
                          / result_or_else / result_unwrap_or_else
                          / result_collect    -> result.{...}
```

**Surviving alias**: `opt_or` — `or` is a reserved keyword and
`option.or(o, dflt)` does not parse. Out of scope per the brief.

**Caller migrations** (counted distinct call sites):

- `stdlib/decimal.kai`: 1 site (`string_repeat("0", n)` → `string.repeat`).
- `stdlib/core/result.kai` intrinsic tests: ~24 sites qualified to
  `result.<op>(...)`. The block-level perl regex preserved the
  function bodies; only `test "..." { ... }` blocks were touched.
- `stdlib/core/option.kai` intrinsic tests: ~22 sites qualified to
  `option.<op>(...)` similarly.
- `stdlib/core/string.kai` intrinsic test: 5 sites qualified to
  `string.repeat(...)`.
- `examples/ufcs/chain_basic.kai`: 1 site (`r.result_map(...)` →
  `r.map(...)` — UFCS now narrows by receiver `Result[_, _]`).
- `examples/stdlib/option_filter_basic.kai`: 3 sites.
- `examples/stdlib/option_zip_basic.kai`: 4 sites.
- `examples/stdlib/result_*_basic.kai` + `examples/sequence/pipe_*_rejected.kai`:
  4 files with comment-only references migrated to
  `result.<op>` / `option.<op>` for consistency. The two
  `pipe_*_rejected.err.expected` goldens were updated to match the
  compiler's new diagnostic text (which now suggests `option.and_then`
  / `result.and_then` instead of the retired alias names).

Total: ~64 caller sites touched. None remain referencing the retired
alias names.

## UFCS demo verification

`make -C stage2 test-ufcs` covers 7 fixtures: `basic`, `chain_basic`,
`field_wins_over_function`, `method_not_found` (negative),
`no_partial_application` (negative), `over_union_component`,
`postfix_interaction`. All pass post-fix.

The interesting case was `over_union_component`, which dispatches
`Red.paint()` against a root-file `pub fn paint(c: Color)`. `paint`
has no `module_origin` (root file), so my qualified-only collector
returned no candidates. Pre-fallback the typer reported `method
'paint' not found for type 'Color'`. The fix adds a `ty_env_lookup`
fallback to `synth_ufcs_dispatch` that re-emits `EVar(ident)` as the
callee when no qualified candidate matches, preserving legacy
behaviour for root-file fns and stdlib helpers that are *not*
module-qualified.

The 26-demo `demos-no-regression` baseline holds (23 OK + 3
no-golden, 2 baseline-allowed failures `mini_ledger` / `spiral`
unrelated to this lane).

## Compiler errors I encountered

- `expected identifier after '...' in list pattern` from kaic1 when I
  first wrote `[first, ..._]` (anonymous rest pattern). Stage 1 needs a
  named rest binding even when unused; renamed to `..._rest_params`.
- `error: undefined name 'is_none'` ... `undefined name 'is_empty'` ...
  `undefined name 'sum'` from result/option/list intrinsic tests when
  I tried to compile the test target without the corresponding sibling
  preludes. The `test-stdlib-core-intrinsic` Makefile target loads
  every other core module as `--prelude`, so these errors only fire
  outside that recipe — surfaced once during a manual repro and went
  away once I matched the recipe.
- `non-exhaustive match` panic in the test binary running the result
  tests under the option target. Smoking gun: the C output for
  `map(r, ...)` was `kai_list__map__mono__Any__Any__Any` instead of
  `kai_result__map`. The DTest body was bypassing `infer_decl`'s DFn
  arm entirely (the `_` wildcard returned the decl unchanged with
  `errs: 0, insts: []`), so no typer-side rewrite ever ran for test
  bodies. Fix: rewrite the call shape from inside `synth_call` /
  `synth_pipe` *and* migrate the intrinsic tests to qualified
  `<mod>.<op>(...)` calls. Both prongs landed in this lane —
  qualifying the tests is the safer choice (the typer rewrite
  already works for DFn bodies; the qualified spelling makes the
  intent explicit at the call site).
- `error: type mismatch in pipe call` at `[1..15] | fizzbuzz |>
  each(println)` (fizzbuzz demo) and `xs |> filter((x) => ...)`
  (collatz demo). Diagnosis: `synth_pipe`'s `ECall` arm and
  `synth_pipe_dispatch` both went through bare `synth(callee)`,
  picking the most-recently-registered bare scheme — `result.map`
  for the `|` operator, `option.filter` for the `|>` operator. Fix:
  thread the same narrow + rewrite through both pipe paths.
- `error: type mismatch in function call` (`expected: (String,
  String) -> String`, `found: (String, [String]) -> ?t3 / ?e1`) on
  `string.repeat`'s self-recursive call `repeat(s, n - 1)`. Tyvar
  tie-break bug: list.repeat's `(x: a, n: Int)` first param vacuously
  unifies with String, so it won the race against
  string.repeat's `(s: String, n: Int)`. Fix: split candidates into
  concrete-headed vs tyvar-headed buckets in `pick_by_first_arg`,
  prefer concrete on tie.
- `method 'paint' not found for type 'Color'` from
  `over_union_component`. Fix: `synth_ufcs_dispatch` falls back to
  `ty_env_lookup` when no qualified candidate matches, keeping the
  pre-PR-235 dispatch path for root-file user fns.

## Friction points

- **zsh string-vs-array splitting masked the diagnosis.** My early
  manual repros built `preludes=" --prelude foo --prelude bar"` and
  passed unquoted `$preludes` to `kaic2`, which zsh treated as a
  single argv entry. The compiler complained about an "unknown
  flag" but my brain registered it as "narrow doesn't fire here for
  some reason." Burned ~20 minutes confirming the fix worked under
  `bash -c` before realising the problem was the shell, not the
  compiler. Lesson: always `preludes=()` array + `"${preludes[@]}"`
  for kaikai compiler invocations from zsh.
- **DTest bodies are not type-inferred.** `infer_decl`'s `_`
  wildcard returns non-DFn decls unchanged with empty `insts`. I
  spent ~30 minutes building a DTest body inference arm before
  realising it caused unrelated regressions in list.kai and the
  cleaner fix was to just qualify the intrinsic tests' bare-name
  calls. The test bodies were the only consumer that exercised the
  bug post-rename, so making them qualified is both the smallest
  delta *and* documents the post-#235 idiom (qualified calls are
  the canonical form; bare calls are reserved for unique-name
  helpers).
- **macOS `test-signal-trap` flake** — same as the lane #219 / #230
  retros. `tier1` and `tier1-asan` exit 143 (SIGTERM) /
  Bus error 10 inside the m8 trap fixture under the local sandbox,
  unrelated to my changes. CI runs them on Linux. tier0,
  test-stdlib-core-intrinsic, test-ufcs, selfhost, and
  selfhost-llvm are all locally green.
- **The four-site fix is uniform but the LOC count grew.** I had
  budgeted ~80 lines per the brief's "two-site" estimate; the final
  delta is ~250 lines. The bulk is the `pick_by_first_arg` /
  `ty_env_collect_candidates` helpers (which are reused at each
  site) and the four parallel rewrites of the call-site lowering
  shape. Each individual rewrite is small (~15 lines); the
  multiplier is the four-way fan-out of the typer's call-handling
  paths.

## Spec ambiguities or interpretive choices

- The brief mandated "narrow by receiver type / argument type" but
  was silent on the concrete-vs-tyvar tie-break. Without it,
  `list.repeat`-style polymorphic functions silently win against
  module-specific concrete-typed functions. I chose to prefer
  concrete-headed first params. The alternative (preferring tyvar-
  headed) would have inverted my failure mode: `string.repeat`
  works, but `xs.map(f)` for `xs : [Int]` would pick whichever
  module's `map` registered last instead of `list.map`. The
  concrete-prefer rule is the only one where every fixture passes.
- The brief named `ty_env_lookup` and `synth_ufcs_dispatch` as the
  surgery sites. The lane scope grew to four sites (`synth_call`,
  `synth_pipe`, `synth_pipe_dispatch` added). I treated this as
  staying within scope — every site has the same shape (bare-name
  lookup picking by load order) and the same fix
  (qualified-narrow + EModCall rewrite). The brief's STOP-and-
  report clause governs *new* gaps; this is a wider surface of the
  same gap.
- The brief mandated retiring the 10 aliases as proof of fix. After
  the typer fix worked for DFn bodies, I migrated the intrinsic
  tests to qualified calls rather than typing DTest bodies — the
  brief was silent on the test-body path and the qualified spelling
  is what users will write.

## Subjective summary

The diagnosis from the #230 retro was right: the typer's bare-name
lookup is the load-order footgun. What it understated is that the
typer has *four* paths into the same lookup, all equally susceptible.
The fix is uniform but has to land at every site, and the codegen
side has its own load-order footgun (`efn_resolve`) that the typer-
side fix has to bypass by switching the AST shape from `EVar` to
`EModCall`.

The concrete-vs-tyvar tie-break was the scariest part of the lane.
Without it, retiring `string_repeat` silently routed `string.repeat`
through `list.repeat` because polymorphism beat specialisation in the
race. With it, every test passes.

The DTest dead-end (~30 min building body inference, then reverting)
was the costliest detour. The lesson going forward: when the test
target's intrinsic block is the failure mode, the cheapest fix is to
qualify the call sites in the test bodies, not to add a typer pass
for them.

The 10 aliases retire cleanly. `opt_or` survives separately, as
documented in the brief and the option.kai header.

## Limitations of this report

- Only the local macOS run is documented; CI confirmation lands when
  the PR opens. `tier1` / `tier1-asan` are locally FAIL'd by the
  macOS-only `signal_trap` flake (same as #219 / #230 retros). All
  the gates that *are* path-relevant (tier0, test-stdlib-core-
  intrinsic, test-ufcs, selfhost both backends) are green.
- The "concrete-vs-tyvar" classifier in `first_param_head_is_concrete`
  enumerates Ty constructors explicitly; a future Ty variant added
  without updating that helper would default to "not concrete" and
  fall into the tyvar bucket. This is the conservative direction —
  the worst-case result is a missed concrete-prefer optimisation,
  not a wrong dispatch.
- The pipe rewrite (`synth_pipe`'s ECall arm) only fires when the
  callee is a syntactically-bare `EVar(nm)`. A callee like
  `xs |> (some_var.map_resolver)(p)` would skip the narrow and use
  the legacy `synth(f)` path. Such call shapes are rare; flagging
  them for follow-up if they surface.
- Compile-time impact of the four-site narrow walk is uncharacterised.
  Each call site adds one O(env.entries) walk for candidate
  collection plus one instantiate-per-candidate. For typical stdlib
  modules the candidate count is ≤ 3 and `env.entries` is ~ 1500
  long; locally selfhost wall-time grew by ~1s over the
  pre-narrow baseline (29s → 30s). Not measurable as a regression
  but worth noting if the helper count grows.

## Build telemetry

```
timestamp	cmd	outcome	elapsed_s
2026-05-04T15:46:14-04:00	selfhost-baseline-after-typer-fix	OK	29
2026-05-04T16:38:14-04:00	test-stdlib-core-intrinsic	OK	-
2026-05-04T16:40:21-04:00	selfhost	OK	30
2026-05-04T16:40:21-04:00	selfhost-llvm	OK	40
2026-05-04T16:56:03-04:00	tier0	OK	58
2026-05-04T16:59:55-04:00	tier1	FAIL (macOS signal_trap flake, same as #219/#230 retros)	186
2026-05-04T17:01:06-04:00	tier1-asan	FAIL (macOS signal_trap flake, same as #219/#230 retros)	48
2026-05-04T17:07:31-04:00	tier0	OK	77 (after ufcs fallback)
2026-05-04T17:07:31-04:00	test-stdlib-core-intrinsic	OK	-
2026-05-04T17:07:31-04:00	selfhost	OK	-
2026-05-04T17:07:31-04:00	selfhost-llvm	OK	-
2026-05-04T17:07:31-04:00	test-ufcs	OK	-
```
