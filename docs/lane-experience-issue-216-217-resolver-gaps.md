# Lane experience — issues #216 + #217 (resolver gaps)

Combined fix for two stage-2 qualified-call resolver gaps that PR
#215 surfaced and PRs 2–6 of m14 (#203) need closed before they can
remove flat aliases.

## Objective metrics

- Lane start: `2026-05-03T23:13:33-04:00`
- Lane end:   `2026-05-03T23:44:58-04:00`
- Wall-clock: ~31 minutes
- Build/test invocations: see appended TSV (1× tier0, 4× tier1, 2×
  tier1-asan, 2× selfhost, 2× selfhost-llvm — the second tier1 and
  selfhost runs were verification after iterating on a regression
  the first version of the fix introduced).

## Diagnosis

**Two independent bypasses, one root cause.** The qualified-call
resolver `rqc_decls` (stage2/compiler.kai:35889) walks every body
under its callsite and rewrites `EField(EVar(mod), fname)` into
`EModCall(mod, fname)`. It already walks scope-introducing shapes
correctly, so within a single parse tree it covers every reachable
expression. The two gaps are not about *what it walks* but about
*which parse trees the pass is fed*:

1. **#216 (prelude scope)** — `compile_source` only ever called
   `rqc_decls` on the main file's `expanded_decls`. The prelude
   decls returned from `load_prelude` were `list_append`ed into
   `merged_raw` raw, never visited by the rewriter. Any qualified
   call written inside a `--prelude` file therefore arrived at the
   typer as a literal `EField(EVar("list"), "nth")` and was
   rejected as "undefined name `list`".

2. **#217 (interpolation scope)** — `desugar_interp_decls`
   (stage2/compiler.kai:18212) lowers each `#{…}` segment by
   re-tokenising and re-parsing the inner expression source span
   (`interp_part_to_expr`, line 18285). The freshly-parsed sub-AST
   has not been seen by `rqc_decls` (which already ran earlier in
   the pipeline on the surrounding file). So a qualified call inside
   `#{…}` skips the rewrite for the same reason as #216 — its parse
   tree was minted after rqc had already run.

The suspected location in #216 (around `rename_proto_calls_kind` and
"the prefix-fallback hook") was off — `rename_proto_calls_*` is the
protocol op resolver, not the module-qualified call resolver.
`rqc_decls` is the right target. The "Suspected location" hint was
useful as a near-pointer but the actual fix sat one helper over.

## Fix

Two-line surface in `compile_source` plus a thread-through of the
module table into the interp desugar.

- `stage2/compiler.kai:41217` — add `rqc_decls(prelude_decls, mt, path)`
  before the merge so prelude bodies go through the same rewrite the
  main file does.
- `stage2/compiler.kai:18212–18305` — `desugar_interp_decls` now
  takes `mt: [ModuleEntry]` and `file: String` and threads them down
  to `interp_part_to_expr`, which calls `rqc_expr` on the freshly-
  parsed inner expression *before* recursing into the desugar (so
  qualified calls survive desugar and protocol ops still get their
  `__proto_<op>` route).
- `stage2/compiler.kai:41339–41341` — feed `module_table` and `path`
  into the new `desugar_interp_decls` arity.

The "single root cause vs two fixes" framing in the lane brief
turned out closer to "shared mechanism, two distinct call sites":
both gaps are about delivering the right parse tree to `rqc_decls`,
but the prelude path needed an extra call and the interp path
needed parameter threading. They could not collapse into a single
delta without restructuring the pipeline.

### Approach rejected mid-lane

First attempt: move `rqc_decls` to *after* `desugar_interp_decls`
and run it once on the merged stream. That elegantly closes both
gaps in one call, but `desugar_interp_decls` itself walks
`ECall(EField(receiver, op_name), args)` and rewrites it to
`__proto_op(receiver, ...args)` whenever `op_name` is a declared
protocol op (e.g. `max`, `min`). Calls like `list.max([])` then
become `__proto_max(list, [])` — the EField shape is gone before
rqc gets a chance, and the typer rejects the bare `list` EVar.
`examples/stdlib/list_extrema.kai` (already in the repo) caught
this in tier1, which is why I kept the early rqc call and threaded
mt+file into interp desugar instead.

