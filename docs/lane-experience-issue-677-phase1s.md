# Lane experience — issue #677 phase 1s (cli + driver extraction; FINAL phase-1 lane)

**Closes phase 1 of issue #677.** This lane extracted the CLI + driver
apex — the last block in `stage2/main.kai` — into `compiler/driver.kai`,
leaving `main.kai` with nothing but the package header and `fn main()`.
With it, the stage-2 compiler is fully modularized: 24 `compiler/*.kai`
modules, a 33-line entry point.

## Scope: planned vs shipped

**Planned (brief):** move the `# cli + driver` block (lines ~1097–5200,
~4100 LOC) into `compiler/driver.kai`; main.kai keeps `fn main()` +
minimal orchestration (~200–500 LOC).

**Shipped:** moved lines **45–5195** (~5150 LOC) into
`compiler/driver.kai`; main.kai keeps **33 LOC** (header + one import +
`fn main()`). The block moved is ~1050 LOC larger than the literal brief
range because the cross-section scan found three orphan clusters between
the import manifest and the `# cli + driver` header that are consumed
*only* by the driver:

1. **dump/report cluster** (45–782): `dump_tokens`, `report_errors`,
   `report_mono_leaks`, the whole `dump_*` AST-dumper family
   (`dump_expr`/`dump_decl`/`dump_type`/…), and the display helpers
   (`type_expr_display`, `unit_expr_display`, `row_label_*`). Every one
   is called only from the driver block or recursively within the
   cluster. No extracted module calls them — emit_c.kai even shipped
   *mirrors* of `type_expr_display`/`unit_expr_display` in phase 1r with
   the literal note "reconcile in the driver lane." (See *Mirror
   residue* below — the brief's "do not touch emit_c" constraint means
   the reconcile is deferred, not done here.)
2. **resolver residue** (746–763): `row_effects_without_default_handler`
   and `collect_type_names`, left behind in phase 1i because they are
   effect-injection logic merely co-located with the resolver; both feed
   `inject_builtin_effects` in the driver.
3. **module-import resolution** (783–1096): `ResolveState`,
   `resolve_module`, `process_imports`, `expand_imports`,
   `collect_pub_exports` and friends — the machinery `compile_source` /
   `run` drive. A header comment at line 780 already grouped this with
   "the cli + driver block."

The brief itself flagged the test: *"if main.kai ends > 500 LOC, report
— something stayed behind."* Moving only the literal range would have
stranded ~1100 LOC of driver-only orphans in main.kai. Moving 45–5195
gives the right close: main.kai at 33 LOC. The integrator confirmed the
wider scope before the cut.

## Design decisions

- **driver.kai imports 21 of the 22 sibling modules; only `intervals`
  is unused.** This is exactly what the cross-section analysis (PR #687,
  `docs/lane-driver-cross-section-analysis.md`) predicted: the driver is
  the apex of the pipeline, so it imports nearly everything. An Explore
  agent verified per-module that the block references at least one `pub`
  symbol of each — `intervals` had zero hits (its `ival_*`/`Interval*`
  surface is consumed by infer/refinements, not the driver).

- **main.kai imports only `compiler.driver`.** Once the driver owns the
  whole orchestrator, `fn main()` only needs `run` + `parse_cli`. The
  21-module manifest moved into driver.kai; main.kai's manifest shrank
  to a single line. kaic2 resolves the rest transitively through
  driver.kai's imports; kaic1 ignores import lines and rides
  `BUNDLE_SRCS` order.

- **Pub surface is exactly 4 symbols:** `parse_cli`, `run` (the two
  entry points `fn main()` calls) plus `Mode` and `CliOptions` (the
  types that flow between them). The pub-access validator forced the two
  types: `parse_cli`/`run` expose `CliOptions`, and `CliOptions` exposes
  `Mode`, so both had to be `pub`. This matches the analysis's
  prediction of a "small and stable" surface — and is one of the
  *smallest* pub surfaces of the whole effort (emit_c shipped ~10).

- **Zero downward-glue mirrors.** The driver calls *down* into the 21
  modules (which it imports) and *nothing* up into main.kai (which holds
  only `fn main()`, and `fn main()` calls down into the driver). This is
  the cleanest topology of the effort — the opposite of the early infer
  lane, which needed mirrors because the flat bundle hid upward refs.
  The apex has no upward refs by construction.

