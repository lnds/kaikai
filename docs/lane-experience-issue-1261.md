# Lane experience — issue #1261: the Result Ok-first flip is involutive, not idempotent

## Scope as planned vs as shipped

**Planned.** Make `kai migrate`'s Ok-first `Result` flip genuinely idempotent —
a second run over already-migrated source must be a byte-identical no-op —
and delete the `.oneshot` harness exemption that hid the defect.

**Shipped.** Exactly that, plus the piece the brief anticipated as "the fine
part": the convergence signal. Five touch points:

- `stage2/compiler/migrate_walk.kai` — the expression walk now threads a
  `ft: Bool` (flip types) so the three type-annotation sites inside bodies
  (lambda params, `let`, `var`) can be gated.
- `stage2/compiler/migrate_types.kai` — `mig_ty_opt_if` / `mig_params_if`,
  the gated wrappers.
- `stage2/compiler/migrate.kai` — `migrate_decls_renames_only`, the
  call-rename-only decl pass.
- `stage2/compiler/driver.kai` — `run_migrate` takes the declared edition and
  picks the pass; `orongo` joins `known_editions()`.
- `bin/kai` — `cmd_migrate` passes `--edition "$from_ed"` to kaic2, and
  `--write` stamps the target edition into `kai.toml`.

## Why the positional flip could not converge

`mig_result_args` rewrote every `Result[a, b]` to `Result[b, a]`
unconditionally. The rewrite is *positional*, and the two orders are the same
shape: `Result[Int, String]` (already migrated) and `Result[String, Int]` (not
yet) are indistinguishable to a walk that only sees the type node. A function
with no fixed point other than the empty program cannot be idempotent — it can
only oscillate. No amount of care inside the rewrite fixes this, because the
information needed (*has this already been flipped?*) is simply not present in
the input.

That is the general lesson: **an order-swapping rewrite is involutive by
construction. Its idempotency has to come from outside the syntax.**

## The re-coupling point chosen: edition, not marker

Two candidate signals:

1. **An intra-source marker** — a pragma or stamped comment in the `.kai` file
   saying "already migrated". Rejected: it contaminates the user's source with
   compiler bookkeeping, it is trivially lost to a hand edit or a `git
   checkout`, and it puts a migration artefact in a file that outlives the
   migration by years.

2. **The declared edition** — `kai.toml`'s `edition = "..."` (or the repo
   `EDITION` file). Chosen. The flip *is* the hanga-roa → orongo type-order
   change; it was only ever meant to apply to hanga-roa source. The bug was a
   decoupling: `kaic2 --migrate` ran the flip knowing nothing about the source
   edition, while `bin/kai` validated the edition pair and then discarded it.
   The fix re-couples them — the flip runs iff `edition == "hanga-roa"`.

Convergence then falls out of the edition lifecycle rather than from anything
in the source text: `--write` stamps `edition = "orongo"` into the manifest, so
the user's second `kai migrate` resolves `--from` to orongo, the Result rule no
longer applies, and the run is a no-op. The source file stays clean.

A flagless `kaic2 --migrate` now defaults to *not* flipping. That is the safe
direction: Ok-first is the present order, so the no-flag case treats input as
already-current and only applies the shape-recognisable call renames. Callers
that genuinely want the flip say so with `--edition hanga-roa`, which is what
`bin/kai` does.

Note that `orongo` was added to `known_editions()` / `edition_rank` so
`--edition orongo` is accepted. This is *not* an edition bump: the `EDITION`
file still reads `hanga-roa`, no feature gate keys off rank 2, and no global
flip to orongo happened. The compiler merely has to be able to *name* the
target edition of a migration it already advertises in `kai migrate --help`.

## Why `.oneshot` was a patch and not a fix

`examples/migrate/result_ok_first.oneshot` was an empty marker file that made
`tests/migrate_fixtures.sh` skip the idempotency assertion for that fixture.
It landed in the same commit as the flip, with an honest comment explaining
why. But its effect was to make the harness *agree* with the defect: the one
fixture whose rewrite was not idempotent was excused from the check that would
have caught it, so CI stayed green while `kai migrate --write` run twice
silently returned user code to Err-first — reporting "6 automatic rewrite(s)
applied" both times, indistinguishable from useful work.

