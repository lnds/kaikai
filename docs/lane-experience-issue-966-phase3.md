# Lane experience — issue #966 Phase 3: lint catalog expansion

The linter (`kai lint`, #966) is a permanent home for opinion rules, not
a feature that closes. Phase 1 shipped the command + `discard_pure_value`;
Phase 2 added the idiom nudges (`point_free_nudge`, `and_then_to_map_nudge`).
This lane (Phase 3) adds the rest of the seeded catalog. It uses
`refs #966` — **#966 stays OPEN** as the catalog's home for future rules.

## Scope as planned vs as shipped

The brief listed eight candidate rules. Empirical probing against the
current compiler reduced this to **six** shipped, with the owner's
sign-off on two cuts:

- **`dead_code_unreachable_arm` — cut.** The compiler already rejects
  both canonical shapes as HARD ERRORS: an arm after a catch-all
  (`unreachable match arm`) and a duplicate literal (`duplicate match
  arm`). The linter runs only after a clean type-check, so it would
  never see those programs — the rule would be dead code in the linter
  itself. A linter must not duplicate a hard compiler error (Clippy's
  discipline: `unreachable_patterns` lives in rustc, not Clippy). A
  related gap surfaced: `Some(_)` followed by `Some(5)` (a subsumed,
  non-identical arm) DOES pass the checker — that is a usefulness-check
  gap in the compiler, not lint material; left for a separate issue.
- **`redundant_unwrap_or_zero` — cut.** `o.unwrap_or(0)` has no simpler
  equivalent form: the zero does not make it redundant, it is exactly
  what you want. No firing case is sound, so any fire would be a false
  positive. The brief itself said "if in doubt, do NOT fire."

Shipped rules: `redundant_if_bool`, `redundant_match_catchall`,
`match_option_to_combinator`, `dead_code_unused_priv`,
`effect_over_declared`, `effect_ffi_without_extern`.

## Split as shipped

`lint.kai` was already near the 400-LOC bar, so the catalog was split by
family BEFORE growing it:

- `lint.kai` (352) — core walk, dispatch, reporting, purity (unchanged
  rule: `discard_pure_value`).
- `lint_idiom.kai` (260) — `point_free_nudge`, `and_then_to_map_nudge`
  (unchanged).
- `lint_redundant.kai` (~100) — `redundant_if_bool`,
  `redundant_match_catchall`.
- `lint_combinator.kai` (217) — `match_option_to_combinator`.
- `lint_deadcode.kai` (172) — `dead_code_unused_priv` (program-level).
- `lint_effects.kai` (254) — `effect_over_declared`,
  `effect_ffi_without_extern` (program-level).

Every new file is under the 400-LOC cap. Expr-level rules hook into the
per-node walk via `*_findings(e)`; program-level rules (dead-code,
effects) are appended once over the whole decl list.

## Design decisions

- **Effect rules: per-function `declared − used`, never re-derived**
  (asu consult). The check compares each pub function's declared CLOSED
  row against the effects its OWN body demands, reading the sources the
  typer already resolved: ops referenced, effects on each call's stamped
  row (so transitivity is the typer's answer, not re-computed), handlers
  installed, and an `EExternBody` (which demands Ffi by construction).
  Open rows and parametric labels are skipped. First implementation used
  a whole-program used-set — wrong: an effect used by ANY function
  masked an over-declaration in another. Fixed to per-function.
- **`effect_ffi_without_extern` is `effect_over_declared` instantiated on
  Ffi** — one motor, two lint ids. A pub `extern "C" fn` never fires
  because its `EExternBody` counts as an Ffi use.
- **Dead-code is intentionally non-transitive.** A name mentioned
  anywhere (even inside another unused function) counts as used. This
  avoids false positives on mutually-recursive or example-only chains;
  the bias is false-negatives over noise. `pub` and `main` are exempt.
- **`unwrap_or` only on a TRIVIAL default.** `match o { Some(x) -> x;
  None -> dflt }` is `unwrap_or(dflt)` only when `dflt` is a literal /
  variable / nullary ctor. A computed default is `unwrap_or_else`
  territory — `unwrap_or` evaluates eagerly, the lazy `None` arm does
  not — so it is left alone. This is the evaluation-order equivalence
  guard the rule hinges on.

## Structural surprises (the cost sink)

The bulk of the lane's time went to **kaic1 (stage1) codegen traps** in
new `compiler/*.kai`, none of which surface as a build error — they
produce a broken kaic2 or a runtime `panic: non-exhaustive match`:

1. **Non-ASCII bytes corrupt the whole bundle.** A single `⇒` in a
   COMMENT made kaic2 emit a spurious `__strip_unit / Real<u>` type error
   on every input file. Fix: ASCII-only in compiler sources.
2. **`ECall(_, args)` — wildcard in the callee slot, args fed to a
   list-match — miscompiles to a runtime panic.** `ECall(c, _)` (read
   first field) is fine. The arity check it fed was unnecessary (a
   constructor's arity is fixed), so the fix was to drop it and check
   only the callee.
3. **`EList([])` as a match arm panics** when a non-empty list reaches
   it. Use `EList(els) -> empty_helper(els)` with a cons-pattern helper.

The reliable method was: mimic the EXACT shape of an existing working
helper in the same bundle (e.g. `lint_idiom`'s `arity_one`), keep sources
ASCII, and rebuild + run a real fixture after every change — a clean
`make kaic2` does NOT mean the binary is correct. Debugging compiler-side
logic was done by emitting values into the lint message string (the only
"printf" available). Captured in a memory note for future lanes.

A second surprise: the **root-file `mo` inconsistency** (a pub root fn
carries `mo = Some(root_module)`, a private one `None`). The effect rules
need pub functions, so `lint_program` now takes the root file and derives
the root module name via the now-pub `inf_prelude_module_name`. This also
forced every NON-effect fixture to gain a `main` that exercises its demo
functions, so `dead_code_unused_priv` does not fire on them — the
fixtures now read like real programs, which is an improvement.

## Fixtures

Positive + negative pair per rule under `examples/lint/`, auto-globbed by
`test-lint`, with `.lint.expected` (human) and `.lint-json.expected`
(JSON) goldens. 18 fixtures total (6 pre-existing + 12 new). Negatives
encode the precision boundary of each rule: the lazy-default case for
`unwrap_or`, the open-row and handled-effect cases for over-declared, the
pub-extern case for Ffi, the non-transitive chain for dead-code.

Coverage gap: no fixture exercises a rule firing INSIDE a nested lambda
or match arm for the program-level rules (dead-code / effects only scan
top-level decls), because those rules are decl-shaped by design.

## Verified

- `make tier0` green at each block (selfhost byte-identical:
  `kaic2b.c == kaic2c.c`).
- `make -C stage2 test-lint`: 18/18 OK on the C backend.
- `tools/test-info-blocks.sh`: 94/94 doc blocks compile (the six new
  `kaikai` examples in `docs/info/lint.md` among them).
- Every rule fires on a real case AND in `--lint-json`; severity
  `warning`, exit 0 (never blocks a build).

## Follow-ups for next lanes

- Compiler usefulness-check gap: `Some(_)` before `Some(5)` is an
  unreachable arm the checker accepts. Worth an issue (label `compiler`).
- `dead_code_unused_priv` could extend to unused private `type`s and to
  `pub`-in-a-binary once binary-vs-library context is available; both
  were left out as higher false-positive risk.
- The effect rules read the declared row from the `RowExpr` (surface
  form); a future pass could read the resolved `Row` for parametric or
  aliased effects, which are currently skipped conservatively.