## Compiler errors I encountered

- `examples/stdlib/list_extrema.kai` failed under approach A
  (above) because `list.max` has the same op_name as `Ord.max`. The
  protocol-op rewrite inside interp desugar fires regardless of
  context, so deferring rqc lost the module-qualified semantics.
  Lesson: the protocol-op rewrite in `desugar_interp_kind` and the
  module-qualified rewrite in `rqc_kind` operate on the same
  `EField(EVar(name), member)` shape with overlapping member names;
  ordering matters and is currently rqc-first.
- `list.first` is not exported by `stdlib/core/list.kai` — picked it
  on the first draft of the prelude fixture by guessing. The
  improved diagnostic (see below) caught it: "module 'list' does
  not export 'first'; available exports: …".

## Friction points

- The hint about resolver pass location in both issues pointed at
  `rename_proto_calls_kind`, which is the protocol op resolver. The
  module-qualified resolver lives in `rqc_*` and has no string
  match for "qualified" in its function name, only in comments.
  Took a couple of greps to land on `rqc_decls`.
- Diagnostics improved naturally with the fix. With prelude scope
  closed, a missing-export call now triggers the existing
  `rqc_diag_missing_export` path, which prints a precise message
  with the available export list. The misleading "did you mean
  `xs`?" suggestion the issues called out is still produced as a
  *secondary* error from the typer (because the EField fallback
  still runs), but the primary error is now correct and arrives
  first. Filing a follow-up issue for the secondary cascade did not
  feel high-value: now that the primary diagnostic is right, the
  second one is harmless context.
- The `*_lib.kai` skip pattern in `test-stdlib` is new in this
  lane. Existing convention used `bench_*|check_*` prefixes; I
  added `*_lib|qualified_call_in_prelude` to keep helper files and
  the multi-file fixture from being run as standalones by the
  glob-iterating target.

## Spec ambiguities or interpretive choices

- **Surviving aliases**: deferred the actual re-migration of
  `list_is_empty`, `list_nth`, `list_take`, `list_contains`,
  `list_sum` (stdlib/core/list.kai) and the prelude reverts in
  PR #215 to a follow-up PR (m14 phase 2). The new fixtures prove
  the resolver gaps are closed; the alias removal is m14's job.
- **Diagnostic improvements**: did not file a follow-up. The
  primary diagnostic became correct as a side effect of the fix;
  the residual secondary "did you mean …?" cascade is harmless.

## Subjective summary

- Confidence: high. Both repros pass; tier0, tier1, tier1-asan,
  selfhost, selfhost-llvm all green. Stage1→stage2 compiles
  byte-identical fixed point.
- Hardest: spotting the protocol-op-vs-module-export name overlap.
  Approach A (single late rqc call) looked elegant at first and
  the test surface only catches it through one fixture
  (`list_extrema.kai`). Without that fixture I might have shipped
  a regression masquerading as a refactor.
- Easiest: the prelude-scope fix itself — one extra call to an
  existing helper.
- Compiler help: the "did you mean …?" near-miss cascade is
  noisy, but the primary `rqc_diag_missing_export` path is a
  beautiful diagnostic — full export list inline made fixing my
  draft fixture a five-second turnaround.

## Limitations of this report

- Wall-clock includes one full backtrack from approach A to
  approach B; a fresh attempt landing on approach B from the start
  would be ~15 minutes.
- Did not measure RC budget or runtime perf — the change is a
  compile-time pass thread-through with no runtime shape change,
  so no perf regression is plausible. Selfhost byte-identity
  confirms the compiler emits the same C as it would have if the
  fix had been a no-op on already-resolved inputs.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-03T23:17:30-04:00	tier0	OK	36
2026-05-03T23:20:27-04:00	tier1	OK	134
2026-05-03T23:28:53-04:00	tier1	OK	262
2026-05-03T23:29:50-04:00	tier1-asan	OK	51
2026-05-03T23:30:18-04:00	selfhost	OK	19
2026-05-03T23:30:23-04:00	selfhost-llvm	OK	0
2026-05-03T23:30:59-04:00	selfhost-llvm	OK	-
2026-05-03T23:37:20-04:00	tier1	OK	226
2026-05-03T23:44:21-04:00	tier1+asan+selfhost	OK	337
```
