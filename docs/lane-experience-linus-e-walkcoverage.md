# Lane experience — linus-e-walkcoverage

Lane E (last) of Linus's km quality audit of stage 2 (the audit plan doc is not
checked into this worktree; lanes A/B/D already landed). Single-file perf fix in
`stage2/compiler/infer.kai`.

## Scope as planned vs as shipped

**Planned.** The effect-coverage walk (`walk_coverage_expr` + ~7 helpers,
`infer.kai` ~2241-2466) threaded `decls: [Decl]` through the whole recursion for
one reason: to call `effect_default_op_names(eff_name, decls)` once per `EHandle`
node. That helper re-scans all of `decls` (O(n_decls)) to locate the effect and
return its extern-handler-bridged default op names. Repeated per `EHandle` ⇒
O(N_EHandle × n_decls). The brief: precompute one `eff_name -> [op_names]` map by
walking `decls` once, thread the map, drop the per-`EHandle` rescan.

**Shipped.** Exactly that, with one deliberate refinement the brief's literal
wording ("replace the `decls` parameter with the map") did not anticipate — see
below. The map is built once at the coverage entry point and threaded through all
8 walk functions; both hot `effect_default_op_names` call sites now do a single
assoc-list lookup.

## The precomputed map

Structure chosen — `type EffDefaultOps = EDOps(String, [String])` plus a builder
`build_effect_default_op_map(decls) : [EffDefaultOps]` and a lookup
`effect_default_op_map_get(table, eff_name) : [String]`.

Why this shape: it is a verbatim copy of the `EffOps`/`use_eff_lookup` assoc-list
idiom already in `desugar.kai:2665` (`type EffOps = EffOps(String, [String])`),
the canonical "effect name → list of op names" table in the codebase. The brief
asked to copy an existing pattern rather than invent one, and to stay inside
`infer.kai` — so the type/builder/lookup are local clones of that idiom rather
than an import (importing `desugar.EffOps` would have crossed module boundaries,
which the lane scope forbids).

Builder reuses a new tiny helper `default_block_extern_ops(Option[DefaultBlock])`
that both the map builder and the now-vestigial `effect_default_op_names` share,
so the "extract extern-handler op names from a default block" logic lives in one
place. Each table entry holds **exactly** what `effect_default_op_names` returned
for that effect: the extern-handler-bridged op names of its `default { }` block,
or `[]` when the effect has no such block. The builder walks `decls` once,
emitting one entry per `DEffect` (skipping non-effect decls).

## Behavior-identity discipline (the load-bearing constraint)

The map had to produce byte-identical coverage diagnostics. Two correctness
hinges:

1. **Absent-effect case.** `effect_default_op_names` on an undeclared effect
   returns `[]` (because `inf_find_effect_default_block` finds no `DEffect`).
   `effect_default_op_map_get` returns `[]` when the lookup falls off the end of
   the table — same observable. So a missing entry ≡ the old "effect not found"
   path. Verified empirically: `examples/negative/effect_not_handled.kai` (an
   effect with no `default { }`) still emits the identical
   `error[E_EFFECT_NOT_HANDLED]: effect not handled: Logger.warn` with the same
   span, note, and help text.

2. **`decls` is NOT fully removed.** The brief said "replace the `decls`
   parameter with the map", but `check_op_call_coverage` consumes `decls` for two
   *cold* (error-path) callees that the map cannot serve: `is_effect_declared`
   (needs the full decl list) and `report_uncovered_op` → `describe_default_status`
   (needs the actual `DefaultBlock` to render the nuanced help text). Those are
   diagnostic paths that fire at most once per uncovered op — not the hot rescan.
   So the lane **adds** `eff_map` alongside `decls` and replaces only the two hot
   `effect_default_op_names` calls. Removing `decls` would have meant rebuilding
   the cold-path lookups against the map (a richer map, more surface, more risk)
   for zero hot-path gain. The objective of the lane — kill the
   O(N × n_decls) hot rescan — is fully met without it. This is the one place the
   literal brief and the correct minimal change diverged; I took the correct
   minimal change.

