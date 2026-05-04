# Lane experience — issue #230 (test-target module-table registration)

## Objective metrics

- Branch: `issue-230-test-target-module-table`
- Base commit: `33a90f1` (Merge PR #229 — typer EModCall qualifier filter)
- Lane open: 2026-05-04T10:57:11-04:00
- Lane close: 2026-05-04T11:46:05-04:00 (writeup)
- Files changed: `stage2/compiler.kai`, `stdlib/core/option.kai` (regression
  fixture only).
- Compiler delta: ~80 net additions in `stage2/compiler.kai` (target ME
  registration in `compile_source` + `tag_target_decls_module_origin` /
  `tag_target_decl` helpers).
- Aliases retired: 0 (see "Friction points" — STOP and report applied).
- Selfhost: byte-identical on both backends (`selfhost` and
  `selfhost-llvm` green).
- `test-stdlib-core-intrinsic`: 6/6 modules green (option, result, string,
  char, tuple, list).

## Diagnosis

Trivially mirrorable. The `--prelude` registration path (`load_prelude` →
`PL([Decl], ModuleEntry)` → `compile_source` → `module_table`) had two
inputs: the basename-as-mod-name and the public exports. The target file
in `compile_source` already had `path` (basename derivable via
`prelude_module_name`) and `expanded_decls` (exports derivable via
`collect_pub_exports`). Producing a target `ModuleEntry` and appending
it to `module_table` was a six-line edit.

The subtle part was the DFn tagging. Issue #229 made `add_decls_loop`
register a qualified `<mod>::<fname>` key only for DFns whose
`module_origin` field is `Some(_)`. Without tagging the target's DFns,
the typer's `EModCall` lookup for self-qualified calls misses the
qualified key and falls back to bare — which works *only* when the
target's bare entries shadow same-named preludes via `add_decls_loop`'s
prepend-newest-wins discipline. That discipline holds inside
`test-stdlib-core-intrinsic` (where the target is one of the core
modules and its decls are the last to register), but it does not hold
in general: when the target lives among many preludes that re-export
common names, the typer chooses the wrong scheme by alphabetical/load
order.

So the fix tags the target's `pub` DFns too — with three carve-outs:

1. **Only `pub` DFns get tagged**. Non-pub helpers in the target stay
   `module_origin = None`, so their C symbol stays bare
   (`kai_<name>`). Test gates that grep for literal `kai_<helper>`
   symbols (`test-m4c`'s `kai_run_with__mono__Int__Int`,
   `kai_outer__mono__Int`, etc.) keep working without per-test
   bookkeeping. Non-pub helpers are also unreachable via a qualified
   `module.helper(...)` from outside the file, so the qualified-key
   registration `add_decls_loop` would do for them is dead weight.

2. **`main` stays untagged** even when `pub`. Tagging would mint
   `kai_<basename>__main`, but `emit_main_wrapper` plants a literal
   `kai_main()` call in the C harness and the LLVM emitter has a
   hard-coded `if nm == "main"` special case (`@kai_main`). Both
   paths assume the entry symbol is bare.

3. **DFns that already carry `Some(_)` stay untagged**. `expand_imports`
   includes `import foo` decls in `expanded_decls`, already tagged with
   `Some("foo")`. Re-tagging with the target's basename would rename
   `kai_foo__bar` to `kai_<target>__bar` and break the qualified call
   from the user's `foo.bar()` site (which still emits `kai_foo__bar`
   from `EModCall("foo", "bar")`). The `mq-basic` fixture caught this
   on the first build.

## Fix shape

Smallest delta. One site touched in `compile_source`, one new helper
pair (`tag_target_decls_module_origin` / `tag_target_decl`).

```
let target_mod_name = prelude_module_name(path)
let target_exports = collect_pub_exports(expanded_decls_raw, [])
let target_me = ME(target_mod_name, target_exports, path)
let expanded_decls = tag_target_decls_module_origin(
                       expanded_decls_raw, target_mod_name)
...
let module_table = list_append(
  list_append(prelude_mods, match expanded { EP_Mod(_, mt) -> mt }),
  [target_me])
```

The helper restricts tagging to `pub` non-`main` DFns and skips
already-tagged DFns; everything else passes through unchanged. No
struct changes, no signature churn, no Makefile edits.

## Migration inventory

**Aliases retired**: 0 / 10. The brief recommended retiring all 10 in
the same diff *as proof of fix*. I attempted a full pass, found a third
blocker (see "Friction points" — bare-name receiver-driven dispatch),
and reverted to the conservative shape: typer fix + regression fixture.

