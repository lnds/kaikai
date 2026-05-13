# Lane retro — issue #538 module system (shapes 2 + 3)

## Scope as planned

Close shapes 2 and 3 of the three module-layer gaps reported in #538:

- **Shape 2** — duplicate `fn` declarations in the same module were silently
  shadowed (second wins). Type-level duplicates were already rejected by
  `validate_type_name_collisions_decls` (#518); the function-level analogue
  was missing.
- **Shape 3** — `import nothing_here` printed `kaic: cannot open module …`
  to stderr but exited 0 unless some downstream code referenced a name from
  the missing module. The resolver had no error counter, only an
  `eprint` side-effect.

Shape 1 (import cycles silently accepted) was intentionally left out of this
lane per the 2026-05-12 audit — it requires threading an `in_progress` stack
through the resolve-state plumbing, which has a larger blast radius and
should not be bundled with the two scalar fixes.

## Scope as shipped

Matches the plan. No scope creep, no scope shrink.

- Shape 2: `validate_fn_name_collisions_decls` walker added next to its type
  twin. Same diagnostic shape (anchor at the second site, cite earlier
  file:line:col, add a help line). Wired into the `compile_source` exit-code
  aggregator after `type_name_coll_errs`.
- Shape 3: `ResolveState` grew an `errs: Int` field; `rs_bump_errs` increments
  it on every `read_file` miss. `ExpandedProgram` (the `EP_Mod` ctor) carries
  the count out so `compile_source` can fold it into the aggregator. Placed
  first in the aggregator chain because a missing module makes everything
  downstream meaningless.
- Two fixtures promoted / added:
  - `examples/negative/silent_contract/duplicate_fn_decl.kai` →
    `examples/negative/modules/duplicate_fn_decl.kai` + `.err.expected`.
  - `examples/negative/modules/missing_import_no_use.kai` + `.err.expected` —
    new fixture covering the "no downstream use" variant that pre-#538 was
    invisible to `test-negative.sh`.

## Design decisions

**Adding `errs: Int` to `ResolveState` vs. a separate side-channel.** The
field is the obvious extension and was free to plumb — three constructor
sites updated, one new helper `rs_bump_errs`, plus the `EP_Mod` ctor widened
from 3-ary to 4-ary. Threading a separate counter through `process_imports`
would have meant a second return type or a pair, which complicates every
recursive caller. Rejected.

**Where to place `import_resolve_errs` in the aggregator.** Put first
(before `alias_res.errs`) because the resolver runs before alias / type /
contract validation, and a missing module's decls never enter the flat list,
so every downstream pass operates on an incomplete program. The first error
the user sees should be the import failure itself, not a cascade.

**No shared helper between `validate_type_name_collisions_decls` and
`validate_fn_name_collisions_decls`.** The type validator does more work
(prelude-collision detection against `module_table`, distinct help wording
for prelude vs. import shadow), and the fn validator only needs the
in-flight-claims half. Sharing a helper would have meant either parameter-
izing the prelude path or stripping out half the call sites. The 30-line
loop + claim-finder reads cleaner duplicated — same shape as the type case,
same `FNC(name, line, col)` triple. Aligns with the "no premature
abstraction" Tier-1 rule.

**Fixture for shape 3.** `missing_import.kai` (the pre-existing enforced
fixture) ships a downstream call `nothing_here_fn()`. Pre-#538 that call
was what produced the non-zero exit (unbound-name typer error), not the
missing-module loader event. The new `missing_import_no_use.kai` strips the
downstream reference, so the only signal is the resolver's error counter.
Both fixtures keep PASSing; the new one would FAIL on pre-#538 `kaic2`.

## Structural surprises

None. The shape-of-the-fix matched the audit hypothesis exactly:

- `validate_type_name_collisions_decls` had the right scaffolding to copy.
- `ResolveState` already had a record type with `update`-via-construction
  semantics, so adding a counter field was mechanical.
- The aggregator was already an `else if` chain ordered by which pass owns
  the diagnostic; inserting a new branch at the top required one line.

Pre-audit confirmed zero intra-file duplicate fns in `stage2/compiler.kai`,
`stage1/compiler.kai`, `stdlib/**`, `demos/**`, and `examples/**`. Selfhost
converged on the first pass.

## Fixtures and coverage

- `examples/negative/modules/duplicate_fn_decl.kai` + `.err.expected`
- `examples/negative/modules/missing_import_no_use.kai` + `.err.expected`

`test-negative.sh` went from 79 PASS to 81 PASS, 0 FAIL, 0 MISS. The
`silent_contract/duplicate_fn_decl.kai` file is gone; the
`silent_contract/import_cycle/` subtree stays in cuarentena, and the
README row for #538 was edited to mention shape 1 as the still-open piece.

## Real cost vs. estimate

Brief estimated 4–6 hours; lane closed in ~2 hours of focused work. Shape 3
was a 4-line semantic change wrapped in a 4-field record update. Shape 2 was
a copy-paste-and-strip of the type validator. The compiler rebuilt cleanly
on the first try after each shape; selfhost converged on the first pass; no
sweep was needed in the compiler / stdlib because no duplicate-fn decls
existed.

## Follow-ups for next lanes

- **Shape 1 of #538** — import cycle detection. Needs an `in_progress: [String]`
  stack on `ResolveState`, separate from the `visited` set. When
  `resolve_module` is asked for a path already in `in_progress`, emit a
  cycle diagnostic and bump `errs`. The fixture
  `examples/negative/silent_contract/import_cycle/` is ready to be
  promoted; the resolver wiring is the new work.
- **Linus's adjacent observation (out of scope here).** The same "blind
  prepend" pattern that gave us shape 2 also lives in `register_one`
  (compiler.kai:10548) for typeclass / impl / alias entries. Worth a
  follow-up issue checking whether duplicate `effect` / `protocol` / type
  alias decls in one module are also silently accepted. Not fixed here per
  brief.

## Issue state after merge

#538 should remain open after this lane lands, with the body retitled or
commented to indicate that shapes 2 and 3 are closed and only shape 1
(import cycle) is outstanding. The PR title flags `shapes 2+3` explicitly
so the issue tracker shows partial progress.
