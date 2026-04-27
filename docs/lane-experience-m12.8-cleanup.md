# Lane experience report — m12.8-cleanup

Lane: convert hand-written `dump_X` / `eq_X` / `hash_X` functions in
`stage2/compiler.kai` to `impl Show / Eq / Hash for X` via `#derive`
(records) or manual impl (sum types). Spec dividend predicted in the
m12.8 retrospective: ~150 LOC reduction.

## Result, up front

**Zero conversions performed. Zero LOC reduced.**

This is not a v1 `#derive` capability problem. It is that the kaikai
compiler does **not contain hand-written structural Show / Eq / Hash
functions** to convert. Every candidate the lane brief listed turns out
to be one of:

1. A multi-line, depth-indented AST/IR pretty-printer that is the
   implementation of a CLI `--dump-*` flag (API contract, golden-tested).
2. A diagnostic-formatted `*_to_string` / `*_display` with a custom
   render shape (`(a, b) -> Bool`, `Eff[Int]`, `Real<u>`) that no
   structural `Show` derivation could reproduce.
3. A partial-state equality (`row_label_eq` compares names only,
   ignoring args by design).

The build remained green throughout the audit. The branch is left ready
with no source changes; the report alone is the deliverable.

## Objective metrics

- Start: `2026-04-26T20:00:54-04:00`
- End:   `2026-04-26T20:08:04-04:00`
- Wall-clock: ~7 min (one Claude session, audit only)
- Build/test invocations on the unchanged branch (baseline confirmation):
  - `make all` — OK
  - `make selfhost` — OK (stage 1 + stage 2 fixed point)
  - `make -C stage2 selfhost-llvm` — OK
  - `make test` — OK (full suite, including `test-protocols` and
    every `--dump-*` smoke test)

## Conversion inventory

| Type | Original LOC | Converted via | New LOC | Saved |
|---|---:|---|---:|---:|
| (none) | 0 | — | 0 | 0 |
| **Total saved** | | | | **0** |

## What was NOT converted and why

The audit walked every `dump_`, `*_eq`, `*_hash`, `*_to_string`,
`*_show`, and `*_display` function in `stage2/compiler.kai` (and
`stage1/compiler.kai` for completeness — stage 1 has no protocol
support). Each is listed below with the reason it is out of scope.

### `dump_*` functions — CLI API contract

All 30+ `dump_*` functions in `stage2/compiler.kai` implement the body
of a `--dump-tokens` / `--dump-typed` / `--dump-mono` / `--dump-mono-out`
/ `--dump-last-use` / `--dump-holes` / `--dump-holes-json` / `--types`
CLI flag. Their output is depth-indented multi-line text with explicit
source-position annotations (`@line:col`), e.g.:

```
fn pub "name" @5:3
  param x @5:8
    ty_name Int @5:10
  ty_name Bool @5:15
```

`#derive(Show)` would emit a single-line bracketed structural rendering
(`Param { pname: "x", ptype: Some(...), pline: 5, pcol: 8 }`). That
render is not API-compatible with the CLI contract:

- **Tested by Makefile targets**: `test-dump-typed`, `test-dump-mono`,
  `test-tokens`, `test-ast`, `test-types`, `test-env`, `test-infer`,
  `test-holes` (`stage2/Makefile:85-112`, `:1119`). Output shape
  changes would diff against committed golden files.
- **Stateful printing**: each `dump_*` calls `emit_line(depth, ...)`
  with a recursive `depth + 1`; the *side-effect of indented printing*
  is the function's job. `Show` returns `String` — the recursion
  pattern doesn't translate.
- **Cross-cuts external state**: `dump_token(src, t)` consults `src`
  (the source buffer) to slice the token text — `Show for Token`
  cannot do that without breaking the protocol's pure-function shape.