**Surviving aliases (still in the stdlib, intentionally)**: all 10 from
the brief — `string_repeat`, `opt_map`, `opt_filter`, `opt_zip`,
`result_map`, `result_and_then`, `result_unwrap_or`, `result_or_else`,
`result_unwrap_or_else`, `result_collect`. Plus `opt_or` (separate
keyword reservation concern, also not retired).

**Why retirement could not land in this lane**:

- `opt_map` → `option.map` requires `option.kai` to declare
  `pub fn map`. Then both `option.map` and `list.map` co-exist as bare
  exports across the prelude chain. `add_decls_loop` registers them
  bare in `merged_raw` order; the typer's `ty_env_lookup` returns the
  most-recently-prepended scheme.
- 30+ external demo / example callers spell `xs.map(f)` (UFCS) or bare
  `map(xs, f)` against `[a]` receivers. UFCS dispatch
  (`synth_ufcs_dispatch`, `compiler.kai:23949`) does
  `ty_env_lookup(env, ident)` *without* receiver-type filtering, so it
  picks the most-recently-registered `map` and unifies with `[a]` only
  by accident. Post-rename, `option.map` (or `result.map`, depending on
  prelude order) shadows `list.map` in bare scope and `xs.map(f)`
  fails to unify with `Option[?]` first arg.
- Same shape for `filter`, `zip`, and `repeat`. `and_then`, `unwrap_or`,
  `or_else`, `unwrap_or_else`, `collect` colliding *inside* the stdlib
  (option vs result) is workable — every external caller already
  qualifies — but the typer's bare lookup path inside option.kai's /
  result.kai's intrinsic test blocks would still pick the wrong scheme.

The ergonomic fix is the same as #219's friction note: extend UFCS (and
the typer's bare lookup path that has receiver type information
available) to filter candidates by receiver-arg type, like
`efn_resolve` already does at codegen. That is independent compiler
work and out of scope for #230's "register the target in the module
table" delta.

**Recommendation**: open a follow-up issue scoped to
"receiver-type-driven dispatch in bare-name lookup (UFCS + first-arg
unify guard)". The 10 aliases retire mechanically once that lands.
With #230 in place, the qualified-call path is solid; only the bare
path remains as the lookup-collision footgun.

## Compiler errors I encountered

- `error: undefined name 'option'` at `option.is_some(option.map(...))`
  inside option.kai's intrinsic test block, pre-fix. Captured by the new
  regression fixture (`test "issue #230 — qualified self-reference
  resolves"`). Disappears with the target ME registered.
- `error: type mismatch in function call` at list.kai's bare `map(t, f)`
  recursion when option.kai was the test target — option's freshly-
  renamed `pub fn map` shadowed `list.map` in bare scope and the typer
  unified `[a]` against `Option[?]`. This was during the abandoned
  alias retirement.
- `error: undeclared function kai_arith__double` from the `mq-basic`
  fixture during my first tagging pass — I had tagged every DFn in
  `expanded_decls`, including those imported from `arith.kai` which
  arrived with `Some("arith")` already. The carve-out for already-
  tagged DFns fixed it.
- `kai_main()` undeclared in `build/kaic2b.c` selfhost — the entry
  point got renamed to `kai_compiler__main` because I tagged every
  DFn including `main`. The skip-`main` carve-out fixed it.
- `m4c FAIL m4c_run_with — specialisations missing` from the
  `test-m4c` gate on CI Linux. The grep was looking for
  `kai_run_with__mono__Int__Int` literal, but my second tagging pass
  prefixed it to `kai_m4c_run_with__run_with__mono__Int__Int`. The
  pub-only carve-out fixed it: `run_with` is `fn` (not `pub fn`), so
  it stays bare and the gate's literal grep keeps matching. Surfaced
  in CI rather than locally because tier1 is path-gated and macOS
  signal_trap noise hid the m4c FAIL line on the local terminal.

## Friction points

- **Bare-name receiver gap (third blocker)**. The brief warned that
  retirement might reveal a new gap; it did. PR #218 closed prelude
  scope, PR #229 closed the qualified-call path, PR #230 closes the
  test-target gap, but the bare-name path still resolves by load order
  rather than receiver type. The retirement strategy reaches its
  logical end here: every same-named export across `option`/`result`/
  `list` collides in bare scope, and 30+ existing demos use bare
  `xs.map(f)` (UFCS). Migrating them is a separate, larger lane.
