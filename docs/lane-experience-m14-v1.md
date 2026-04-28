# Lane experience — m14 v1 (stdlib qualified-call surface)

Date: 2026-04-28. Branch: `m14-v1`. Two commits:

  - `m14 v0: register --prelude files as modules` (infra prerequisite)
  - `m14 v1: qualified-call surface for stdlib (prefix-fallback)`

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

## Items deferred from m14 v1

  1. **Rename of stdlib definitions** to drop the prefix
     (`pub fn list_take` -> `pub fn take`). Blocked on the
     `emit_ident_value` shadow fix; the prefix-fallback is
     callable forever in principle, but the long-term goal is
     the cleaner internal form once the codegen is fixed.
  2. **`print` / `println` consolidation** (`Console.print` /
     `Console.println`). Independent of the rename; shipped on
     a separate lane.
  3. **Stage 1 backport of `EModCall`**. Only required when
     `stage2/compiler.kai` itself starts using qualified calls
     internally; today every caller in the bootstrap chain
     stays on the legacy bare-name surface.
  4. **m6.2 v2 universal prefixed minting** (`kai_<module>__<name>`).
     Tracked separately in `docs/m6.2-design.md`. Becomes
     relevant when two stdlib modules want to export the same
     bare name (e.g. `list.repeat` and `string.repeat`).

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
