# Lane experience — m14 v1 (stdlib qualified-call surface)

Date: 2026-04-28. Branch: `m14-v1`. Four commits:

  - `m14 v0: register --prelude files as modules` (infra prerequisite)
  - `m14 v1: qualified-call surface for stdlib (prefix-fallback)`
  - `m14 v1.x: fix emit_ident_value local-shadow-global codegen bug`
  - `m14 v1.A: rename list_* defs to bare names + legacy aliases`

The original lane was scoped as a "mechanical naming migration" —
rename `pub fn list_take` -> `pub fn take` across `stdlib/core/*`
plus update every caller. Two findings reshaped the work and one
of them blocked the rename outright.

## What the lane was meant to be

Per `docs/stage2-design.md` §m14 (pre-revision) and the lane
brief: with m6.2 v1 (qualified calls) landed and the `core/*` file
split landed, the only remaining work was a sed pass over `list_*`
-> `list.*`, `string_*` -> `string.*`, etc. Estimated 1–3 days,
mechanical, contained.

## Finding 1 — preludes were not modules (resolved)

The first probe (`fn main() { let xs = [1,2,3]; if list.is_empty(xs) ... }`)
returned `error: undefined name 'list'` even after the file split
landed and m6.2 v1 was supposedly available.

Root cause: `load_prelude` (`stage2/compiler.kai:26307`) parsed
`--prelude` files and returned their `[Decl]` for concat — it
never registered a `ModuleEntry`. m6.2's `expand_imports` only
registers a module per explicit `DImport`, and the prelude files
loaded by `bin/kai` are passed via `--prelude`, not `import`.
With no module table entry, the rqc rewrite (`rqc_kind`,
`stage2/compiler.kai:23489`) had nothing to dispatch through, so
`list.is_empty` fell through to record projection on a value
named `list`, which doesn't exist — hence the diagnostic.

Fix (commit `m14 v0`): thread a `ModuleEntry` per prelude file
from the loader into `compile_source`'s module table.

  - `load_prelude` now returns `PreludeLoaded(decls, ModuleEntry)`
    with `name = basename(path)` minus `.kai` and
    `exports = collect_pub_exports(decls)`.
  - `load_preludes` now returns `PreludesLoaded(decls, [ModuleEntry])`
    accumulated in argument order.
  - `compile_source` prepends the prelude module list to the
    table built by `expand_imports` before `rqc_decls` runs.

Selfhost C + LLVM byte-identical fixed point: OK. All test
suites green (incl. `modules-qualified` positive + negative
fixtures).

## Finding 2 — `emit_ident_value` shadows local bindings (deferred)

After v0 enabled qualified-call dispatch, the rename of
definitions itself triggered a regression in `decimal_basic`
(test-stdlib). The probe narrowed it to:

```kai
fn main() {
  let drop = 5
  print(int_to_string(drop))
}
```

  - With `pub fn drop` in the prelude (post-rename): prints garbage
    (e.g. `4309912560` — typically a closure-table pointer).
  - Without `pub fn drop` in the prelude (pre-rename): prints `5`.

Cross-checking against main HEAD with a `let list_drop = 5`
shadow reproduces the same garbage — the bug is **pre-existing**
and was simply latent because no test fixture historically used
`list_*` / `opt_*` / `ch_*` as a local-variable name. The rename
exposes it because common bare names (`take`, `drop`, `head`,
`tail`, `count`, `sort`, `repeat`, …) collide heavily with
user-code locals and parameters.

Root cause: `emit_ident_value` (`stage2/compiler.kai:7412`,
mirrored in stage 1) emits `kai_closure(&_kai_<name>_thunk, ...)`
for any `EVar(name)` whose `name` matches a top-level fn,
regardless of whether the surrounding lexical scope rebinds it.
The same path runs for both call-target and value-position
`EVar`, so a local `drop` reads as a function reference instead
of the `Int` the user bound.

The proper fix threads scope info into `emit_ident_value` (or
disambiguates at AST level — a resolver pass that rewrites local
`EVar(name)` to a distinct `ELocal(name)` variant the codegen
treats separately). Out of scope for this lane.

## What v1 actually shipped

Instead of renaming the definitions, the v1 commit adds a
**prefix-fallback** to the qualified-call resolver
(`me_lookup_export` in `stage2/compiler.kai:23413`):

  1. If `fname` is exported verbatim, use it.
  2. Else build `<prefix>_<fname>` and try that. The prefix is
     `module_legacy_prefix(mod_name) ?? mod_name`, with two
     overrides: `option -> opt`, `char -> ch`.
  3. Else `None` -> the M6.2 v1 missing-export diagnostic.

`EModCall` carries the **resolved** export name (the underlying
`list_take`), so codegen mints the existing C symbol — no rename
required. User code reads `list.take(xs, n)`; under the hood
this calls `kai_list_take`. The legacy bare-name surface
(`list_take(xs, n)`) keeps working unchanged.

