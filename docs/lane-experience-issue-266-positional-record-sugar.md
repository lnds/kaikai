# lane retrospective — issue #266 (positional record construction sugar)

Wave 4 of plan A: parser + small pre-typer desugar pass that lowers
`T { v1, v2 }` to `T { f1: v1, f2: v2 }` in declaration order.

## Objective metrics

| metric | value |
| --- | --- |
| start | 2026-05-05T00:38-04:00 |
| end | 2026-05-05T01:09-04:00 |
| wall | ~30 min (including one redo after a stash mishap restored work) |
| compiler.kai delta | +~410 lines (parser dispatch + helpers + desugar walker + diagnostics) |
| docs delta | +~80 lines (`docs/syntax-sugars.md` §8) |
| fixtures added | 3 (`positional_record_basic`, `positional_record_arity`, `positional_record_mixed`) |
| tier gates green | tier0, tier1, tier1-asan, selfhost, selfhost-llvm |

## Diagnosis

The named-field record literal lives in
`stage2/compiler.kai:parse_record_lit` (~3148) and recurses through
`parse_record_fields_loop`. That loop already encoded m7d §10's
"`{ x, y }` punning" branch alongside the canonical `IDENT : Expr`
branch, so the dispatch surface lives at the very top of the loop.

The typer side reads field types via `synth_record_lit` (~26305) and
the record registry (`RecInfo` / `rec_find`, ~22639). The registry
is keyed by name and carries the original `[FieldDecl]`, which is
exactly what the desugar pass needs to look up declared field order.

## Algorithm

1. **Parser dispatch.** After consuming `{`, peek the first item:
   - `IDENT (: | , | })` → existing named/punning path.
   - anything else → positional path (`parse_record_pos_loop`).

2. **Positional path.** Each item is a full `parse_expr`; values are
   tagged with sentinel field names `__pos_<i>__` so the existing
   `ERecordLit` AST node carries the data without growing a new
   variant. If a subsequent item starts `IDENT :`, the parser
   immediately rejects with "expected positional or all-named record
   fields, found mixed forms".

3. **Pre-typer desugar pass.** A new `desugar_pos_records_decls`
   (placed right after `expand_aliases_in_decls` in `compile_source`)
   walks every decl body, looks up each `ERecordLit` whose first
   field is sentinel-tagged, validates against the record registry,
   and rewrites the FieldInits with real names in declaration order.
   Two diagnostics:
   - unknown record name
   - arity mismatch
   The pass returns `{decls, errs}`; on `errs > 0` the compile
   short-circuits before any later invariant walker runs.

The desugar walker is structural — it owns the recursion (rather
than reusing `map_expr_kind`) so it can co-thread an error count
under the `Console` effect.

## Empirical verification

```kai
fn main() : Unit / Stdout {
  let z = Complex { 1.0, 2.0 }
  Stdout.print(real_to_string(z.re))
  Stdout.print(real_to_string(z.im))
}
```

Output:
```
1
2
```

(`real_to_string` strips trailing zeros — same shape the
`mini_mandel` demo uses today.)

The fixtures cover the three acceptance cases verbatim:

- `positional_record_basic.kai` — `Complex { 1.0, 2.0 }` and
  `Point { 3, 4 }` plus the named form side-by-side.
- `positional_record_arity.err.kai` — `Complex { 1.0 }` rejected
  with "expects 2 positional fields, got 1".
- `positional_record_mixed.err.kai` — `Complex { 1.0, im: 2.0 }`
  rejected with the parser-level mixed-form diagnostic.

## Friction points

- **Stash mishap.** A mid-lane `git stash && git stash pop` round
  silently dropped the in-flight `stage2/compiler.kai` edits — git
  reflog showed three `reset: moving to HEAD` entries with no
  intervening commit, suggesting an external hook fired during the
  detour. Lesson: avoid `git stash` while mid-implementation; commit
  WIP locally before sidestepping into a make-clean rebuild for
  baselining.
- **Punning vs positional.** The existing m7d §10 punning form
  `T { x, y }` (bare ident, no value) overlaps the issue's
  "anything not `IDENT :` is positional" rule. The implementation
  carves out a `IDENT (, | })` exception that keeps punning intact;
  positional kicks in only when the first item is a literal,
  expression, or an ident followed by an expression-continuation
  token. No fixture broke under that rule.
- **`mini_ledger` demo.** Pre-existing failure ("expected
  expression"); reproduced on baseline (`git stash`) so unrelated
  to this lane. Demos baseline (26 passing) holds.
- **Selfhost convergence.** Byte-identical on both backends on the
  first attempt — the parser+pass shape was small enough to land
  cleanly without rippling into stage2's own AST construction.

## Subjective summary

Smallest of plan A's waves so far. The dispatch is one peek-2
lookahead; the rewrite pass mirrors the n-tuple sugar (#154) shape;
the diagnostics fall out of the existing `Console`-effect pattern.
The win is purely ergonomic — `Complex { 1.0, 2.0 }` reads better
than `Complex { re: 1.0, im: 2.0 }` for the common case where field
order is the call site's whole point.

## Limitations

- **No partial positional with `..rest`.** Out of scope per the
  issue body; revisit alongside an n-tuple spread design.
- **No function-style `T(v1, v2)`.** Conflicts with the type-ctor
  model — explicitly rejected in the issue body.
- **Sentinel name collision.** A user-declared field literally
  named `__pos_0__` would round-trip through the desugar as
  positional. Real records cannot start with `__` by repo
  convention, so this is a theoretical hole, not a practical one.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-05T00:48:24-04:00	tier0	OK	-
2026-05-05T00:55:11-04:00	tier1	OK	-
2026-05-05T01:06:48-04:00	tier0	OK	-
2026-05-05T01:06:48-04:00	tier1	OK	-
2026-05-05T01:07:47-04:00	tier1-asan	OK	-
2026-05-05T01:08:24-04:00	selfhost	OK	-
2026-05-05T01:09:22-04:00	selfhost-llvm	OK	-
```
