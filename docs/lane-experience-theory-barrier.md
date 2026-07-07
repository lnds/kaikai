# Lane experience — the hard `theory`-catalog-only barrier

## Scope

Turn the soft `theory X = { ... }` surface (kind/theory v1, PR #1107)
into a hard barrier: a `theory` declaration is legal in exactly one
compilation unit — the canonical kind catalog
`<stdlib_path()>/core/kinds.kai` — and a hard error anywhere else, in
user code and in every other stdlib file alike. `kind` declarations
stay legal everywhere; only `theory` is gated.

Scope as planned == scope as shipped. One check plus one negative
fixture, one positive fixture adjusted, no new prims, no rebuild of
stage 1.

## Where the check lives — and why not where the brief first guessed

The brief pointed at the resolver (`check_program`, which holds
`env.file`). That was the natural guess, and it is wrong for a
measurable reason: **`DTheory` never reaches the resolver.** It is
dropped from the decl stream one pass earlier, in the post-parse
doc-strip walk `dsc_loop` (`compiler/doc_attr.kai`), whose `DTheory`
arm already discarded catalog decls before any downstream stage — the
typer, the resolver, the emitter — could see them. A check placed in
`check_program` runs, sees zero `DTheory` decls, and silently passes
(exit 0). I caught this by instrumentation, not by reading: the first
build traced the path forms *and* the `DTheory` count, and the count
was the tell.

`dsc_loop` is in fact the single best home for the barrier:

- It is the **only** pass that sees a `DTheory` with its source file in
  hand (`file` is a direct argument).
- It runs on **every** compilation unit — the root file
  (`doc_strip_collect`, driver) and every imported module
  (`doc_strip_all` via `uc_resolve_decls`) — so the barrier covers a
  `theory` smuggled in through an `import` as well as one in the root.
- It already owns the "drop catalog decls here" behaviour and already
  emits a typed error + bumps `errs` for the duplicate-module-doc case,
  so the barrier reuses an established error-propagation path
  (`diag_error_from_src` + `errs + 1`) that the driver already treats
  as fatal on both the root (`doc_errs > 0`) and module
  (`rs_bump_errs_by`) sides.

## The path-form trap — measured, not assumed

The one decision that makes the check work or fail silently is the
**form** of the two paths being compared. `stdlib_path()` is an
absolute path (baked as `-DKAI_STDLIB_PATH=$(abspath ../stdlib)`, or the
`KAIKAI_STDLIB_PATH` env override `bin/kai` exports). The compilation
unit's `file`, in contrast, is whatever the caller passed — relative
(`stdlib/core/kinds.kai`) when compiling the catalog directly, absolute
when loaded as a module, or a `/tmp` symlink that `realpath` rewrites to
`/private/tmp`. A raw `file == stdlib_path() + "/core/kinds.kai"`
compare fails on any of those mismatches.

The fix is to canonicalise **both** sides through the `abspath` prim
(POSIX `realpath`) before comparing:

```
fn theory_here_ok(file: String) : Bool =
  abspath(file) == abspath(string_concat(stdlib_path(), "/core/kinds.kai"))
```

Measured forms (traced from a one-off build) confirmed identity:

| input                                   | abspath(file)                                  | verdict |
|-----------------------------------------|------------------------------------------------|---------|
| `stdlib/core/kinds.kai` (relative)      | `<repo>/stdlib/core/kinds.kai`                 | allowed |
| `<repo>/stdlib/core/kinds.kai` (abs)    | `<repo>/stdlib/core/kinds.kai`                 | allowed |
| `/tmp/th.kai` (user)                    | `/private/tmp/th.kai`                          | error   |
| `/tmp/mycore/core/kinds.kai` (own copy) | `/private/tmp/mycore/core/kinds.kai`           | error   |

The check is **full-path-exact**, not core-prefix and not basename. A
user's own `core/kinds.kai` under a different root is rejected; a
`theory` in `core/list.kai` is rejected; and the real catalog file
compiles only when `stdlib_path()` actually resolves to its root
(proven by copying the stdlib elsewhere and flipping
`KAIKAI_STDLIB_PATH` — the same `kinds.kai` compiles with the override
on and errors with it off). This is the infalsifiable property the
brief asked for, and it fell straight out of the exact-path compare
once both sides were canonicalised — no core-set fallback was needed.

## Fixtures

- Added `examples/negative/modules/theory_outside_catalog.kai` (+
  `.err.expected` golden) — a `theory Foo = { ... }` in user code,
  rejected with the new message. Home `modules/` matches the precedent
  of the `duplicate_*_decl` fixtures: they gate *where a decl may
  appear*, which is exactly this rule's shape.
- Adjusted `examples/sugars/kind_theory_surface.kai` (PR #1107's
  positive fixture): it declared `theory AbelianGroup = { ... }` in a
  user file, which the barrier now — correctly — rejects. Dropped that
  line; the fixture keeps `kind Measure : AbelianGroup with unit`
  (naming an existing catalog theory, still legal for users) so it
  still exercises the `kind` surface + units end-to-end. Its
  `.out.expected` golden is unchanged (`kind surface ok`).

## Verification

- `stdlib/core/kinds.kai` compiles (relative and absolute), exit 0.
- Negative fixture rejects with span at the `theory` position, exit 1.
- `kind Bar : AbelianGroup with unit` in user code compiles.
- `make selfhost` byte-identical (`kaic2b.c == kaic2c.c`); stage1 too.
- `tools/test-negative.sh`: 135 PASS, 0 FAIL, 0 MISS.
- A normal user program (imports core modules) compiles clean — no
  false positive from any auto-loaded core file, since none declares a
  `theory`; `kinds.kai` is not in the auto-loaded core set.

## Follow-ups

- `kinds.kai` is not in `core_module_files()`, so it is never
  auto-loaded; it is compiled only when named directly. If a future
  lane adds it to the core set, the barrier already handles that path
  form (module load canonicalises through the same `abspath`).
- The message names `core/kinds.kai` (the stable relative tail), not the
  absolute path, so it reads correctly regardless of install layout.