End state:

  - User-facing qualified surface complete: `list.*`, `string.*`,
    `option.*`, `result.*`, `char.*`, `io.*` (just `println`),
    `tuple.*` (no fns yet) all reachable.
  - Internal stdlib defs unchanged — no shadow exposure.
  - Selfhost C + LLVM byte-identical fixed point: OK.
  - Full test suite green.

## Finding 3 — codegen shadow fix landed (resolved)

The shadow bug was fixed by mirroring the LLVM backend's
`e.locals` design in the C backend: a `lcs: [String]` parameter
threads lexical scope through every `emit_*` helper, extended at
each scope-introducing site (fn params, lambda params + captures,
clause params + stateful aliases, match-arm pattern binders,
`SLet` bindings folded across block stmts). `emit_ident_value`
and the named-call paths (`emit_named_call_lookup` /
`emit_pipe_named_lookup`) check `lcs` before falling through to
`evar_find` / `prelude_find` / `efn_find`.

Selfhost C + LLVM byte-identical fixed point preserved (stage 2
source has no local-shadow-global pattern, so visible output is
unchanged); user code that triggers the pattern now codegens the
local read.

## Finding 4 — list.kai rename landed (v1.A)

With the shadow fix in place the rename was straightforward.
`stdlib/core/list.kai` now defines its 39 ops under bare names
and ships 29 legacy `pub fn list_X(...) = X(...)` thin aliases
at the bottom for backward compat. Internal `*_loop` helpers
demoted to private `fn`. Selfhost + full test battery green.

## Why string / option / result / char did NOT follow in v1

Cross-module bare-name collisions:

  | bare name    | collides between                               |
  |--------------|------------------------------------------------|
  | `repeat`     | list.kai (renamed) + string.kai                |
  | `map`        | option.kai + result.kai + builtin `map`        |
  | `and_then`   | option.kai + result.kai                        |
  | `unwrap_or`  | option.kai + result.kai                        |

m6.2 v1's plain-path minting is `kai_<name>` regardless of
module — two `pub fn map` defs land on the same C symbol and the
linker fails. Resolving this needs **m6.2 v2 universal prefixed
minting** (`kai_<module>__<name>`), tracked separately in
`docs/m6.2-design.md`.

The user-visible qualified surface for these four modules is
already complete via `me_lookup_export`'s prefix-fallback:

  - `string.repeat(s, n)` resolves to `string_repeat`
  - `option.map(o, f)` resolves to `opt_map` (override `option->opt`)
  - `result.is_ok(r)` resolves to `result_is_ok`
  - `char.is_digit(c)` resolves to `ch_is_digit` (override `char->ch`)

Internal-name cleanup for these four modules waits for m6.2 v2.

## Items deferred from m14 v1

  1. **Rename of `string` / `option` / `result` / `char` defs**
     (per `Why string / option / result / char did NOT follow`
     above). Blocked on m6.2 v2 universal prefixed minting.
  2. **`print` / `println` consolidation** (`Console.print` /
     `Console.println`). Independent of the rename; shipped on
     a separate lane.
  3. **Stage 1 codegen shadow fix**. The bug is identical in
     `stage1/compiler.kai`'s `emit_ident_value`. Stage 2 source
     does not currently trigger it (no local-shadow-global
     pattern), so the bootstrap chain stays consistent. Worth
     fixing for full correctness when stage 1 maintenance
     touches that area.
  4. **Stage 1 backport of `EModCall`**. Only required when
     `stage2/compiler.kai` itself starts using qualified calls
     internally; today every caller in the bootstrap chain
     stays on the legacy bare-name surface.
  5. **m6.2 v2 universal prefixed minting** (`kai_<module>__<name>`).
     Tracked separately in `docs/m6.2-design.md`. Unblocks
     v1.B-E renames and lets two stdlib modules export the
     same bare name (e.g. `list.repeat` and `string.repeat`).

## Methodological notes

  - The lane brief over-counted stdlib function totals
    (~580 LOC predicted vs. ~70 actual). Surveying the files
    early is cheap and prevents over-scoping.
  - The "selfhost fixed point byte-identical" gate is necessary
    but not sufficient — selfhost OK does not catch regressions
    in user-code semantics if no kaic2 source path triggers the
    regressed pattern. `make test` (full battery) is the second
    gate; `decimal_basic`'s `let drop = ...` was the failing
    canary that surfaced the shadow bug.
  - When a lane's "mechanical" framing assumes infrastructure
    that does not exist, factor the infrastructure out as a
    sub-commit (here: v0) and ship the user-visible surface
    on top (v1) rather than blocking on the original
    definition-rename plan. The two commits read independently
    in the log.