`effect_default_op_names` itself is now unreferenced in the walk but left intact
(it is private, harmless, and its comment documents the extern-bridge rule). It
could be deleted, but doing so is out of the lane's perf scope and would touch
behavior-irrelevant surface; left for a future cleanup if desired.

## Complexity before/after

- Before: per `EHandle` node, `effect_default_op_names` scans all of `decls`
  (O(n_decls)) plus the matched effect's default block. Over a program with N
  `EHandle` nodes: **O(N × n_decls)**.
- After: one `build_effect_default_op_map` pass over `decls` (**O(n_decls)**, plus
  the default-block extraction it would have done anyway), then each `EHandle` does
  one assoc-list lookup over the compact effect table (**O(n_eff)**, n_eff ≤
  n_decls and typically n_eff ≪ n_decls). Total: **O(n_decls + N × n_eff)** —
  i.e. the linear-build + cheap-lookup shape the audit asked for, replacing the
  quadratic product.

The map is built once per `check_default_coverage` call (per module) and shared
across every per-`DFn` body walk in that module.

## Fixtures that cover it

No new fixture added — the existing set already exercises both map branches and
the lookup-miss path, and the audit guidance says not to add redundant fixtures.
The relevant pre-existing fixtures (all run by `make test-negative`, all PASS
with byte-identical diagnostics under the new compiler):

- `examples/negative/effects_phase2/partial_handle_no_default.kai` — partial
  handle of an effect whose `default { }` does not cover the op → map entry is
  `[]` / not-found → the uncovered-op diagnostic fires. The **empty/absent**
  branch.
- `examples/negative/effects_phase2/partial_handle_extern_default_masked.kai` —
  effect **with** an extern-bridged `default { }` whose op is masked by a partial
  inner handle → map entry is a **non-empty** op-name list. The non-empty branch.
- `examples/negative/effects_phase2/partial_handle_kaikai_default.kai`,
  `handle_with_undeclared_effect.kai`, `args_helper_no_row.kai`,
  `exit_helper_no_row.kai` — further coverage-walk shapes.
- `examples/negative/handle_leak/*` (8 fixtures), `examples/negative/pub_effect/*`
  — leak / pub-row enforcement that routes through the same walk.
- `examples/effects/{default_block_full_user_handle, extern_handler_user_effect,
  issue_558_user_effect_default_main_install}.kai` — positive fixtures with
  extern-bridged default blocks (non-empty map entries), run by `make test`.

Coverage gap: none new. The two map branches (empty list, non-empty list) and
the lookup-miss path are all exercised by the above.

## Gates

- `make selfhost` — determinism OK (`kaic2b.c == kaic2c.c`), exit 0. The change
  is internal; emitted C is deterministic and self-hosts. ✅
- `make tier0` — selfhost + demos baseline 34, exit 0. ✅
- `make -C stage2 test-llvm` — 5/5 C↔LLVM fixtures OK, exit 0. ✅
- **Effect-coverage central gate (run in isolation):** ✅
  - `make test-negative` — **105 PASS, 0 FAIL, 0 MISS**, exit 0. Every
    effect-coverage negative fixture (the `effects_phase2/*`, `handle_leak/*`,
    `pub_effect/*` listed above) passes with byte-identical diagnostics.
  - `make test-diagnostics-collected` — 6 OK incl. `diags_t5_missing_effect`,
    exit 0.