- **`mq-basic` regression caught the over-tagging bug**. The first
  iteration tagged every DFn in `expanded_decls` (including imported
  ones). `make tier1` surfaced it at the link-time `undeclared
  function kai_arith__double` — exactly the diagnostic shape the
  qualified-call codepath emits when symbol minting and call-site
  encoding diverge. The carve-out for already-tagged DFns ships in
  this lane.
- **`test-stdlib-core-intrinsic`** is now self-consistent: every core
  module's intrinsic tests can mix bare same-module references and
  qualified other-module references freely. The new fixture asserts
  `option.is_some(option.opt_map(...))` end-to-end inside option.kai
  itself.
- **macOS test-effects flakiness**. Same as the lane #219 retro:
  `test-signal-trap` (issue #107 fixture) returns SIGTERM (exit 143) /
  Bus error 10 under the local sandbox, breaking `make tier1` and
  `make tier1-asan`. Unrelated to my changes; CI runs them on Linux.

## Spec ambiguities or interpretive choices

- The brief mandated retirement *as proof of fix*. The `STOP and report`
  clause governs when retirement reveals a new resolver gap, which
  applied here. I shipped the test-target fix + regression fixture and
  documented the third blocker, mirroring lane #219's resolution shape.
- The brief listed `opt_or` as a separate concern (keyword reservation)
  and the 10 retirable aliases as in-scope for #230. I confirmed the
  bare-name receiver gap blocks every one of those 10, so the lane
  holds 11 surviving aliases (10 + `opt_or`) total.
- The fix shape "target file's basename → module key in the qualified-
  call module table" admits two interpretations: register only the ME
  (no DFn tagging) or register both. I picked both — only the second
  resolves through the qualified `<mod>::<fname>` typer key (#229's
  contract), which makes the fix robust to prelude-order changes.
  Without tagging, self-qualified calls would resolve correctly only
  by accident of the bare-prepend ordering.

## Subjective summary

The diagnosis was straight-line: the prelude registration was already
named `load_prelude` and produced exactly the data structure
`compile_source` needed. The target had the same data — just no code
that constructed it. Six-line edit plus a tagging carve-out.

The friction was the same flavour as #219: the brief assumed the typer
fix would unblock alias retirement, the typer fix did unblock the
qualified path, but the bare path is still a name-only `ty_env_lookup`
with receiver-driven dispatch absent. The 10 aliases survive a third
PR. The strategy is correct; the surface area is bigger than any one
lane should swallow.

The regression fixture
(`test "issue #230 — qualified self-reference resolves"` inside
`stdlib/core/option.kai`) is the cleanest way to capture the bug:
calling `option.is_some(option.opt_map(...))` from inside option.kai
itself. Pre-fix, kaic2 prints `error: undefined name 'option'`. Post-
fix, the test passes through `test-stdlib-core-intrinsic` along with
the other 35 option tests.

## Limitations of this report

- Only the local macOS run is documented; CI confirmation lands when
  the PR opens. The `tier1` / `tier1-asan` lines in the build TSV are
  locally FAIL'd by the macOS-only `signal_trap` flake (same as the
  #219 retro). Selfhost / selfhost-llvm / test-stdlib-core-intrinsic /
  tier0 are all locally green.
- I did not measure compile-time impact of the extra ME registration +
  per-decl tagging walk on the target. The walk is O(|target_decls|),
  which is dwarfed by the existing prelude-decl walks; no measurable
  signal expected.
- The `tag_target_decl` carve-out for already-tagged DFns matches
  `tag_decl_module_origin`'s structure mechanically; I did not add a
  test that exercises both paths in isolation. The integration test
  (`test-modules-qualified` covering `mq-basic`, `mq-two_modules`, and
  the `import`-bringing fixtures) gates the carve-out's correctness.
- Retirement strategy: the bare-name receiver gap is *the* outstanding
  resolver issue blocking flat-alias cleanup. Until it lands, every
  cleanup lane in this area will hit the same wall.

## Build telemetry

```
timestamp	cmd	outcome	elapsed_s
2026-05-04T11:01:17-04:00	test-stdlib-core-intrinsic-baseline	OK	-
2026-05-04T11:35:36-04:00	tier0	OK	51
2026-05-04T11:43:15-04:00	tier1	FAIL (macOS signal_trap flake; same as lane #219)	184
2026-05-04T11:45:37-04:00	tier1-asan	FAIL (macOS signal_trap flake)	44
2026-05-04T11:45:37-04:00	test-stdlib-core-intrinsic	OK	-
2026-05-04T11:45:37-04:00	selfhost	OK	24
2026-05-04T11:45:37-04:00	selfhost-llvm	OK	31
```
