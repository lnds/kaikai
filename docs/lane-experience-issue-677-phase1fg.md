# Lane experience — issue #677 Phase 1f/1g + parse rollback (2026-05-24)

## Scope as planned

Continue the per-module extraction of `stage2/main.kai`. The
handoff named the next two modules in dependency order:

- **ast** (Phase 1f) — shared AST data model. Stopped previously
  because of the `Ty ↔ Expr` mutual-recursion cycle; user
  authorised "Opción C" (bundle Expr + Ty into one module) to
  unblock.
- **intervals** (Phase 1g) — m12.6.x #2 integer interval lattice
  + post-typing propagation. Independent of typer/parser state.

And per the issue plan, **parse** (Phase 1h) was the next module.

## Scope as shipped

Phase 1f and 1g shipped. Phase 1h was attempted and rolled back.

### Phase 1f (`68aca46`) — `compiler/ast.kai`

464 lines of AST syntactic types + 69 lines of semantic Ty /
Label / Row / RVBind, plus the four AST constructor helpers
(mk_expr, with_ty, mk_pat, mk_ty). The two type families live
together because `Expr.ty: Option[Ty]` and `Ty` contains
`TyRefineT(Ty, Expr)` — kaikai's package resolver does not
permit cross-module import cycles (verified empirically with
a 3-file probe: `kaic: import cycle detected`), so the only
way to share both with downstream modules is to bundle them.

What stays in main.kai:
- Algorithmic helpers over Ty / Row (resolve_ty, row_of_expr,
  rvbind_lookup, mk_rvbinds, …) — they migrate into infer.kai
  when that module extracts.
- Parser-state and operational types (Parser, PExpr, Env, …).

Public surface: 35 top-level pub decls. Tests: 10 unit + 3
property checks, all green.

### Phase 1g (`893c3bb`) — `compiler/intervals.kai`

376 lines: the Interval / IntervalEnv / IntervalBinding types
plus ival_top / ival_bot / ival_const / ival_range / ival_show
/ ival_join / ival_intersect / ival_add / ival_sub / ival_mul /
ival_{lt,le,gt,ge,eq,ne} / ie_empty / ie_lookup / ie_extend /
ival_of_expr / ival_of_decl / ival_of_match / ival_of_block /
ival_thread_stmts / ival_seed_params / ival_bind_pat /
match_is_total / ival_int_min / ival_int_max.

What stays in main.kai:
- The Issue #83 sub-case 3 call-site substitution analysis. It
  uses interval results but composes them with the typer's
  signature inspection and refinement-discharge code.

Public surface: 37 top-level pub decls. Tests: 21 unit + 5
property checks, all green.

### Phase 1h attempt and rollback — `compiler/parse.kai`

Attempted to extract `parse` per the handoff order. The naive
"extract section between 'parser state' and the next big section
header" cut spanned ~7700 LOC from main.kai. The extraction:

- Created `stage2/compiler/parse.kai` with 426 pub decls.
- Added `import compiler.{chars,diag,lex,ast}` to parse.kai
  and `import compiler.parse` to main.kai.
- Removed the source range from main.kai (8k LOC delete).

Selfhost failed immediately with cross-module privacy errors.
Investigation: the "parse" range as I delimited it INCLUDED
glue paths that are not parse:

- `tp_make`, `tp_make_with_bounds`, `tp_*` (type-param
  mangling — typer machinery).
- `analyze_calls_in_*`, `analyze_program_calls_loop` (call-
  elision analysis — refinement-discharge module).