## Structural surprises the brief did not anticipate

- **`BuildMode` and `dump_types` already left for emit_c.** The original
  analysis (#687 §2) flagged `BuildMode` as a driver-defined type
  consumed by the emitter, and recommended lifting it to `ast`. By the
  time this lane ran, phase 1r had already moved `BuildMode` *and*
  `dump_types` into emit_c.kai (where the emitter that consumes them
  lives). So the driver does not define them — it *imports* them from
  emit_c. The analysis's "latent design smell" resolved itself naturally
  as the emitter extracted. One fewer pub export than predicted.

- **The orphan clusters were not in the briefed range.** The brief's
  line numbers (1097–5200) were the `# cli + driver` header to EOF. But
  ~1050 LOC of driver-only code sat *above* that header — leftovers from
  phases 1d/1h/1i that were co-located with, but not part of, the
  sections those lanes extracted. The cross-section scan (grep each
  block-defined symbol for external callers; grep each cluster for
  consumers outside the block) is what surfaced them. Lesson reinforced:
  the briefed range is a starting hypothesis, not the boundary — the
  scan defines the boundary.

## Mirror residue (deferred, not done)

emit_c.kai (phase 1r) holds private mirrors of `type_expr_display`
(line 9943) and `unit_expr_display` (9967), tagged "reconcile in the
driver lane." The canonical definitions now live in driver.kai. Because
the brief restricts this lane to `driver.kai` + `main.kai` + `Makefile`,
the emit_c mirrors are left in place — they are private copies, not
broken refs, so selfhost is byte-identical. **Follow-up for phase 2:**
delete the two emit_c mirrors and have emit_c import the display helpers
once the dependency direction is settled (emit_c is *below* driver, so
it cannot import driver — the helpers may need to sink to `emit_shared`
or `diag` instead; that is a design call, not a mechanical move).

## Fixtures added + coverage

`stage2/tests/test_driver.kai`: **11 unit tests + 3 property checks**,
all green (`kaic2 --test` / `--prop-check`, 100 iter each).

- Unit: `parse_cli` end-to-end — empty argv → MDefault; bare arg → path;
  `--test`/`--emit=llvm`/`--fmt`/`--fmt-check`/`--library-mode` mode
  selection; `--strict-holes` flag; `--path`/`--package-name`/`--edition`
  argument consumption; `Mode` + `CliOptions` constructibility.
- Property (body Bool, Bool generators per the phase-1n String-generator
  caveat): a bare path keeps MDefault; `--strict-holes` is idempotent;
  `CliOptions.strict_holes` round-trips a generated Bool.

Coverage rides `parse_cli` (pub) end-to-end so the private
`parse_cli_loop` flag walk + `cli_with_*` builders are exercised
transitively — the same approach as test_emit_c.kai for `emit_program`.
The heavy orchestrators (`run`, `compile_source`, library probes) are
covered by selfhost + tier1 end-to-end; pinning them in a unit test
would re-implement the pipeline.

**Coverage gap:** the dump/report cluster and the import-resolution
machinery now live in driver.kai but are exercised only transitively
(through selfhost + tier1, not test_driver.kai). A future lane could add
focused tests for `expand_imports` / the `dump_*` family if they ever
need to evolve independently of the driver.

## Cost vs estimate

Estimate (brief): a clean lane, "should be clean because all layers
driver orchestrated are extracted." Actual: clean as predicted. The only
non-trivial work was the scope decision (literal range vs the wider
driver-only block) and the three pub-access iterations
(`CliOptions` → `Mode` cascade). Selfhost was byte-identical on the
first green build. No mirrors, no upward-ref surprises — the apex
extracts cleanly *because* it sits on top, exactly as the sequencing
argument in #687 said it would.

## The full phase-1 effort (issue #677)

main.kai: **70179 LOC (post package-flip, phase 1c) → 33 LOC.** 24
modules in `compiler/`, ~76000 LOC total:

| # | Module | Extracted in |
|---|--------|--------------|
| 1 | chars | 1b |
| 2 | diag | 1d |
| 3 | lex | 1e |
| 4 | ast | 1f/1g |
| 5 | util | 1f/1g |
| 6 | parse | 1h |
| 7 | resolve | 1i |
| 8 | fmt | 1j |
| 9 | intervals | 1l |
| 10 | refinements | 1l/1m |
| 11 | cache | 1k |
| 12 | modules | 1k |
| 13 | desugar | 1m |
| 14 | infer | 1n1 |
| 15 | fnreg | 1n3 |
| 16 | unbox | 1n3 |
| 17 | perceus | 1n4 |
| 18 | protos | 1p |
| 19 | monomorph | 1n2 |
| 20 | emit_shared | 1r |
| 21 | emit_llvm | 1q |
| 22 | emit_c | 1r |
| 23 | (driver) | **1s (this lane)** |
| 24 | (main.kai entry) | — |

(Module count vs lane letters is not 1:1 — some lanes shipped two
modules, e.g. 1f/1g landed ast + util together, 1n3 landed fnreg +
unbox, 1r landed emit_shared + emit_c.)

### Lessons applied across the effort

- **Move-vs-mirror = sole-block-caller test** (1n1). A symbol moves when
  every caller is inside the block; it stays + gets mirrored when an
  out-of-range caller remains. The driver lane is the limit case: every
  block symbol is driver-internal, so *everything* moved, *nothing*
  mirrored.
- **The flat bundle hides upward refs** (1n1/1n3/1n4). kaic1 concatenates
  `BUNDLE_SRCS`, so a missing import compiles fine in the bundle but
  fails in modular kaic2. Every lane verified with the modular build
  (`make selfhost`) + a standalone test file, not just `make kaic2`.
  This lane is the one where it could not bite — the apex has no upward
  refs.
- **AST-sink rule** (desugar 1m): a shared type in a public signature
  spanning ≥2 layers sinks to `compiler.ast`. The driver did not trigger
  it — `Mode`/`CliOptions` are driver-private types that only `fn main()`
  touches, so they stay in driver.kai.
- **Pre-sink shared infra** (fnreg 1n3, emit_shared 1r): when N modules
  need the same infrastructure, sink it to a dedicated module before the
  consumers extract. The driver needed no pre-sink — it consumes the
  pre-sinks (emit_shared) that earlier lanes created.
- **Read layer extent from section headers, not brief LOC** (1n2/1r).
  The briefed range is a hypothesis; the cross-section scan + section
  headers define the real boundary. This lane is the clearest example:
  the boundary was 1050 LOC wider than the brief, and the scan proved it.
- **`make kaic2` hides privacy; only selfhost catches it** (1n2/1r). The
  bundle ignores `pub`; modular kaic2 enforces the pub-access validator.
  This lane's three pub iterations (`CliOptions` → `Mode`) came from
  selfhost, not from the bundle build.
- **Module name must not collide with a stdlib prelude module** (#698).
  `driver` is safe — no `stdlib/*.kai` named `driver`. (Verified.)

## Follow-ups for phase 2 / beyond

1. **Reconcile the emit_c display mirrors** (see *Mirror residue*).
   Delete `type_expr_display`/`unit_expr_display` mirrors in emit_c.kai;
   decide where the canonical helpers live (emit_shared or diag — they
   cannot import driver, which sits above them).
2. **`.kaii` interfaces (phase 2).** Each module gets an explicit
   interface file pinning its pub surface; driver.kai's is trivial
   (`parse_cli`, `run`, `Mode`, `CliOptions`).
3. **AST-functor family sink (optional).** The `dump_*` and `find_*`
   walkers in driver.kai re-implement the same expr/decl/pattern descent
   that infer/monomorph/perceus also re-implement. If a generic AST
   functor is ever introduced, these are candidates to collapse — out of
   scope for phase 1, a real design lane for later.
4. **Focused tests for the import-resolution + dump clusters** if they
   need to evolve independently (see *Coverage gap*).

## Verification

- `make selfhost` → `kaic2b.c == kaic2c.c` (byte-identical). The
  critical gate: the driver is the last thing extracted, so a mismatch
  would mean a lost orchestrator branch. Identical on first green build.
- `make tier0` → green (31 OK, selfhost deterministic, demos baseline 34
  holds).
- `make tier1` → green end-to-end (run locally before push).
- `kaic2 --test --path . tests/test_driver.kai` → 11/11.
- `kaic2 --prop-check --path . tests/test_driver.kai` → 3/3 (100 iter).
