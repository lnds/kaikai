# Lane experience — issue #1166 (curiosity tax: reference granularity for LLM consumers)

## Scope as planned vs. as shipped

The brief asked for all four issue suggestions in one lane, and all
four shipped:

1. **Symbol-sized answers as the advertised default** — `docs/info/llm.md`
   rewritten around "the cheap path" (`kai doc module.symbol` first,
   `deltas` once, `syntax` per unfamiliar form, topic pages as the
   fallback). Steering lines added to `kai info` no-args output and
   `kai info -h`.
2. **Prior-collision cheat card** — new `kai info deltas` topic
   (`docs/info/deltas.md`). A standalone topic won over a section at
   the top of `llm` because it is discoverable from the topic list
   and `-k` search, and it keeps `llm` a bootstrap doc rather than a
   mixed bag.
3. **TL;DR discipline + JSON field selection** —
   `kai info <topic> --json --section <name>` (repeatable,
   case-insensitive, unknown names error listing what exists;
   `--section` without `--json` is rejected; no flag = full object).
   On TL;DR reordering: an audit of all 19 pre-existing topic pages
   found every one already opens with a tagline plus a Description
   that answers the common question — no reordering was needed, so
   none was done. The two pages this lane touched (`llm`, `holes`)
   now front-load the cheap path explicitly.
4. **Bounded `--holes`** — `--holes` / `--holes-json` now report the
   hole's local bindings plus an elided count (`scope_elided` in
   JSON); `--holes-scope` / `--holes-json-scope` keep the full dump.
   One-hole report: 58 KB → ~600 bytes (~100x).

## Design decisions and alternatives

- **Locals boundary via `TypedProgram.toplevel_len`.** A hole's
  captured scope is `[locals…, top-levels…, builtin seed]`
  (cons-front). The existing `prelude_len` only covers the builtin
  seed (drops ~30 of ~900 entries — why the "user scope" report was
  still 58 KB). The clean cut is the length of the env a root-module
  decl body starts from; `infer_all_loop` computes it for free
  (`list_length(env.entries)` in its base case) and the modular fold
  keeps the last module's value, since `partition_decls_by_home`
  types the target file last. Alternatives considered: threading a
  `base_len` through `InferState` (touches ~15 record-copy helpers —
  rejected as noise) and filtering by top-level name sets (wrong
  under shadowing).
- **`--holes-json` migrated, not frozen.** The issue's data
  falsified the schema comment's premise ("an LLM wants the full
  picture"), so the default changed too: `in_scope` = locals,
  new `scope_elided` field, full dump behind `--holes-json-scope`.
  The only in-repo consumer (`kai lsp`) reads position/name/
  expected_type and explicitly ignores `in_scope`, so the migration
  is safe; `validate_holes_json.py` now requires `scope_elided`.
- **`--section`, not `--fields`.** The JSON object's only plural
  axis is `sections`; a generic field selector would have one real
  use. Section names match H2 headers case-insensitively.
- **`deltas` has no code blocks on purpose.** The card is one dense
  read; compilable blocks would triple its token cost and add
  test-info-blocks surface for no verification gain — every claim
  was instead verified against the live compiler while writing (see
  below).

## Structural surprises

- **The issue's own delta list had a falsehood.** "no `++`" is wrong:
  `++` exists as right-associative concat (`[1] ++ [2]`,
  `"a" ++ "b"`); what does not exist is increment `i++`. Likewise
  "pipes vs iterators" understates UFCS — `xs.map(f)` is legal sugar
  for `map(xs, f)`. Both entries were verified empirically and
  written accordingly; the brief's "a cheat card that lies is worse
  than none" was load-bearing in practice.
- **The `kai info --json` awk emitter had two latent bugs**, found
  because piece 3 touched it: every section line was emitted twice
  (a second pattern-less awk rule also appended to the buffer), and
  any non-ASCII byte came out as the NUL escape (the `ord` table only
  covers ASCII, so em-dashes were destroyed). Both fixed in the same
  commit; every topic's `--json` now round-trips through
  `json.load`. Nobody had noticed — a hint that the JSON surface had
  no real consumers at full-page granularity.
- **A pre-existing C-backend miscompile surfaced** in
  `dump_hole_report`: an effectful `if` (no `else`) as the final
  statement of a match-arm block had its then-body emitted twice —
  once inside the conditional and once unconditionally after it
  (visible in the generated C). Minimal standalone repros (4
  variants) did not reproduce; the trigger needs more of the real
  shape. Worked around by using `match elided { 0 -> () _ -> … } `
  instead. Left unfixed here (out of lane); the generated-C evidence
  is in this retro for a follow-up issue. Related trap hit on the
  way: `()` on the line after an `if` block parses as a CALL of the
  if's value — a comment line between them breaks the fusion.
- **`kai info -k '++'` crashes** (keyword is interpolated raw into an
  awk regex). Pre-existing, out of lane, noting it here.

## Fixtures and coverage

- `tools/test-info.sh` (tier 1 via `make test-info`): `--section`
  narrowing, unknown-section error, `--section` without `--json`
  rejected, `deltas` existence.
- `stage2/Makefile test-driver-holes` (tier 1 via test-fast):
  bounded default (`in scope (local)`, no `Spawn.spawn` leak,
  elided-count line), `--holes-scope` full dump,
  `--holes-json` `scope_elided > 0` with small `in_scope`,
  `--holes-json-scope` schema-valid with `scope_elided == 0`.
- `test-info-blocks` green (112/112) — `llm.md`'s updated blocks
  compile; `deltas.md` adds none.
- `make tier0` green (selfhost deterministic).

## Cost vs. estimate

Docs pieces (1–3a) were as cheap as briefed. Piece 3b grew by the
two awk bug fixes (worth it — the JSON was silently 2x its own size
and lossy). Piece 4 was the real work and its cost was dominated by
two discoveries: `infer_program` routes through the modular fold
(the first `toplevel_len` landed in dead code returning 0), and the
if-statement miscompile (three rebuild-and-inspect cycles of the
generated C to characterise).

## Follow-ups left for next lanes

- File the `if`-without-`else` double-emission miscompile with the
  generated-C evidence (needs maintainer go-ahead to open the issue).
- File the `kai info -k <regex-metachar>` awk crash.
- The authorability bench (separate repo, kaikai-authorability-bench)
  can re-run its `no-info` ablation grid against these changes to
  measure the curiosity-tax delta — that measurement is now
  available to whoever owns the bench.
- `--section` could also work for plain-text output (print just the
  markdown section); deliberately not built until someone wants it.