Functions in this category (line numbers in `stage2/compiler.kai`):
`dump_token` (718), `dump_tokens` (729), `dump_expr` (4416),
`dump_hclause` (4526), `dump_hreturn` (4536), `dump_arm` (4545),
`dump_field_init` (4556), `dump_list_elem` (4565), `dump_stmt` (4575),
`dump_pat` (4603), `dump_pfield` (4639), `dump_type` (4649),
`dump_row_expr` (4672), `dump_decl` (4743), `dump_proto_op` (4806),
`dump_effect_op` (4817), `dump_param` (4828), `dump_type_body` (4833),
`dump_field_decl` (4855), `dump_variant` (4864), `dump_program` (4873),
`dump_decl_type` (10069), `dump_types` (10117), `dump_env_entry` (10948),
`dump_env` (10951), `dump_infer` (15089), `dump_typed_expr` (15149),
`dump_typed_exprs` (15161), `dump_typed_arms` (15168),
`dump_typed_fis` (15186), `dump_typed_elems` (15196),
`dump_typed_stmts` (15209), `dump_typed_subexprs` (15216),
`dump_typed_stmt` (15253), `dump_typed_decl` (15275),
`dump_typed` (15285), `dump_mono_line` (15302), `dump_mono` (15311),
`dump_mono_out_line` (15325), `dump_mono_out` (15335),
`dump_last_use_for_param` (16136), `dump_last_use_for_decl` (16145),
`dump_last_use` (16156), `dump_scope_entry` (16172),
`dump_hole_candidate` (16175), `dump_hole_report` (16178),
`dump_holes_loop` (16220), `dump_holes` (16350),
`dump_holes_json` (16459).

### `*_to_string` / `*_display` — custom diagnostic formats

These render values as user-facing text in error messages. Their output
shapes are deliberately *not* the structural `Show` shape — they are
the syntax users wrote:

| Function | Renders | Why not structural Show |
|---|---|---|
| `ty_to_string` (9947) | `(Int, Real) -> Bool / Reader[Int]` | Function arrows, list brackets, row suffix — not a record literal |
| `row_label_display` (4695) | `Eff[T1, T2]` | Bare effect or bracketed args, never the constructor name |
| `type_expr_display` (4702) | `Money[USD]`, `[Int]`, `(a) -> b` | Mirrors source syntax |
| `unit_expr_display` (4718, m12.5) | `kg * m / s^2` | Algebraic unit expression |
| `unit_canon_display` (9983) | canonicalised `kg * m * s^-2` | Sorted/grouped powers |
| `row_to_string_suffix` (10048) | ` / Console + IO + ?e3` | Effect-row syntax with row-var |
| `label_to_string` (14752) | `Reader[Int]` or bare `Console` | Same as `row_label_display`, different type |
| `scheme_to_string` (10933) | `forall a b. a -> b` | Type-scheme syntax |
| `tok_line_str`, `tok_col_str` (715-716) | `int_to_string(t.line)` | Trivial wrapper; not a Show pattern |

Diagnostic format changes are user-visible: error messages reference
exact spans and would diff in `make test`.

### `*_eq` — partial / custom semantics

| Function | Why not `#derive(Eq)` |
|---|---|
| `row_label_eq` (9806) | Compares only the name field, ignoring `args` by design — see comment at 9824 ("for now we dedup by name only"). `#derive(Eq)` would compare both, changing dedup behavior |
| `ty_eq_shape` (12232) | Sum type (`Ty`); `#derive(Eq)` for sum types is **not** in v1 (see m12.8 follow-up list). Also intentionally ignores the row arg of `TyFnT(_, _, _)` — semantically partial, would not match `#derive` |
| `ty_list_eq_shape` (12253) | Wraps `ty_eq_shape`; same reason |
| `char_eq_at` (874) | Index-based comparison helper, not a per-type `eq` |

### Records that *could* hypothetically grow an `impl Show`

The compiler defines 107 records and ~30 sum types. None of them
currently has a hand-written `show_X` / `dump_X` returning a structural
string. Adding new `impl Show for X` blocks would be net-new code, not
a "convert hand-written to derived" cleanup, and would expand scope
beyond what the lane brief authorises.

## Spec gaps discovered

The m12.8 retrospective's "Out-of-scope cleanup follow-ups" entry
estimated "~150 lines of hand-written `dump_X` / `eq_X` / `hash_X` in
the compiler". The estimate over-projects because:

1. **`dump_X`** in this codebase means *indented AST printer*, not
   *structural value printer*. The two share a name prefix but solve
   different problems. The retrospective conflated them.
2. **`eq_X`** functions barely exist in the compiler — only one
   (`row_label_eq`), and it is intentionally non-structural.
