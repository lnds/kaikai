# Lane experience — issue #681 Phase 1: `#[doc("...")]` attribute

Branch `doc-attr-681`. Scope: parser + AST + JSON exposure + stdlib
migration (Phase 1 only; Phase 2 `kai doc` and Phase 3 `kai info`/LSP
belong to later lanes).

## Scope as planned vs as shipped

Planned (issue #681 Phase 1 bullet list): attribute whitelist arm,
`doc` slot on the six Decl variants, `module_doc` on TypedProgram,
verbatim string capture, fmt byte-identity, `kai info --json` +
`--holes-json` exposure, stdlib migration.

Shipped, with two deliberate deviations:

1. **Wrapper variants instead of per-variant `doc` slots.** The issue
   (written pre-#677-modularisation) asked for `doc: Option[String]`
   on DFn/DType/DAxiom/DEffect/DProtocol/DConst. Those are positional
   ADTs with ~450 construction/match sites across the F-grade
   monoliths (infer 18K LOC, emit_c 13K, driver 5K). Adding a slot to
   each variant would have been a 450-site mechanical edit through
   exactly the files this lane was told not to grow. Instead `#[doc]`
   follows the `DUnstable`/`DDerive` precedent: a `DDoc(raw, text,
   inner, l, c)` wrapper plus a standalone `DModuleDoc(raw, text, l,
   c)` carrier, both stripped right after parse into a `DocItem` side
   table owned by the new `compiler/doc_attr.kai`. Equivalent data
   model, ~20 wiring lines in existing files instead of ~450 edits.
2. **`kai info builtins` had to be invented.** The acceptance
   criterion names `bin/kai info builtins --json`, but no `builtins`
   topic existed — `kai info` is a static reader over `docs/info/*.md`.
   The lane added a `--builtins-doc-json` file-less mode to kaic2
   (loops the hard-coded core module set, parses fresh, renders via
   doc_attr.kai) and a dynamic-topic special case in `bin/kai
   cmd_info`. Plain `kai info builtins` prints a per-module
   documented/total summary; `--json` is the canonical surface.

## Design decisions and alternatives considered

- **Strip-early, side-table.** Doc wrappers never travel past the
  formatter: `compile_source` strips them right after parse (the MFmt/
  MFmtCheck branches run before the strip, so `kai fmt` sees them);
  `load_prelude`, `emit_prelude_cache_for` and the import resolver
  strip at load time. Consequences: the `.kab` cache format is
  untouched (no version bump — its DDoc/DModuleDoc encoder arms are
  `panic` guards), codegen/monomorph/perceus never see the wrappers,
  and the `--doc-json` dump is purely syntactic (no typecheck).
- **`TypedProgram.module_doc` via a carrier decl.** The root file's
  single `DModuleDoc` is the one wrapper allowed to ride the pipeline
  (every pass between parse and infer either has a `_ ->` default or
  got a one-line arm), and `infer_program`'s terminal lifts its text
  into the new field. Alternative — threading a parameter into
  `infer_program` — was rejected: 16 call sites, all of which would
  pass `None`.
- **Module-vs-item rule, operationalised.** "Immediately followed by a
  declaration head" became: at most ONE newline token between `]` and
  a head token (`pub fn type const effect protocol axiom impl extern
  #[`). A blank line (≥2 newlines), `import`, EOF, or any other token
  ⇒ module doc. The issue's example (module doc, blank line, then
  `import`) and the stdlib layout both satisfy this. Quirk worth
  knowing: `#[doc] <blank> pub fn` is a module doc — and a second one
  errors — which is the strictness the issue asked for.
- **Verbatim capture.** The doc body is the raw byte span between the
  quotes — no escape decoding (it is CommonMark, not a kaikai string).
  The lexer already tokenises `"""…"""` (and `#{…}` inside strings) as
  one TkString, so code fences and interpolation examples inside docs
  survive untouched. `kai fmt` re-emits the whole `#[doc(...)]` from
  its recorded byte span: byte-identical by construction, fmt-idempotent
  (verified).
- **JSON escaper moved to util.kai.** doc_attr needs JSON string
  quoting; infer owned the only escaper. Re-authoring it would have
  been a new `km dups` group; importing infer from doc_attr would
  cycle (infer imports doc_attr). Moving `json_escape`/`json_quote`
  down to util.kai (infer's `json_str` stays as a one-line alias)
  resolved both and shrank infer by ~30 lines.

## Structural surprises

- **Exhaustiveness is only checked when the typer runs over the code
  that matches.** kaikai matches without a default arm panic at
  runtime (`panic: non-exhaustive match`, no location). The fast
  oracle turned out to be `kaic2 --check stage2/main.kai` — the
  selfhost typecheck statically lists every non-exhaustive Decl match.
  Before discovering that, the lane chased two sites with lldb
  (`b kai_prelude_panic` + bt, same technique as the KIR lane retro).
  Sites needing arms: modules/qualtype_decl, emit_c/expand_ta_decl +
  dump_decl_type, infer/expand_unit_aliases_decl, emit_shared/
  collect_decls, resolve/{register_one, chk_decl}, driver/{dump_decl,
  find_enc_decl}, cache/cache_decl_to_hex (panic guards), fmt/
  {decl_line, fmt_decl}.
- **stage1 is not in the parse path for stdlib.** kaic1 only ever
  parses `stage2/**` (where `#` comments swallow single-line `#[doc]`
  and triple-quoted attribute bodies are therefore banned — util.kai's
  header documents this pre-existing constraint). Only kaic2 parses
  stdlib, so multi-line module docs there are bootstrap-safe.
- **`km` needed an upgrade.** kimun 0.21.0 did not recognise `.kai`;
  0.24.0 does. Scores below are 0.24.0.

## Code-quality outcome (the differential gate)

- `stage2/compiler/doc_attr.kai` (new): **km A (90.6)**, 162 LOC,
  cogcom avg 2.6 / max 7, no new dup groups.
- Edited monoliths: minimal wiring only. util.kai 72.5 → 72.0 (C → C,
  the +json section; its cogcom-25 issue is the pre-existing
  esc_body_loop). Sampled stdlib files all moved slightly UP after
  migration (array 99.2→100.0, core/list 79.7→80.0, http 62.5→63.4,
  regexp 53.7→56.7).

## Stdlib migration count

Script-driven (one-off Python, line-run scanner at column 0):

- **55/55 module headers** → module-position `#[doc("""…""")]`.
- **234 item blocks** directly above pub declarations (incl. blocks
  above `#[unstable]`/`#[derive]`) → item docs.
- Kept as comments: 2 section-divider-led blocks, inline notes,
  blank-line-separated blocks, private-fn comments.
- 0 blocks skipped for embedded `"""`; `kai info builtins` now reports
  e.g. core/list 37/78 documented, protocols 10/46.

## Fixtures added

- `examples/negative/doc/dup_item_doc` — duplicate `#[doc]` on one
  item, error at second occurrence (tools/test-negative.sh).
- `examples/negative/doc/second_module_doc` — second module-position
  doc, error at second occurrence.
- `examples/negative/doc/doc_bare_flag` — `#[doc(hidden)]` rejected
  with the Phase 3 pointer.
- `examples/attributes/attr_doc_basic` — module doc + single-line +
  multi-line + doc-over-`#[unstable]` + type/const docs, compiled and
  run against an `.out.expected` golden (auto-discovered by
  test-attributes).
- `examples/doc/doc_json_basic` + `.doc.expected` — byte-exact
  `--doc-json` golden; `examples/doc/doc_holes` — `--holes-json`
  carries the surrounding declaration's doc. Both pinned by the new
  `stage2/Makefile test-doc-attr` target (in `test` and
  TEST_LIGHT_TARGETS, so tier1 runs it).

## Coverage gaps / follow-ups left for next lanes

- Stacked order `#[unstable] #[doc]` is rejected (parse_unstable_decl
  expects `pub` next); only doc-first stacking is accepted. Fine for
  Phase 1 — the migration always emits doc-first — but Phase 3 may
  want order-insensitivity.
- A `#[doc]` directly above `import` (no blank line) resolves to
  module doc rather than an "imports are not documentable" error.
- Module docs of imported (non-root) modules are dropped at load time;
  Phase 2's `kai doc` walks files directly so nothing is lost, but
  TypedProgram only carries the ROOT file's module doc.
- `--holes-json` doc lookup is line-based against the root file's side
  table; holes reported from non-root decls (rare) get no doc.
- The `kai info builtins` plain-text mode is an awk summary; a real
  renderer belongs to Phase 2/3.

## Cost vs estimate

Issue estimated "1-2 sessions, the stdlib migration is the bulk".
Actual: one session, and the migration was the CHEAP part (scripted,
~25 min including verification). The bulk was wiring the wrappers
through a pipeline that enforces exhaustiveness lazily — the
qualtype/expand_ta/register_one whack-a-mole until the selfhost
typecheck oracle was found.

## Build/verification log

- tier0 green (selfhost deterministic kaic2b.c == kaic2c.c) after the
  compiler core and again after the stdlib migration.
- test-stdlib-modules 55/55; test-negative 109 PASS (3 new);
  test-attributes 7 OK (1 new); test-doc-attr 3 OK (new).
- tier1 green (one flaky-under-load test-effects SIGTERM while a
  background tier1 raced the foreground rerun; clean run passes).