The comment documented the hazard; it did not protect anyone from it. And the
code around it was actively lying: `driver.kai` said "Idempotent: a second run
over migrated source is a no-op" and `migrate.kai` opened with "migrate is a
source-preserving, idempotent AST rewrite". A test exemption that encodes a
known-wrong behaviour turns a bug into a specification. The exemption is gone;
`result_ok_first` now passes the same idempotency assertion as every other
fixture.

## Structural surprises the brief did not anticipate

**The flip was not confined to the decl pass.** The brief pointed at
`mig_result_args` in `migrate_types.kai`, and gating the decl-level entry
points (`migrate_decls`) looked sufficient. It was not: `migrate_walk.kai`
independently flips type annotations at three sites *inside* expression
bodies — `ELambda` parameter types, `SLet` and `SVar` annotations. The first
implementation attempt passed the "0 rewrites" counter check while still
flipping a `let r: Result[Int, String]` annotation, because the counter and
the rewrite are separate walks and only the counter had been gated. Worth
recording: **in this migrator, the rewrite count and the rewrite are two
independent passes, and gating one does not gate the other.**

Threading a `Bool` through the 137-line walk was preferred over duplicating it
(~70 lines of near-identical recursion in a second file) or reaching for global
state. The `map(xs, mig_f)` call sites became `map(xs, (x) => mig_f(ft, x))`.

**A fixture assertion compared unformatted source.** The new edition-gate check
first diffed migrate's output against the raw input file and failed on pure
layout differences (`let r :` → `let r:`, `type` body line breaks) with the
`Result` arguments plainly unchanged. Both migrate and fmt re-emit through the
fmt-writer, so the comparison baseline has to be `--fmt` of the same source,
not the file on disk. A migrate fixture that diffs against raw source is
asserting the formatter's output, not the migrator's.

## Fixtures added and coverage

- `examples/migrate/result_ok_first.oneshot` — **deleted**. The fixture now
  runs the standard idempotency assertion.
- `tests/migrate_fixtures.sh` — step 1 runs as `--edition hanga-roa`; step 2
  (idempotency) runs as `--edition orongo`, mirroring how a user's second
  `kai migrate` actually reaches the compiler after `--write` stamped the
  manifest.
- New **edition-gate** case in the same harness: Err-first source under
  `--edition orongo` must come back unflipped (compared against `--fmt` of the
  same source). This is the direct guard on the gate itself — the previous two
  steps would still pass if the flip were unconditional in one direction.

Verified beyond the harness: the second run over a *real* first-run output
(not just the golden) is byte-identical, and a flagless second run is also a
no-op. `Ok`/`Err` constructors are untouched, as they are position-independent.
The `collections_from` renames still apply under orongo — they are
shape-recognisable and idempotent on their own, so they are deliberately not
edition-gated.

Coverage gap left open: there is no fixture exercising `promote_manifest_edition`
end-to-end (a real package directory with a `kai.toml`, migrated with `--write`,
then migrated again). The harness works at the `kaic2` level and has no package
scaffolding; the shell function is straightforward `sed` over a manifest that
already has a validated `edition` field. Worth adding if `kai migrate` grows a
package-level test.

## Follow-ups left for next lanes

- **Manifest-less packages have nowhere to record the transition.** A loose
  `.kai` file with no `kai.toml` falls back to the repo `EDITION`, so a user
  migrating a single file outside a package must pass `--from` themselves on the
  second run or get a no-op with no explanation. Acceptable today (the no-op is
  the *safe* failure), but a targeted diagnostic — "this package declares no
  edition; pass --from" — would be kinder than silence.
- **The edition-gate pattern should be the precedent for the next order
  change.** Any future rewrite that permutes rather than renames has the same
  non-convergence property. The rule this lane establishes: a positional
  rewrite must be gated on the source edition, and the harness must assert the
  gate, not exempt the fixture.
- `docs/editions.md` may want a line stating that migration rules are keyed to
  the origin edition and that `--write` advances the manifest — the mechanism
  is now load-bearing for idempotency, not just bookkeeping.
