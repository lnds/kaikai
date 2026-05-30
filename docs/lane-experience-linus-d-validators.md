# Lane experience — linus-d-validators

Lane D of the km quality audit (`docs/stage2-quality-km-audit-2026-05-29.md`):
unify the near-identical same-module name-collision validators in
`stage2/compiler/infer.kai` into one parametric walker plus thin wrappers.

## Scope as planned vs as shipped

**Planned (brief):** "unify the 6 near-identical name-collision validators
into one parametric validator + thin calls, ~400 LOC removed." The brief
listed `type / fn / effect / const / axiom` as the five linear walkers and
named a shared helper `mk_collision_diag(file, name, "<entity-label>",
line, col)`, with `validate_union_collisions_decls` as the lone outlier.

**Shipped:** four walkers unified (`fn`, `effect`, `const`, `axiom`); two
left bespoke (`type` AND `union`); 124 net LOC removed from `infer.kai`
(104 inserted, 228 deleted). The brief's premises were partly inaccurate
and the implementation corrected for them — see below.

### Two corrections to the brief

1. **`mk_collision_diag` does not exist.** A repo-wide grep found no such
   helper. The real pattern was one bespoke `report_dup_<entity>_decl` per
   kind, each emitting a *different* error noun ("function"/"effect"/…)
   AND a *different* help line (`'fn foo(...)'` vs `'effect E { ... }'` vs
   `'const N = ...'` vs `'axiom A(...)'`). So the parametric path varies on
   **three** text fragments, not one label. That is encoded as
   `EntitySpec = ES(word, kw, syntax)`.

2. **`type` is a second outlier, not a fifth linear walker.**
   `validate_type_name_collisions_decls` layers cross-module collision
   (case a: a `pub type T` already exported by a prelude/imported module),
   reserved-builtin-name rejection (`Cont`), `DDerive`/`DUnstable`
   unwrapping, and a *distinct* message ("already declared in this scope",
   with module-origin phrasing). Its public signature also carries three
   extra params (`module_table`, `prelude_mods`, `target_mod_name`).
   Forcing it through the generic mould would drag all of that into the
   shared path. Left untouched.

## Design — parametric walker + extractor closures

Two small nominal types (no anonymous tuples — the stage2 compiler uses
nominal records/sums throughout, e.g. `CollisionResult`, and stage1
kaikai-minimal has no anonymous-tuple precedent in this bundle):

```kaikai
type NameSite  = NS(String, Int, Int)        # name, line, col
type EntitySpec = ES(String, String, String) # word, kw, syntax
```

One walker carries the shared linear scan:

```kaikai
fn validate_name_collisions_walk(remaining: [Decl], file: String,
                                    extract: (Decl) -> Option[NameSite],
                                    spec: EntitySpec,
                                    seen: [NameSite], errs: Int) : Int / Console + File
```

`extract` is a **pure, top-level named function** (`extract_fn_site`,
`extract_effect_site`, `extract_const_site`, `extract_axiom_site`) passed
by name — exactly how `any_arm(arms, arm_pat_empty_list)` and the
`inf_map_*` family pass `f`/`pred` in this same file. That is the
conservative choice for stage1: the higher-order parameter is a named fn
value, not a capturing lambda. Each extractor returns
`Some(NS(name, line, col))` for the one `Decl` variant it owns and `None`
for everything else, so the walker stays oblivious to variant slot order.

Each public entry point shrinks to a one-liner:

```kaikai
pub fn validate_fn_name_collisions_decls(root_decls: [Decl], file: String) : Int / Console + File {
  validate_name_collisions_walk(root_decls, file, extract_fn_site,
                                   ES("function", "fn", "(...)"), [], 0)
}
```

The four public signatures are **unchanged** (the driver calls them at
`driver.kai:4612/4626/4628/4630`); only their bodies changed.

### Diagnostic byte-identity

The shared `report_dup_name_decl` reassembles the exact original strings.
For `fn`: `"function '" + name + "' is already declared in this module
(earlier at " + file + ":" + line + ":" + col + ")"` and help `"remove or
rename the second 'fn " + name + "(...)' declaration"`. Substituting
`word/kw/syntax` per kind reproduces all four originals character-for-
character — verified by compiling each `examples/negative/modules/
duplicate_*_decl.kai` fixture with the rebuilt `kaic2` and diffing the
emitted error + help lines against the pre-refactor output.

## Why `union` (and `type`) stay out