- `compile_or_panic`, `regex_compile_or_panic` (regex compiler
  — m12.6.x #7 module).
- `subst_self_list`, `subst_var_list` (typer substitution).
- `extern_handler`, `find_impure_call_args`,
  `collect_callee_sigs_loop` (call-site analysis).
- `mask_*`, `escape_str_body_for_c` (regex helpers + emitter
  bridge).
- `closest_name` (diag — already extracted; this is an
  IMPORT bug not a placement bug).
- `dump_call_elision_one` (driver — dumper).

The reverse direction is also tangled: main.kai's post-parse
declaration analyses (mono setup, refinement discharge, AST
dumpers, etc.) reach back into parse-state types like the
PXxx workspaces.

Rolled back to Phase 1g state (`893c3bb`). selfhost + tier1
back to green.

## Why parse is harder than the earlier modules

The first five extractions (chars / diag / lex / ast /
intervals) had a clean topological boundary: each was either
pure-leaf (chars), composable with one other module (diag /
lex on chars and ast), or operating on already-extracted
types with no reverse coupling (intervals on ast).

Parse is in the middle of the pipeline. The original section
header `# parser state` … `# program` reflects source-file
layout, NOT a module boundary. Several invariant-discharge
passes (m12.6.x refinement, m4c monomorph leak detection,
call-site analysis, type-param mangling) are co-located with
parse-section helpers because the original author put them
where the convenient type definitions sat — that is precisely
the kind of co-location issue #677 was filed to fix, but it
means the extraction must be more surgical than "delete this
contiguous range."

## Design decisions and alternatives considered

### Phase 1f: bundle Expr and Ty vs decoupling

asu / linus had not flagged Ty ↔ Expr as a likely cycle (the
handoff session predicted ciclos en protocols/infer y
resolve/modules). The user authorised Opción C ("bundle both
into ast.kai") with a clear "después vemos si migramos" exit
hatch. The pragmatic move was right: the alternative (Opción B:
decouple via opaque index for `Expr.ty`) would touch every
Ty-walker pattern-match in the codebase.

### Phase 1g: intervals as a standalone vs leaving with refinements

The interval lattice is a logically independent piece of
machinery — it has its own test surface and runs without the
typer's working state. The call-site substitution analysis
(Issue #83 sub-case 3) USES intervals but composes them with
parser TyRefine shapes and the discharge path; it belongs with
the refinement-discharge code, not with the lattice. Leaving
that boundary clean was the entire point of choosing
intervals as a separate module rather than rolling it into a
hypothetical `refinements.kai`.

### Phase 1h: rollback vs surgical-fix

Two options were on the table once selfhost broke:

A. **Surgical fix in-flight**: mark every cross-module fn pub
   in main.kai, import them into parse.kai, iterate until
   selfhost passes. Likely 30-60 minutes of plumbing.
B. **Rollback + handoff**: leave Phase 1h for a user decision
   on how to subdivide the parse range.

Option A would land *something* called `compiler/parse.kai`
but it would actually contain ~3000 lines of typer / monomorph
/ regex / emitter glue mis-named as "parser." Option B is the
discipline call: the module name is a contract that future
readers and contributors will trust. "compiler/parse.kai
contains the parser" is a contract worth keeping clean.

Chose B. Rollback is one commit-less revert; no public-history
artefact.

## Structural surprises the brief did not anticipate

### Mass-`pub` was the right move on extraction; un-pub is a separate audit

For chars (7 pubs) the up-front pub markup was overkill — the
typer would have flagged exactly the 7 the user calls. For
diag (21 pubs after probing) the probe found 20+1 (abs_int
hidden). For lex (42 pubs) and ast (35 pubs) and intervals
(37 pubs), mass-pub-on-extraction is faster than iterative
probing. The un-pub audit becomes a follow-up lane per
module — none of those have run yet.

### intervals.kai header opening `# ====` was lost in the sed cut

The original `# =====` header line that opened "Integer
interval lattice" was at the line immediately before the cut
range (line 6052 in pre-1g main.kai). Including it would have
required either widening the sed range (and risking absorbing
trailing blank lines from the previous section) or post-cut
patching. Phase 1g chose post-cut patching — added the missing
`# ====` opener after the import line. Cosmetic only.

### Variant name memory drift on test-writing

For ast.kai tests, the first draft used `EString` (not `EStr`)
and `PWildcard` (not `PWild`). The typer's "available exports"
list in the error message is exhaustive, so the fix was a
one-line sed. Test files should grep the exports table once
before writing pattern names — cheaper than the round-trip.

## Fixtures added and coverage gaps

Per-module test files for each landed extraction:

- `tests/test_ast.kai` (10 unit + 3 property) — covers the
  four AST constructor helpers and the EInt / EReal / EBool /
  EStr literal carriers + Ty primitives + Row construction.
- `tests/test_intervals.kai` (21 unit + 5 property) — covers
  the pure lattice ops (top / bot / const / range / join /
  intersect / arithmetic / comparisons / env helpers). The
  AST-walking entry points (ival_of_expr, …) are exercised
  implicitly during every selfhost.

Coverage gaps left intentionally:

- The propagation walker entry points in intervals.kai are
  not unit-tested. They need a typed Expr to exercise, and
  setting one up by hand duplicates the parser harness. Better
  to rely on the implicit selfhost coverage until a Tier 2
  cross-cut test harness lands.
- ast.kai's Decl / Stmt variants are not enumerated — same
  reason; they materialise via the parser.
- No bench probes yet. Still flagged as future work.

## Real cost vs estimate

- Phase 1f (ast extraction + tests): ~90 minutes. The cycle
  decision was the biggest chunk; the extraction itself was
  20 minutes once the boundary was settled.
- Phase 1g (intervals extraction + tests): ~60 minutes,
  routine after Phase 1f.
- Phase 1h attempt + rollback: ~30 minutes. The lesson cost
  ~25 minutes of wasted plumbing offset by certainty about
  what the rest of `parse` extraction needs.

## Follow-ups for the next lanes

### Parse needs an up-front cross-section analysis

Before another attempt at compiler/parse.kai, someone should
run a grep cross-section over the candidate range, building a
dependency graph of EVERY helper called inside and a reverse
graph of every callsite reaching IN to the range from outside.
The output would identify three groups:

1. **Pure parse helpers** — only called by other parse fns
   and from main.kai's program entry. Safe to move.
2. **Typer / monomorph / regex co-locators** — called by the
   parse range but the callee belongs elsewhere. Move them
   FIRST to their home module (`infer.kai`, `refinements.kai`,
   etc.).
3. **Glue paths** — fns that legitimately bridge two phases
   (parse → desugar, parse → analysis). Stay in main.kai
   until both endpoints extract.

Without this analysis the extraction either explodes parse.kai
with foreign code (option A above) or misses ~30% of the parser
because it lives under another section header.

### `closest_name` import bug in parse.kai

The Phase 1h attempt failed partly because parse called
`closest_name` (already in compiler.diag). That's a code
quality issue independent of the parse module: any helper
already-extracted should be qualified or imported directly,
not referenced bare via the bundle. A grep over main.kai for
the names from each already-extracted module would surface
these — possibly worth a small "wire up diag/lex/ast/intervals
calls properly in main.kai" lane before the next big extraction.

### Pre-existing duplicate `# ====` header in main.kai

main.kai now has a leftover `# ====` block at line 115/116
before "parser state" — artifact of an earlier extraction.
Cosmetic; doesn't affect compilation. Can fold into the next
real touch of that region.

### Un-pub audit deferred

Five modules now have mass-pub surfaces. An un-pub audit per
module (remove pub from helpers that turn out to be internal)
is a small lane each, valuable for the long-term public-API
discipline. Sequenced after the structural extractions.

### CI runner shaka (macOS m2) offline

Throughout this session, GitHub Actions `tier1` runs queued
indefinitely because the self-hosted shaka runner is offline.
User confirmed they are restoring it; this retro flags it so
the next session knows tier1 CI lights up post-restore. Local
`make tier1` is the authoritative gate for now.
