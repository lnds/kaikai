# Lane retrospective — Issue #594: convention-based pipe dispatch

## Scope as planned

Replace the hardcoded `head_module_for(head)` table inside
`stage2/compiler.kai` (which knew exactly one entry: `List → list`)
with a per-compile head-owner cache built from the post-`expand_imports`
decl stream + module table. Any package that declares `pub type T`
and exports the canonical `pub fn map / flat_map / filter` participates
in `|`, `||`, `|?` automatically — no compiler change required when a
new type wants pipe surface.

15 fixtures (6 positive baseline, 2 positive convention multi-file,
6 negative anchored, 1 perf probe), 3 doc updates (`docs/protocols.md`,
`docs/design.md`, source-code comment block), selfhost byte-identical,
tier0 + tier1 green, perf regression ≤ 1%.

## Scope as shipped

Identical to plan. No design departures from the issue body.

One small reformulation: the issue body's positive fixture #8
("package with TWO `pub type` declarations exporting their own
`map / filter`") could not be expressed as a single module —
stage 2 ships no name overloading inside one module, so two `pub fn
map` decls collide. Reformulated as a "package" of two sibling
modules under one directory (`mybox.kai` + `mystream.kai`). The
head-owner cache still carries entries for both heads pointing at
distinct modules; the dispatch invariant holds.

## Cache shape

```
type HeadOwnerEntry = HOE(String, [String])
type HeadLookup = HLOk(String) | HLAmbiguous([String]) | HLNone

# Field on TyEnv
head_owners: [HeadOwnerEntry]
```

Single-element `mods` list is the dispatchable case; multi-element
flags ambiguity. Lookup at each pipe call site is `find_head_owner`
walking the small linear list. Cache built once per
`compile_source` invocation by `build_head_owner_map`.

The cache lives on `TyEnv` rather than `InferState` because:

- 25+ `st_set_*` helpers that reconstruct `InferState` would each
  need a new field; a `TyEnv` field requires updating the 15
  `TyEnv { ... }` constructors only.
- `synth_pipe_dispatch` already has `st.env` in scope, so the
  lookup is one function call without threading new parameters
  through the typer's recursion.

## LOC per site

- `stage2/compiler.kai`:
  - +21 LOC: `TyEnv` field + `HeadOwnerEntry` type + `ty_env_with_head_owners` helper.
  - +57 LOC: `build_head_owner_map` + `collect_pub_type_names` + `build_head_owner_loop` + `modules_declaring_type` + `filter_out_target` + `any_module_named` (+ extensive doc comment block).
  - +18 LOC: `HeadLookup` type + `find_head_owner` + `find_head_owner_loop`.
  - −3 LOC: removed `head_module_for(head: String) : Option[String]`.
  - +35 LOC: `synth_pipe_dispatch` rewrite (4 diagnostic branches: HLNone, HLAmbiguous, HLOk-no-qualified, HLOk-with-qualified) + `join_modules_for_diag`.
  - +20 LOC: `infer_program_with_protos_cached` + `typecheck_program_with_head_owners` + threading through `typecheck_program_loop` + `typecheck_module`.
  - +15 LOC: 15 `TyEnv { ... }` construction sites updated.
  - +1 LOC: `compile_source` invocation site (build cache + pass to typer).
  - **Net ~165 LOC compiler-side**, slightly above the issue body's
    50–70 LOC estimate. The overrun came from threading the cache
    through `typecheck_program → typecheck_program_loop →
    typecheck_module → ty_env_with_head_owners` rather than
    stuffing it directly into `InferState`; the trade was for
    fewer touched call sites overall.
- Fixtures: 11 new files in `examples/pipes/` (6 positive
  baseline), 7 new files in `examples/multi-module/` (2 positive
  convention, multi-file), 9 new files in `examples/negative/pipes/`
  (6 negative anchored).
- Docs: ~50 lines in `docs/protocols.md` §"Pipe operators are
  convention-based"; +2 lines in `docs/design.md` "Decisions"; the
  comment block at the cache definition site is ~60 lines.

## Structural surprises the brief did not anticipate

1. **`DType` does not carry a module-origin tag.** `DFn`'s 10th
   field (`Option[String] module_origin`) is set by
   `tag_decl_module_origin` during `process_imports`; the same pass
   has carve-outs for `DAxiom`, `DConst`, `DImpl`, but `DType`
   intentionally stays untagged because its codegen contract
   (`kai_<type>_<variant>` mangling) doesn't need a module
   prefix. So I cannot derive a `pub type T`'s home module from the
   decl itself — I have to consult `[ModuleEntry]` and search by
   `T ∈ me.exports`.
2. **The target file's `ModuleEntry.exports` carries every imported
   pub name.** `compile_source` builds `target_me` from
   `collect_pub_exports(expanded_decls_raw, [])` where
   `expanded_decls_raw` already has the imported decls merged in.
   So `MyBox` exported by `mybox.kai` appears in BOTH `mybox`'s
   exports AND the root file's exports. Without a target-filter,
   every imported `pub type` looked ambiguously declared by
   `(<owning_module>, <root_basename>)`. Added `filter_out_target`
   to drop the target ME from the dispatch search.
3. **`List` is a built-in head with no user-visible `pub type
   List` decl.** The parser stamps `[T]` syntax directly to
   `TyListT`; `core/list.kai` ships `pub fn map / flat_map /
   filter` against `List[T]` (where `List` is the builtin name
   resolved by `ty_head_name(TyListT(_))`). The cache had to seed
   `List → list` unconditionally so the test-sugars harness
   (which does NOT preload `core/list.kai` and relies on the
   runtime `kai_prelude_*` stubs via the bare-name `EVar(op)`
   fallback) keeps working. Without the seed, every `[T] | f`
   surfaced "no module declaring type `List` is in scope" — a
   regression in 3 demos and several stage 1 fixtures.
4. **No name overloading inside a module.** The issue body asked
   for a single-module fixture with two `pub type` heads each
   exporting `map`; stage 2 rejects two `pub fn map` decls in the
   same file with a "function name collision" error. Reformulated
   as two sibling modules in a "package" directory.

## Fixtures added

| Tier | Path | Purpose |
|---|---|---|
| Positive baseline | `examples/pipes/pipe_list_map_baseline.kai` | `[Int] | f` resolves through cache exactly as pre-#594 hardcoded path |
| Positive baseline | `examples/pipes/pipe_list_flat_map.kai` | `[Int] || f` regression guard for `flat_map` |
| Positive baseline | `examples/pipes/pipe_list_filter.kai` | `[Int] |? p` regression guard for `filter` |
| Positive baseline | `examples/pipes/pipe_apply_baseline.kai` | `|>` apply pipe untouched (it does NOT use the dispatch table) |
| Positive baseline | `examples/pipes/pipe_chained.kai` | three pipes in a row, type inference threads through |
| Positive baseline | `examples/pipes/pipe_with_effect.kai` | `Console` row propagates through pipe rewrite |
| Positive convention | `examples/multi-module/pipe_custom_type/` | `MyBox` package + main consumer, no compiler change |
| Positive convention | `examples/multi-module/pipe_two_types_same_pkg/` | sibling-module package with `MyBox` and `MyStream`, both dispatchable |
| Negative anchored | `examples/negative/pipes/pipe_no_module.kai` | root-only type → "no module declaring type `T` is in scope" |
| Negative anchored | `examples/negative/pipes/pipe_no_map/` | module declares Box, no `map` → "module `box` declares `Box` but does not export `map`" |
| Negative anchored | `examples/negative/pipes/pipe_ambiguous/` | two modules declare same head → ambiguous diagnostic |
| Negative anchored | `examples/negative/pipes/pipe_wrong_signature/` | inverted-order `map` shape → standard call-inference signature mismatch |
| Negative anchored | `examples/negative/pipes/pipe_option_rejected.kai` | `Some(_) | f` keeps issue #201 Constraint #1 rejection |
| Negative anchored | `examples/negative/pipes/pipe_result_rejected.kai` | `Ok(_) | f` keeps issue #201 Constraint #1 rejection |
| Updated | `examples/sequence/pipe_no_dispatch_for_head` | message updated from "no `map` registered" → "no module declaring type" |
| Perf probe | `stage2/kaic2 stage2/compiler.kai` median wall | ~40.83s post vs ~40.98s pre, well under 1% |

## Real cost vs estimate

- Estimate: 50–70 LOC in `stage2/compiler.kai`, single agent session.
- Actual: ~165 LOC in `stage2/compiler.kai` (3× the estimate). The
  extra came from threading the cache through the typer entry-point
  cascade; the inner builder + lookup + diagnostic logic IS within
  the estimate. A single agent session was enough.
- Selfhost byte-identical confirmed (`make -C stage2 selfhost`).
- Performance: 0.4% delta, well under the 1% gate.

## Coverage gaps and follow-ups

- Convention extension to other operators (`scan`, `take`, `drop`,
  `concat`, etc.) deliberately deferred — the issue body listed
  these as out of scope. If a downstream package surfaces evidence
  ("we want `xs |> drop(3)` but we have to write `mod.drop(xs, 3)`"),
  open a separate issue with the canonical signature for that op.
- Annotation `#[pipe_dispatch]` deferred — no evidence today that a
  package wants to opt out of the convention.
- Custom operator definitions (`pub operator |>>`) deferred — same
  reason.
- The seeded `List → list` entry is the only built-in head that
  receives special treatment. If any other built-in container type
  ever ships (e.g. a parser-stamped `Map[K, V]` literal), it needs
  its own seed entry. Today there is no such case.

## Lessons

- **`DType` lacking a module-origin field is a recurring debt
  signal.** Three different lanes have wanted to ask "which module
  declared this type?" in the last six months; each has had to
  derive it from the surrounding context. A future lane should
  consider adding `Option[String] module_origin` to `DType`
  uniformly with the existing `DFn` field, and propagate through
  the 62 DType pattern matches mechanically. Out of scope here.
- **The target ME's exports list is structurally noisy.** It
  conflates "this file's pub decls" with "every pub decl in
  scope". Other lanes that need the former had to filter — same
  pattern this lane added. A future cleanup might split target_me
  into "own pub" vs "transitively visible pub".