- `make tier1` (full) — **RED, for a reason unrelated to this lane.** `make test`
  aborts at `test-issue-318-include` with a `Segmentation fault: 11` (Error 139).
  That test compiles `examples/stdlib/issue_318_kai_test_prelude_scope.kai` with
  `--test --include-prelude-tests` and **runs the resulting binary**, which
  segfaults — a runtime crash that does not touch the coverage walk.

  Verified pre-existing: `git stash`ed this lane's diff, rebuilt `kaic2` from the
  clean base HEAD (`ce5e43b`), ran `make -C stage2 test-issue-318-include` — it
  segfaults **identically** (Error 139) with no change applied. So the failure is
  not caused by this lane. (The brief named the base as `90d29bf`, but the
  worktree's actual base advanced to `ce5e43b`; the segfault is present there.)
  Filed as a GitHub issue with the repro. The `Bus error: 10` earlier in the log
  is the expected output of `m8_fiber_stack_overflow`, which deliberately
  overflows the guard page and reports `OK ... exit=138` — not a failure.

  Because `test-issue-318-include` aborts `make test` before it reaches
  `test-negative` / `test-diagnostics-collected`, those were run standalone
  (above) to confirm the lane's actual target is green.

**Note on `make selfhost-llvm`:** the brief named a `selfhost-llvm` gate, but no
such target exists in this tree's `Makefile` (root) or `stage2/Makefile`. The
real LLVM-backend verification is `test-llvm` (C↔LLVM output parity per fixture)
/ `tier1-backend-parity`; I ran `test-llvm` as the honest equivalent. The
coverage walk emits no IR — it is a typer diagnostic pass — so backend parity is
about the rest of the compiler still emitting identical LLVM, which `test-llvm`
confirms.

**Note on the pre-existing `issue_318_include` segfault:** the per-user decision
on this lane was to open the PR with the failure documented and file an issue for
the segfault (rather than block on an unrelated runtime crash). The PR body cites
the isolated-gate evidence and the issue number.

## Structural surprises

- The brief framed `decls` as serving a single purpose in the walk. It serves
  three: the hot `effect_default_op_names` (replaced) and two cold error-path
  consumers (`is_effect_declared`, `report_uncovered_op`) that genuinely need the
  raw decl list. The "replace decls entirely" framing would have over-reached.
- `effect_default_op_names` was called in **two** sites, not one: the `EHandle`
  arm (the obvious per-node rescan) and the `fname == "main"` branch of
  `check_op_call_coverage` (op escaping to main with no handler). Both are now
  map lookups. The second is lower-frequency but rides the same threaded map for
  free.

## Cost vs estimate

No time estimate was set (project convention: gate by soundness, not hours).
Real cost: one type + two helpers + one shared extractor, plus threading
`eff_map` through 8 signatures and their call sites. Two self-inflicted slips,
both caught before commit:

1. Changed the call sites before the function headers, so the first
   `make selfhost` reported `undefined name eff_map`; fixed by adding the param
   to the 8 headers.
2. When extracting `default_block_extern_ops`, my edit's `old_string` didn't span
   the original `effect_default_op_names` definition, so the refactored copy was
   *added* below rather than *replacing* it — two definitions of the same
   function. selfhost passed anyway (the two were semantically identical, so
   kaikai accepted the redefinition), which is exactly why a green build is not
   proof of a clean diff. Caught by re-grepping call sites in the source before
   committing; removed both and kept only `default_block_extern_ops`. Re-ran the
   gates after the dedup.

Lesson: after a multi-edit refactor, grep the final source for the symbols you
think you replaced — a passing selfhost can hide a duplicated definition.

## Follow-ups for next lanes

- Pre-existing `issue_318_include` runtime segfault (filed this lane) — a
  `--test --include-prelude-tests` binary crash, unrelated to the typer; needs a
  dedicated debugging lane.
- Optional: delete the now-unreferenced-in-walk `effect_default_op_names` if a
  future cleanup lane wants the surface trimmed (it is still cited in comments as
  the behavioral spec the map matches; keep the comment if the fn goes).
- The cold-path `is_effect_declared` / `describe_default_status` still do
  O(n_decls) scans, but they are per-error not per-node, so out of this lane's
  perf scope. If a future lane wants them map-backed, the table would need to
  carry the `DefaultBlock` (or a richer record) rather than just op names.
