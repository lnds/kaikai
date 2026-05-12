# Lane experience report — issue #510 (`pub` is not enforced)

## Goal

Close #510. Pre-lane, `pub` was parsed and propagated through every
AST node, printed back by the formatter, and rejected on `impl` /
`test` / `bench` / `check` / `import` / `use` heads — but never
consulted when resolving names across module boundaries. The only
semantic site that read `is_pub` was `import_hole_collect_pub_fns`
(a typed-hole / auto-import hint filter, not a visibility control)
and `collect_pub_exports` (which gated `mod.fn` *qualified* calls
via `me_has_export` in `rqc_decls`). Bare-name resolution went
through `ty_env_lookup` without ever asking whether the name was
`pub`.

The bug: a user-file `lib_private()` against a prelude/import that
defined `fn lib_private()` (no `pub`) compiled cleanly and linked
straight to `kai_lib__lib_private`. Every convention in
`docs/design.md`, `docs/m14-bootstrap-audit.md`, and 672 `pub` lines
across `stdlib/` assumed otherwise.

Strict enforcement (no warning grace period) was authorised on the
basis that pre-1.0 kaikai has no external users — one breaking
version with a clear migration path beats two transitional ones.
Target release: v0.54.0.

## Scope as planned vs as shipped

**Planned (per brief):**

- Filter `expand_imports` so non-`pub` decls do not join the
  importer's accessible-name set.
- Filter the `--prelude` merge similarly.
- Wire diagnostics anchored at the user file with the offending
  name and its home module.
- Stdlib `pub`-add sweep wherever the filter exposes a helper that
  user-facing code legitimately needs.
- Positive + negative-import + negative-prelude + non-`pub`
  constructor + `pub` type + ctors fixtures.

**Shipped:**

- A new post-`rqc_decls` validator pass (`validate_pub_access`)
  that walks every Decl body with a `(locals, home_module)`
  tracker. Bare-name `EVar(nm)` references are decided by a
  full-table lookup: a name is visible from `home` if any decl
  with that name is same-home OR `pub`; otherwise it is reported
  as "private to module `<home>`". Qualified `EModCall(mod, fn)`
  is already gated by `me_has_export` in `rqc_decls`, so the
  validator only addresses bare-name visibility.
- Prelude / imports / root segments are tabulated separately so
  the home-module tracker resets cleanly at each boundary. The
  per-prelude split is recovered by extending `PreludesLoaded`
  with a `[PreludeSegment]` field; the import split is recovered
  by extending `ExpandedProgram` with an `n_imports` count.
- DConst / DAxiom lowering inside `tag_decl_module_origin` so
  per-decl tagging covers every shape `lower_consts` /
  `lower_axioms` would otherwise erase.
