# Lane experience — issue #677 Phase 1k (cli + driver: analysis only)

Outcome: **analysis-only PR, no extraction.** The cross-section analysis
concluded the `# cli + driver` section is not safely extractable into a
single `compiler/driver.kai` in this lane, for the same class of reason
that made parse phase 1h ship analysis-only (PR #684). The full reasoning
is in `docs/lane-driver-cross-section-analysis.md`; this retro records the
lane meta.

## Scope as planned

Extract the `# cli + driver` section (~7 450 LOC, lines ~51794–EOF) into
`compiler/driver.kai` with docs + `test_driver.kai` + Makefile +
`main.kai` import, mirroring the resolve/parse/fmt sibling lanes. The
brief flagged the section as "grande y heterogénea" and explicitly
authorized an analysis-only outcome if the cross-section work showed the
cut was unsafe — citing the parse-analysis precedent. It also named the
suspected sub-sections (core cli, library mode #454, cache #592, AST
serde #452).

## Scope as shipped

- `docs/lane-driver-cross-section-analysis.md` — the extractability
  analysis with verified dependency tables and a recommended pivot.
- `docs/lane-experience-issue-677-phase1k.md` — this retro.
- **No code moved.** No `compiler/driver.kai`, no Makefile change, no
  `main.kai` import line, no `test_driver.kai`. The branch is otherwise
  identical to `origin/main` @ 7c8f893.

The doc/test discipline (`#[doc]` on every pub, ≥5 unit + ≥2 property
checks per module) did not apply because nothing was extracted. It will
apply in full to whichever code lane the integrator schedules next.

## Cross-section analysis (the load-bearing finding)

Two findings drove the verdict:

1. **The driver is the pipeline apex, not a leaf.** It calls ~143 symbols
   still in `main.kai` — typer, resolver, emitter, every desugar,
   perceus/unbox, protocol lowering, validators, dumps — while only **7**
   of its own symbols are referenced from outside. Extracting it inverts
   the natural dependency arrow: `main.kai` would have to `pub`-export
   ~140 functions to a sibling. The driver must be extracted *last*,
   after the stages beneath it, not as one of "the 3 easy ones".

2. **A genuinely clean cut exists inside the range:** the cache codec +
   AST serde (`Cache*Pos` types + `cache_*_to_hex`/`cache_hex_to_*`,
   ~3 360 LOC) has *zero* inbound references and depends only on AST
   types + itself. It extracts into `compiler/cache.kai` with a
   2-function pub surface, if the four prelude-glue bridge fns stay in
   `main.kai` and call the codec's public API.

The five sub-sections (A core cli / B library mode / C `compile_source` /
D cache codec / E serde) are mutually recursive (A→E, C→E, E→A), so the
block also cannot be sub-split along its own section headers — only the
codec core is severable, precisely because it sits *beside* the pipeline
rather than above it.

## Design decisions

- **Analysis-only over a forced shell.** A `compiler/driver.kai` that
  `pub`-imports 140 symbols would compile and pass selfhost, but it
  inverts the modularization invariant (a module imports what it sits
  above). Shipping it would record a false "phase 1k done" while making
  the eventual real extraction harder. Following the parse precedent, the
  lane defers and documents.
- **Surfaced `compiler/cache.kai` as the recommended pivot** rather than
  silently reporting "can't extract". The codec cut is real, small, and a
  better fit for the "easy" tier than the driver ever was — leaving the
  integrator a concrete, actionable phase-1k alternative.
- **Flagged `BuildMode` / `PreludeSegment` as misplaced types.** They sit
  in the driver but are consumed by the emitter and pub-access passes;
  they belong in `compiler.ast`. Noted as a sequencing follow-up, not
  fixed here (out of lane).

## Structural surprises

- **The brief's line numbers were ~994 stale.** They predated the
  resolve-extract merge (#685). Rebasing onto `origin/main` first (per the
  lane-rebase discipline) was essential — the section header moved from
  52788 to 51794. The topology was identical, so the analysis held, but a
  lane that trusted the brief's absolute numbers without rebasing would
  have sliced the wrong ranges.
- **A sub-agent that `cd`'d to the wrong worktree** (the main checkout,
  not `kaikai.driver-extract`) initially reported pre-rebase line numbers.
  Caught by re-verifying every claim against the correct tree before
  trusting it. Lesson: when delegating grep-heavy analysis across
  worktrees, pin the absolute path and re-verify the load-bearing numbers
  locally.
- **`fn main()` is inside the range** (line 59241), at the tail of the
  serde sub-section, not in a separate region. It stays in `main.kai`
  regardless of any cut.

## Fixtures / coverage

None added — no code moved, so there is no new behavior to fixture. The
cache-codec lane (if scheduled) will add `stage2/tests/test_cache.kai`
exercising `cache_serialize_module` / `cache_deserialize_module`
roundtrips on representative AST shapes (the `Cache*Pos` machinery is
exactly the kind of thing property checks suit: `decls == deserialize(serialize(decls)).decls`).

## Cost vs estimate

Estimated as a large extraction lane (the brief budgeted "más commits OK
porque el lane es grande"). Actual: a focused analysis day — one thorough
callee-graph pass (delegated + locally re-verified), boundary
confirmation, and two docs. Much cheaper than the planned extraction
because the verdict was "don't", reached early. The right outcome: a
forced extraction would have cost far more in churn and in the eventual
re-work to undo the inverted dependency.

## Follow-ups for next lanes

- **Recommended phase-1k pivot:** extract `compiler/cache.kai` (codec +
  serde, ~3 360 LOC). Clean, self-contained, 2-fn pub surface. Leave the
  four bridge fns (`cache_serialize_module`, `cache_deserialize_module`,
  `cache_roundtrip_self_test`, `emit_prelude_cache_for`) in `main.kai`
  calling the public API.
- **fmt lane (parallel):** must export `fmt_program` and
  `collect_comments` as `pub` — the driver's only fmt dependency. This
  holds regardless of which phase-1k option the integrator picks.
- **Driver extraction proper:** sequence it *after* the typer / emitter /
  resolver-of-the-rest / desugar lanes land, so it imports modules not
  bodies. Its eventual pub surface is the 5 remaining inbound symbols
  (`ty_name_zero`, `edition_at_least`, `builtin_effect_names`,
  `prelude_module_name`, `tag_decls_module_origin`).
- **Type relocation:** lift `BuildMode` and `PreludeSegment` into
  `compiler.ast` in whatever lane touches the emitter, to shrink the
  driver's eventual pub surface.
