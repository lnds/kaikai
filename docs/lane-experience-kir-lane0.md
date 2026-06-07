# Lane experience — KIR Lane 0 (define KIR + dumper)

**Scope:** Lane 0 of the KIR plan (`docs/kir-design.md` §7): define the KIR
node set, write `lower_to_kir` (AST → KIR), add `--emit=kir`, no emitter
touched. Foundation for Lanes 1–4 (the two translators).

## Scope as planned vs as shipped

Planned (design §7 Lane 0): `kir.kai` with the §4 types, `lower_to_kir`
over the post-perceus/post-tcrec AST, `--emit=kir` dumper, `.kir.expected`
goldens for representative programs, selfhost byte-id trivially preserved.

Shipped:
- `kir.kai` — the ~28-node §4 type set (slots, atoms, ops, RC ops,
  statements, terminators, blocks, fns, types, program). A++ / 66 LOC.
- `kir_lower.kai` — lowering BASE: state (`LowerSt`), deterministic name
  supply, block accumulation, type/decl walks, pure helpers. A+ / 121 LOC.
- `kir_lower_walk.kai` — the Expr→ANF recursion (expr ↔ call ↔ if/match),
  perceus magic-name decode → `KRC`, TCO/TRMC sentinel decode → typed
  terminators, resume → `KResume`. B+ / 275 LOC.
- `kir_dump.kai` — `--emit=kir` textual dumper, deterministic. A++ / 169 LOC.
- `examples/kir/{atoms_calls,control_flow}.kai` + `.kir.expected` goldens +
  `.kir.fns` extraction lists; `test-kir` harness wired into tier1
  (`TEST_LIGHT_TARGETS`, `test-fast`, `.PHONY`).

Full §4 node set was authored in one cut (per the integrator's call:
"set completo §4 de una"). Coverage of the recursive core is real and
exercised — 304/304 valid `examples/` programs produce KIR without panic;
the 2 that panic are negative contract fixtures the compiler rejects
upstream (not a KIR bug).

## Design decisions and alternatives considered

1. **Perceus as first-class `KRC` nodes (design §4.7), confirmed in code.**
   `__perceus_dup/drop/borrow` ECalls decode ONCE in `lower_named_call`
   into ordered `KRC` statements; borrow is a passthrough (no node), as the
   design argued. The dump shows `dup r` / `drop r` in position — the
   ordering-as-list-position property holds.
2. **Sentinels → typed terminators, reusing the shared parser.** The
   TCO/TRMC pipe strings decode via the EXISTING `es_tcrec_split_pipe` /
   `es_tcrec_parse_decimal` (emit_shared) — no parser duplicated — into
   `KTcrecGoto` / `KTrmcStep` / `KTrmcApply`. The dump shows
   `tcrec-goto _kai_..._entry {p0 <- t, ...} dropmask=4`, the string fully
   materialised. Added `KTrmcApply` + `KResume2` (2-arg resume) the design
   §4.2 omitted but the real emitters carry.
3. **One translator-neutral lowering.** `lower_to_kir` produces resolved
   symbols/tags; the dumper resolves nothing. Slots are conservatively
   `SBoxed` until i64-inline wiring (a later milestone) — correct, not yet
   optimised, and the slot is decided in ONE place (§4.6 goal met
   structurally even before the optimisation lands).
4. **Goldens are user-function-focused, not whole-dump.** The dump includes
   all auto-loaded core (~6700 lines), so a full-text golden would be huge
   and break on any stdlib churn. The harness extracts just the fixture's
   user functions (named in `<name>.kir.fns`) with the SAME awk used to
   author the golden — author == check. Plus a two-run determinism diff
   (Risk 2). Rejected: whole-dump goldens (fragile), grep-by-name (loses
   block structure).

## Structural surprises the brief did not anticipate

The big one: **the bundle-concat build (`make kaic2`) and the selfhost
build (`make selfhost`, real imports) enforce DIFFERENT rules.** A file
compiles under concat (flat namespace, no privacy, no import graph) while
violating things selfhost rejects. Hit four classes the hard way, now
captured in memory `project_kaikai_stage2_bundle_vs_imports_gap`:

- **Name collisions:** `dump_types`/`dump_stmt`/`dump_type`/`dump_program`
  already exist (driver AST dumper). Prefixed all dumper fns `kir_*`.
- **Module privacy:** every cross-file symbol must be `pub` + imported.
  Copied perceus's private `pcs_name_is_ctor` locally (`kir_name_is_ctor`)
  rather than `pub`-ing someone else's private fn.
- **Import cycles:** `lower_expr ↔ lower_call ↔ lower_if` is a mutual-
  recursion cycle the import DAG rejects. They MUST share a module (the
  reason emit_expr/emit_stmt live together in emit_c). Split the lowering
  into BASE (non-recursive, `kir_lower.kai`) + WALK (the cycle,
  `kir_lower_walk.kai`); base ← walk only, acyclic.
- **Reachability:** driver.kai must `import` each new module or selfhost
  can't find `emit_kir`.

Two name-shadowing traps the concat build hid (both already in memory):
- A binding named `args` resolves to the prelude `args()` (argv) → renamed
  every call-arg binder to `cargs`.