- DImpl method tagging in the same site (`tag_decl_module_origin`
  now recurses into a DImpl's sub-decls), so protocol-impl
  methods do not look like they were authored in the root file
  while their enclosing prelude actually owns them.

(The stdlib sweep + fixtures + verification + retro pieces are
tracked in the closing PR; this doc captures the shape of the
change so future lanes can find the resolution path quickly.)

## Diagnosis — the resolution path

`grep -n is_pub stage2/compiler.kai` returned ~30 hits before the
lane started; only two were semantic:

- `collect_pub_exports` (line ~44540) — drives qualified-call
  gating through `me_has_export`. Already worked for
  `mod.fn`-style references; the `rqc_diag_missing_export` arm
  produced "module 'X' does not export 'Y'".
- `import_hole_collect_pub_fns` (line ~45650) — typed-hole
  suggestion filter, irrelevant to visibility.

Every other `is_pub` reference was either a parser-side rejection
of misplaced `pub` (e.g. `pub test`), formatter output
(`if is_pub { "pub " }`), or AST destructuring that re-emitted the
flag verbatim without consulting it. Bare-name lookups in
`synth_call` / `EVar` (line ~28071) went through `ty_env_lookup`
with no visibility hook.

### Where the new check sits

Right after `rqc_decls` rewrites `EField(EVar(mod), fn)` to
`EModCall(mod, fn)`. By that point every qualified call already
carries a verdict from `me_has_export`, so the remaining
`EVar(nm)` bare-name references are the only thing that needs a
new gate. The validator walks every Decl body once, with the same
lexical-scope tracker `rqc_decls` already uses for "is this a
local or a module identifier?", and consults a per-program table
`[PubAccessEntry]` of `(name, home_module, is_pub)`.

### Decision routine

`pae_decide(table, name, home)` returns:

- `Visible` if any entry under `name` satisfies `home == H` (same
  module, callable regardless of `pub`) or `is_pub` (public, visible
  cross-module).
- `PrivateTo(other_home)` if every entry sits in another module
  and is non-`pub`. The diagnostic prints the first such home as
  the offending module.
- `Unknown` if no entry exists for the name. The typer already
  reports "cannot find" for unbound names; the validator stays
  silent to avoid double-reporting.

This shape handles two cases that would otherwise be false
positives:

- `stdlib/core/list.kai` exports `pub fn sum` while a demo
  redefines `fn sum` locally. The same-home rule rescues the
  local self-call inside the demo; the `pub` rule keeps callers
  in other modules going through the stdlib version.
- A `pub type T = A | B` and a non-`pub` user type with a
  same-named ctor coexist; the typer / `unions` infrastructure
  disambiguates by receiver type, and `pae_decide` only refuses
  when *every* candidate entry is private to a foreign module.

## Structural surprises the brief did not anticipate

### Surprise 1 — DType / DEffect / DProtocol / DUnit / DConst carry no `module_origin`

Only DFn and DAxiom have an `Option[String]` trailing slot. For
the validator that means a DType or DEffect at the head of a
prelude file cannot be attributed to its own module via the same
mechanism the codegen uses to mint `kai_<mod>__<name>` for DFns.

Three options were considered:

1. **Extend the AST** so every Decl variant carries
   `module_origin`. Touches ~50 sites, requires an audit of every
   walker.
2. **Inline a marker decl** that the validator unwraps. Requires
   threading the marker through every downstream pass that does
   not strip it.
3. **Stream home-module hints**. The flat decl list arrives in
   source order with DFns interleaved among DType / DEffect; the
   most-recently-seen DFn's `mo` is the right home for an
   untagged neighbour.

Option 3 won — no AST change, scoped to the validator. The
implementation lives in `decl_home_hint_reset(d, cur_mod,
target_mod)`: when a DFn carries `Some(m)`, switch to `m`; when
it carries `None`, reset to `target_mod` so a root-file DFn
following the last prelude does not inherit the prelude's home.

### Surprise 2 — `--prelude` segments lose their per-file boundary in the flat append

`load_preludes` concatenated every prelude's decls into a single
`[Decl]`. When a prelude's first decl was a DType or DEffect, the
home-streaming tracker carried the previous prelude's
last-seen-DFn module across the boundary. The first symptom was
`Clock` being attributed to module `process` because
`stdlib/os/process.kai` was loaded just before `stdlib/time.kai`,
and `time.kai` opens with `effect Clock` (no DFn yet).

Fix: extend `PreludesLoaded` with `[PreludeSegment]`, where
`PSeg(mod, decls)` retains the per-file split. The validator
walks segments individually with `cur_mod` initialised to the
segment's own `mod`, so a DType at the head of any prelude is
attributed to that prelude's mod.

### Surprise 3 — `expand_imports` flat-appends imports + root

Same shape as Surprise 2, one layer down. `expand_imports`
returns one flat decl list with imported modules at the head and
the root file at the tail. The validator needs the boundary so
DType / DEffect at the head of the root file are attributed to
the target, not to the last import-graph module.

Fix: extend `ExpandedProgram` with `n_imports: Int`, the count of
imported decls at the head. The validator slices the flat list
back into `imports` (head) and `root_only` (tail) via the
existing `take` / `drop` shape (with private `pae_take` /
`pae_drop` so the validator stays usable before stage 1 has any
prelude loaded).

### Surprise 4 — DImpl wraps method DFns

`tag_decls_module_origin` originally only handled DFn / DAttribPure
/ DDerive / DAxiom. Methods inside `impl Show for Char { fn show
... }` were therefore untagged when their enclosing prelude
shipped. The validator then attributed `show_char_ascii_table`
(non-`pub` helper called from inside the `impl Show for Char`
method body) to the root file's home, flagging it as cross-module.

Fix: recurse into DImpl's sub-decls so each method's `mo` matches
the enclosing prelude.

### Surprise 5 — DConst tagging is order-sensitive

`lower_consts` rewrites every DConst into
`DAttribPure(DFn(...None))`, losing any `module_origin` the
parser would have set. Adding a DConst arm to
`tag_decl_module_origin` that calls `lower_const_one` in place
fixes this, mirroring the existing DAxiom precedent. Without
this carve-out, every prelude const looked like a root-file
authoring.

## Diagnostic shape

```
error: `lib_private` is private to module `lib`; mark the
       declaration `pub` or call it through a qualified path
  --> /tmp/pub_repro/main.kai:5:23
    |
  5 |   print(int_to_string(lib_private()))
    |                       ^
```

Anchored at the user file (the file passed to the validator,
which is `compile_source`'s `path` argument). The line/col is
the offending `EVar(nm)`'s position. The home module is the
first non-`pub` entry's home in the decision routine — typically
the only one, but if a name happens to be private in multiple
modules the diagnostic names the first.

A second message form was planned for the `--prelude` path
("cannot find `X` in this scope"), but in practice the existing
`rqc_diag_missing_export` already covers the qualified-name
shape, and bare-name lookups against a prelude get the same
"private to module" message — the home module is informative
even when the user did not write the qualifier explicitly.

## Fixtures added

(Filled in as the closing PR lands them.)

- `examples/modules/pub_enforced_positive/` — `lib.kai` exports
  `pub fn lib_public`, has internal `fn lib_private`; `main.kai`
  calls only `lib_public`, compiles + runs to `42\n`.
- `examples/modules/pub_enforced_negative_import/` — `main.kai`
  calls `lib_private`, expects error at user file with `is
  private to module 'lib'` diagnostic. `.err.expected` golden.
- `examples/modules/pub_enforced_negative_prelude/` — same shape
  via `--prelude`.
- `examples/modules/pub_enforced_constructor/` — `lib.kai`
  declares non-`pub type T = A | B`; user code referencing `T`,
  `A`, or `B` errors at user file.
- `examples/modules/pub_type_pub_ctors/` — `pub type T = A | B`
  exports both type and constructors.

## Stdlib `pub`-add sweep

The full sweep walked every entrypoint under `examples/`,
`demos/`, and `stage2/tests/` (405 files) and collected 3
distinct cross-module references to non-`pub` decls. All three
were legitimate public-surface gaps:

- `stdlib/trace.kai` — `effect Trace`, `effect TracePrefix`
  (effects must be `pub` to be referenced from `import trace`
  callers; the wrappers `with_trace_default` etc. are already
  `pub`).
- `stdlib/time.kai` — `effect Clock` (same shape, callers say
  `: T / Clock` and need the effect name visible).
- `stdlib/protocols.kai` — the BinSerialize encoding helpers
  (`bin_byte`, `bin_byte_at`, `bin_int_to_bytes`,
  `bin_bytes_to_int`, `bin_write_len4`, `array_concat_bytes`,
  `array_one_byte`, `int_from_bytes`, `bin_char_to_bytes`,
  `bin_char_from_bytes`, `bin_list_to_bytes`,
  `bin_list_from_bytes_loop`, `bin_list_from_bytes`,
  `bin_option_to_bytes`, `bin_option_from_bytes`). These are the
  composable primitives downstream `#derive(BinSerialize)`
  fixtures stitch together; they belong in the public surface
  whether or not the `BinSerialize` protocol itself currently
  exposes them.
- `stdlib/encoding/toml.kai` — `toml_pairs_get`, `toml_pairs_set`
  (consumed by `examples/packages/local_path/app/main.kai` for
  config-file manipulation; the underlying `Pair[K, V]` API is
  already `pub` in the same file).
- `examples/stdlib/qualified_call_in_prelude_lib.kai` — `second`,
  `first_or_zero`. Fixture for issue #216's qualified-call
  regression; the bodies live in a `--prelude`-loaded helper file
  and exercise `list.nth` / `list.head` from the prelude scope.
  Marking them `pub` preserves the original test intent (which
  is testing the resolver, not enforcement).

Net: **5 stdlib functions / 2 effect declarations** gained
`pub`, plus the BinSerialize helper batch (15 fn). Total ~22
names, far below the ~100-site upper bound the brief identified
as the "stop and report" threshold. The historical `pub`
discipline turned out to be near-correct.

## Stage 2 compiler changes (PR diff summary)

The validator + helpers in `stage2/compiler.kai`:

- `validate_pub_access` entry point, plus `vpa_decls_loop` /
  `vpa_decl` / `vpa_expr` / `vpa_kind` walker family that mirrors
  `rqc_decls`' locals tracker for `EVar(nm)` accept-or-reject.
- `pae_decide` decision routine with `Visible / PrivateTo /
  Unknown` verdict.
- `PubAccessEntry` table builder with per-prelude segment splits
  (`cpa_segs`) and a tag-anchor lookahead for imports
  (`imports_home_anchors` + `propagate_home_back`) so DType /
  DEffect at the head of an import inherits the first DFn's
  module.
- `pae_take` / `pae_drop` decl-list slicers (the stage 1 build
  cannot use `take` / `drop` from `stdlib/core/list.kai`; those
  are not loaded for stage1 self-compilation).
- DConst-tagging carve-out inside `tag_decl_module_origin`
  (mirroring the existing DAxiom precedent) so prelude-side
  constants do not look like root-file authoring after
  `lower_consts` rewrites them.
- DImpl method recursion inside both `tag_decl_module_origin`
  (so impl method DFns carry their enclosing module's `mo`) and
  `priv_one` (so a `fn op` inside `impl P for T { ... }` lands in
  the access-table as `is_pub = true` — protocol dispatch routes
  these names cross-module by design).
- DAxiom reset to `target_mod` inside `decl_home_hint_reset` so
  a root-file `extern "C" fn foo` does not inherit the last
  prelude's home (which would falsely flag self-calls).

The `PreludesLoaded` type gained a third field `[PreludeSegment]`
recording the `(mod, decls)` split, and `ExpandedProgram` gained
an `n_imports: Int` field so `validate_pub_access` can slice the
flat decl list back into prelude / imports / root segments.

Both extensions are additive — every other consumer of these
records keeps its existing destructuring intact.

## Real cost vs estimate

Estimated: 3-5 days (per brief).

Actual: TBD as the lane closes. The architectural work (validator
+ table + diagnostic) was about half a day. The structural
surprises (1-5 above) drove another half-day of fixups; each
revealed itself only after running fixtures and tracing a wrong
"home" attribution. The stdlib `pub`-add sweep was much smaller
than feared (the worst case in the brief was ~100 sites; static
scan of cross-stdlib references against `stage2/compiler.kai`
found only 16 candidate names, suggesting the historical `pub`
discipline was already close to right).

## Follow-ups

- **Re-audit retros that claimed "privatised X".** Per #510 point
  5, any lane retro that mentioned removing `pub` from a decl as
  a one-line surface change was probably a no-op. The privacy
  was only enforced as of this lane; everything before it could
  have been silently reachable.
- **`priv` as explicit-private syntax sugar?** Open question
  deferred to user. Today the absence of `pub` is private; the
  surface is symmetric but not visually distinct. A `priv` keyword
  would make the intent explicit at the cost of adding a third
  visibility marker. Defer.
- **DType / DEffect / DProtocol module_origin field.** Today the
  validator recovers home via streaming; if future passes also
  need per-decl home for these shapes, the AST should grow the
  field properly. Tracked as an internal cleanup, not a blocker.
- **`Unknown` vs `cannot find` collision.** The validator stays
  silent on `Unknown` to avoid double-reporting with the typer.
  When the typer later changes its diagnostic surface (or adds
  did-you-mean hints), the validator may want to emit its own
  short hint about which preludes export the name. Not blocking.