- **`union`** (`validate_union_collisions_decls`): accumulates per-variant
  `VariantClaim`s across all TBSums, pre-seeds the claim table with builtin
  `Result`/`Option` constructors (`builtin_variant_claims`), resolves the
  declaring file per type via the module table, and distinguishes a
  builtin collision (allowed since #644) from a user-vs-user D2 collision.
  None of that fits a per-decl `(Decl) -> Option[NameSite]` extractor.
- **`type`**: see correction #2 above.

Unifying either would require either widening the generic walker with
optional module/seed parameters (defeating the simplification) or special-
casing inside the shared path (reintroducing the duplication the lane set
out to remove). Both stay as standalone walkers; the shared walker's header
comment records the boundary.

## LOC before / after

`infer.kai`: 104 insertions, 228 deletions → **−124 net**.

The four linear walkers were ~282 lines (4 `*Claim` types + 4 public fns +
4 `*_loop` + 4 `validate_one_*` + 4 `find_*_name_claim` + 4
`report_dup_*_decl` ≈ 24 elements). They collapse to ~110 lines (2 types +
1 walker + 1 `find_name_site` + 1 `report_dup_name_decl` + 4 extractors + 4
thin public fns ≈ 13 elements). The brief's ~400 estimate assumed `type`
was in scope; the honest figure over the genuinely-linear four is 124.

## Fixtures

Coverage was already complete — one negative fixture per linear kind:

- `examples/negative/modules/duplicate_fn_decl.kai` (+`.err.expected`)
- `examples/negative/modules/duplicate_effect_decl.kai`
- `examples/negative/modules/duplicate_const_decl.kai`
- `examples/negative/modules/duplicate_axiom_decl.kai`

plus `examples/negative/type_name_collision/duplicate_type_decl.kai` and
`examples/unions/d2_collision.kai` for the two outliers. These run under
`make test-negative` (tier1) with grep-substring goldens; the byte-identical
diagnostics keep all goldens passing. **No new fixture added** — adding one
would be redundant.

## Structural surprises

- **`selfhost` is fixed-point, not byte-identical to the pre-refactor C.**
  Expected and benign: `variant_tag` ids are assigned globally and
  positionally to every sum constructor in the bundle. Removing four
  `*Claim` sum types and adding two (`NameSite`, `EntitySpec`) shifts the
  global tag counter, so downstream matches (e.g. on lambda/clause Decl
  tags in `emit_c`) receive renumbered tags — identical bodies, shifted
  numbers (`369`→`367`, `418`→`416`, …). The 1417-line C delta vs baseline
  is entirely this renumbering plus the validator rewrite. The gate that
  matters — `kaic2b.c == kaic2c.c` (and `s1.ll == s2.ll`) — converges, so
  the renumbered compiler is a stable fixed-point. The brief anticipated
  this ("byte-identical OR fixed-point … if the emitted text changes,
  investigate why"); investigated, root-caused to tag renumbering.

- **Stale base hid a fixed regression; rebased onto the fix.** The lane
  branched from `ce5e43b` (merge of #729), where `make tier1` aborts at
  `test-issue-318-include` with a runtime SIGSEGV (exit 139) after ~76
  prelude tests. An A/B — reverting `infer.kai` to its `ce5e43b` version,
  rebuilding `kaic2` from stage0, rerunning the fixture — reproduced the
  identical crash, proving the refactor did not cause it. Checking CI
  history then showed the cause precisely: `ce5e43b`'s tier1 was already
  red on CI, and `0efb17d` (#732, "register boxed shim for unboxed
  builtin protocol impls") turned it green — that is the proto-eq/Ord ABI
  regression where nested dispatch routes an unboxed builtin impl through
  a boxed cast and SIGSEGVs; the prelude-test suite exercises Eq/Ord
  heavily, hence the crash. `origin/main` had advanced 7 commits past the
  lane base to `0efb17d`. **Rebased the lane onto `origin/main`** (clean,
  no conflicts — the 7 commits touch emit/perceus/protocols, not the
  collision-validator region) and re-ran every gate from a clean stage0
  build. Lesson reinforced: `git log HEAD..origin/main` before the first
  build, every lane — a stale base can mask both regressions and their
  fixes.

- **Worktree/main checkout slip (recovered).** Early edits and the first
  verification round accidentally ran in the *main* checkout
  (`…/kaikai`) instead of the lane worktree (`…/kaikai.linus-d-validators`)
  because the working dir resolved to the shared root. Caught via
  `git stash pop` reporting `On branch main`. Recovered per the standing
  procedure: captured the `infer.kai` diff as a patch, `git apply --3way`
  into the worktree, `git checkout` to revert main, removed the
  mis-placed retro from main (the three other untracked files in main —
  `pass`, `docs/benchmarks/…`, `docs/stage2-quality-km-audit-…` — were
  pre-existing and left intact), then re-ran ALL gates from the worktree
  on a clean stage0 build. The byte-identical-diagnostics result held
  across the move.

## Cost vs estimate

No time estimate (per project policy). Gated on soundness + verifiability:
all gates green, diagnostics byte-identical, public API unchanged.

## Gates

All run from the lane worktree on a clean stage0 build, **after rebasing
onto `origin/main` (`0efb17d`, #732)**:

- `make selfhost` — **OK** (`kaic2b.c == kaic2c.c`), fixed-point.
- `make selfhost-llvm` — **OK** (`s1.ll == s2.ll`).
- `make tier0` — **OK** (selfhost deterministic + demos baseline 34/34).
- `make tier1` — **OK** (full `make test` + demos baseline + fmt + bench +
  check + library-mode + diagnostics-collected + negative-space + stdlib
  modules + package-mode + shadow audits + alias audit + `kai info`).
  Includes `issue-318-include OK` (the #732 fix removed the SIGSEGV the
  stale base showed) and `modules-collision OK`.
- `make test-negative` — **105 PASS, 0 FAIL**, including the four core
  fixtures `duplicate_{fn,effect,const,axiom}_decl.kai` and
  `duplicate_type_decl.kai`.
- Collision diagnostics verified byte-identical pre/post by compiling
  each fixture with the rebuilt `kaic2` and diffing the `error:` + `help:`
  lines.

## Follow-ups for next lanes

- None required. If a future lane ever needs to unify `type`/`union`, the
  honest path is a *separate* shared helper for cross-module-aware
  validators — not widening this one.