3. **`hash_X`** functions do not exist in the compiler at all.

The dividend the retrospective imagined would require **rewriting**
the dump pipeline to a two-layer design (structural `Show` + a separate
indented-tree formatter that consumes it). That is a meaningful
refactor in its own right, not a `#derive` cleanup. It would also
require `#derive(Show)` to support sum types whose variant args
themselves are sums (already supported in v1 per
`derive_show_sum_arm_body` at 19589) and to honour custom field
formatting — which it intentionally does not.

### `#derive` v1 limitations re-confirmed (none bit on this lane)

For completeness, the limitations from `docs/lane-experience-m12.8.md`:
- `#derive(Eq) / #derive(Hash)` for sum types: not in v1.
- Default impls inside `protocol { ... }`: not in v1.
- String interp `#{x}` does not auto-dispatch through `Show`.

None of these became relevant because no candidate reached the point of
needing them — the structural-mismatch with `dump_*` shape blocked
every conversion at the format-compatibility step.

## Subjective summary

- **LOC reduction achieved vs estimated 150**: 0 vs 150. The estimate
  was speculative and not borne out by audit.
- **Did the limitation list from m12.8's report match reality?** The
  listed limitations are real but *did not bind* on this lane —
  the binding constraint is upstream: the compiler simply has no
  structural-Show/Eq/Hash hand-written functions to convert. A future
  refactor that splits `dump_*` into "structural Show" + "indented
  formatter" would unlock the dividend, but that is a design change,
  not a cleanup.
- **Surprises / friction**: none. The audit was mechanical and
  conclusive within ~7 minutes of file walking. The build was green
  before, during, and after — `make all && make selfhost && make
  -C stage2 selfhost-llvm && make test` all OK on the unchanged branch.
- **Confidence in correctness**: trivially high — no source changed,
  so no behavior changed. The selfhost gate is moot here; it confirms
  the baseline still passes.
- **What I deliberately did not do**: I did not invent new `impl Show`
  blocks for records that lack any string conversion today. The lane
  brief is "convert hand-written to derived", not "add new derives".
  Adding net-new impls is a separate scoping decision worth proposing
  if the protocol coverage matters for downstream features (e.g.
  Map[K, V] needing `Hash` on K).

## Recommendations for follow-up lanes

These are observations, not asks — feeding into future scoping.

1. **`dump_*` two-layer split**: a real LOC dividend from `#derive`
   requires first refactoring `dump_*` into `impl Show for X` (one-line
   structural) plus a `format_indented(x: X, depth: Int)` that consumes
   `Show` and adds the depth. This pays off only if the indented form
   stays byte-identical to today's CLI output — likely not without
   either custom field formatters in `#derive` or a hand-written
   wrapper per type. Net win is unclear; estimated 200-400 LOC of
   refactor for ~50 LOC of derived code.
2. **`#derive(Eq)` and `#derive(Hash)` for sum types** (already on the
   m12.8 follow-up list). Independently of this lane, that landing
   would let `ty_eq_shape` and friends become single-line derives —
   *if* the partial-comparison semantics (e.g. ignoring the effect row
   in `TyFnT`) were given up. They probably should not be given up
   without a separate semantic review.
3. **Auto-Show in string interpolation** (already on the m12.8
   follow-up list). Independent of this lane.

## Limitations of this report

- Self-report bias acknowledged: the same agent that audited and
  decided "nothing converts" wrote this report. A fresh agent
  re-auditing might find borderline cases I categorised as Category D
  that they would categorise as Category A.
- The audit took ~7 min of file walking; a more thorough pass might
  surface candidates I missed. The strong negative finding makes me
  comfortable with the audit's depth, but a future agent who finds a
  convertible case has not been contradicted by this report.
- Single agent (Claude Opus 4.7). Not generalisable across LLMs.
- No human review of the inventory; the categorisation rests on the
  agent's interpretation of "structural Show vs presentation logic".

## Build TSV (raw)

```
timestamp	cmd	outcome	elapsed_s
2026-04-26T20:04:14-04:00	all	OK	-
2026-04-26T20:04:30-04:00	selfhost	OK	-
2026-04-26T20:04:48-04:00	selfhost-llvm	OK	-
2026-04-26T20:06:23-04:00	test	OK	-
```