- A rest-binder `...rebuild` resolved to the top-level `fn rebuild` in
  region.kai (the bare-ident resolver-shadow bug) → renamed to `rbld`.
  This one was the long pole: it manifested as a runtime "non-exhaustive
  match" deep in `lower_exprs` receiving a closure instead of a list, found
  only by lldb-backtracing `kai_prelude_panic`.

## Code-quality outcome (§7.1 gate)

`km score` / `km cogcom`, per file:
- `kir.kai`        A++ (97.5), 66 LOC,  cogcom avg 0.0 / max 0
- `kir_lower.kai`  A+  (95.0), 121 LOC, cogcom avg 1.3 / max 4
- `kir_dump.kai`   A++ (97.9), 169 LOC, cogcom avg 1.3 / max 3
- `kir_lower_walk.kai` B+ (83.7), 275 LOC, cogcom avg 2.4 / max 12

Three of four are A-grade. `kir_lower_walk.kai` is B+ — on the floor, not
above it. Honest accounting: it is the recursive lowering core the import
DAG forbids splitting further. cogcom per-function PASSES the bar (avg
2.4 < 5, max 12 < 25); the B+ comes from km's AGGREGATE cognitive +
Halstead penalty on a dense 275-LOC module. Flattening the deepest match
pyramids (`lower_if`/`lower_match` → extracted `*_blocks` helpers) moved it
from B (80.6) to B+ (83.7). Further A− would need either let-destructuring
(kaikai lacks it — the compiler destructures via nested `match`
everywhere) or splitting the cycle (impossible). This is the genuine floor
for a recursive-descent lowering in this language; it is still far above
the F monoliths it will replace (emit_c 13.7K LOC).

## Fixtures added / coverage gaps

- `examples/kir/atoms_calls.kai` — atoms, direct calls, ANF flattening,
  let-binding.
- `examples/kir/control_flow.kai` — `if`→condbr+join, `match`→switch+
  per-arm blocks+`proj`+`dup`/`drop`.
- Both with `.kir.expected` goldens + `.kir.fns` lists; `test-kir` in tier1.

Coverage gaps left for later lanes (honest):
- No golden specifically pinning a TRMC/`tcrec-goto` terminator or an
  effect `KPerform`/`KInstallHandler`/`KResume` shape — those nodes ARE
  exercised over the corpus (rb-tree, effects) without panic, but lack a
  dedicated small golden. Lane 1 (KIR→LLVM) will want them; add then.
- `install_order` and `handlers` are empty in the `KProgram` — the design
  wants them populated from `builtin_default_install_order`; deferred to
  the lane that needs effect lowering (Lane 1/5). Flagged, not silently
  dropped.
- `KPos` is threaded but the dumper does not print positions yet (Risk 7
  says the dump SHOULD show them for #500). Positions ARE carried on every
  node; printing them is a one-line dumper add when #500's lane needs it.
- Constructor-vs-call classification: `Stdout.print` shows as `con` because
  the uppercase heuristic can't tell an effect-op call from a ctor without
  the variant table consulted at that site; cosmetic in the dump, to be
  refined when the translator needs exact dispatch.

## Cost vs estimate

Single session. The lowering itself was straightforward; ~70% of the time
went to the bundle-vs-selfhost gap (name collisions, privacy, the import
cycle restructure) and the two shadowing traps — all now in memory so the
next KIR lane pays none of it.

## Post-lane review (Linus) — fidelity fixes applied before Lane 1

A read-only review after the first commit surfaced three fidelity traps
for Lane 1 (none broke Lane 0, since the dumper is the only consumer).
Fixed in a follow-up commit on the same lane:

1. **Join register now materialised with `KLet` before the fork.** `if`/
   `match` joins were `KStore`d to a name no `KLet` declared, forcing a
   translator to *infer* the alloca. Now `lower_if_blocks`/
   `lower_match_blocks` emit `KLet(join, …, KLitOp(KUnitV))` before the
   condbr/switch, so the slot is explicit. The dump shows `t1: box = ()`
   ahead of the branch.
2. **`match` default block is `KUnreachable`, not a unit store.** kaikai
   matches are exhaustive; the old `KStore(join, ())` in the default would
   silently make a non-exhaustive program return `()` instead of trapping.
   The dump shows `L1: unreachable`.
3. **Ordinal-index invariant documented** in `lower_tcrec`/`lower_trmc_step`:
   cargs order == loop-param slot order (guaranteed by `tcrec_rewrite_decls`),
   so the pipe's `_pnames` are deliberately unused; the dropmask bitfield
   shares the same ordering. Plus the `"Cons"` reuse fallback invariant.

All goldens regenerated; test-kir + selfhost byte-id + km grades hold.

## Follow-ups for next lanes

- Lane 1 (KIR→LLVM translator): the node set + dumper are the contract.
  Add TRMC/effect goldens before relying on those terminators.
- Populate `KProgram.install_order` + `handlers` when effect lowering lands.
- Print `KPos` in the dump for #500.
- Wire i64-inline slot kinds (`SInt64`) from `CtorIntSlots` — the slot
  field exists; only the BASE `lower_ctor_slots` needs the classification.
