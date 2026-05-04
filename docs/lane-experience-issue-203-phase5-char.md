# Lane experience — issue-203-phase5-char

PR 5 of the 7-PR m14 milestone (audit `docs/m14-bootstrap-audit.md`,
PR #214). Migrates `stdlib/core/char.kai` consumers from the legacy
`ch_*` flat-prefix surface to the module-relative `char.*` form, and
removes all 8 `ch_*` aliases from `char.kai` itself.

## Objective metrics

- Start (lane init):     2026-05-04T01:51:02-04:00
- End (retro written):   2026-05-04T02:01:30-04:00
- Wall clock:            ~10 min
- Build/test invocations:
  - `tier0`:                       2 (baseline OK, post-mig OK)
  - `test-stdlib-core-intrinsic`:  1 OK
  - `tier1`:                       1 OK
  - `tier1-asan`:                  1 OK
  - `selfhost`:                    1 OK
  - `selfhost-llvm`:               not a make target in this tree

See appended TSV for the full log.

## Migration inventory

`stdlib/core/char.kai` exports today (legacy `ch_*` flat surface,
before lane):

```
pub fn ch_is_digit
pub fn ch_is_lower
pub fn ch_is_upper
pub fn ch_is_alpha
pub fn ch_is_alnum
pub fn ch_is_space
pub fn ch_to_lower
pub fn ch_to_upper
```

The 8 bare-name primaries (`is_digit`, `is_lower`, `is_upper`,
`is_alpha`, `is_alnum`, `is_space`, `to_lower`, `to_upper`) already
existed before this lane — Phase 5's job was to retire the `ch_*`
delegators after migrating consumers.

### Counts

- Bare-name exports:           8 (already present, no add needed).
- Flat aliases removed:        8 (all 8 `ch_*` deleted; zero #219 blockers).
- Flat aliases surviving:      0.
- Consumer files modified:     8 (1 stdlib core, 4 stdlib non-core,
                                  1 stdlib encoding sub-tree, 1 example,
                                  excluding stage compilers).
- Consumer call sites changed: ~16 distinct call sites across
                                  the 8 files (replace_all in 5 of them).

### Files modified

```
stdlib/core/char.kai                              (8 alias defs + legacy test removed,
                                                    header rewritten)
stdlib/core/string.kai                            (2 sites + 1 comment;
                                                    bare `is_space`, NOT `char.is_space`,
                                                    because both files can be the
                                                    test-stdlib-core-intrinsic target)
stdlib/regexp.kai                                 (2 sites: char.is_digit)
stdlib/uuid.kai                                   (1 site: char.is_digit)
stdlib/money.kai                                  (1 site: char.is_alpha)
stdlib/encoding/base64.kai                        (3 sites: char.{is_upper,is_lower,is_digit})
stdlib/encoding/json.kai                          (2 sites: char.is_digit)
stdlib/encoding/hex.kai                           (1 site: char.is_digit)
examples/aspirational/calculator/calculator.kai   (3 sites: char.{is_digit,is_alpha,is_alnum})
```

### Files NOT modified (deliberately)

- `stage1/compiler.kai`, `stage2/compiler.kai` — out of lane scope.
  Both define their OWN local `fn ch_is_digit`/`ch_is_alpha`/... at
  the top of the file (they predate the stdlib module-loading path
  and self-host the lexer); those internal helpers are not
  consumers of `stdlib/core/char.kai`'s aliases. Stage compilers
  retire their local helpers under PR 6/7 if they ever do.
- Stage 0 prelude builtins (`char_at`, `char_to_int`, `int_to_char`)
  defined in `stage0/check.c`, `stage0/emit.c`, `stage0/runtime.h`.
  The audit (`docs/m14-bootstrap-audit.md` Risk 5) is explicit that
  these are below the user-facing language and out of scope.

## Compiler errors I encountered

**None.** The first post-migration `tier0` was green. This is the
first phase of the m14 milestone where the bare-name promotion did
not cascade through #219 collisions:

- Phase 2 (`string`): collisions `repeat`/`split` with `list`.
- Phase 3 (`option`): collisions `unwrap_or`/`and_then`/`or_else`/
  `unwrap_or_else`/`collect` (newly created, then resolved by
  forcing surviving `option_*` aliases).
- Phase 4 (`result`): collisions for the same 5 names against
  `option`'s newly bare exports, plus `result_map`/`result_collect`.

For `char`, the bare names (`is_digit`, `is_alpha`, `is_alnum`,
`is_lower`, `is_upper`, `is_space`, `to_lower`, `to_upper`) are
already character-domain-specific and do not appear as bare exports
in `string`, `list`, `option`, `result`, or `tuple`. Verified with
`grep -rn "fn is_digit\|fn is_lower..." stdlib/`.

## Friction points

### #219 collisions — DID NOT bite this lane

The lane brief flagged HIGH risk: `is_digit`, `is_lower`, `is_alpha`,
`to_lower`, `to_upper` are very generic. In practice none of these
names exist as bare exports anywhere else in stdlib. The risk
assessment was correct in principle (these names are very generic
and could collide with a future module) but did not materialise on
the current code base.

### test-stdlib-core-intrinsic gotcha — DID NOT bite this lane

Phase 4's worst failure mode (cross-target bare-name silently
dispatching to the wrong implementation at runtime, surfaced as
`panic: non-exhaustive match`) requires the names to be ambiguous.
`char.kai`'s bare exports are unique across stdlib/core, so the
gotcha could not trigger.

The one place where caution was warranted is `stdlib/core/string.kai`,
which itself is an intrinsic test target. I deliberately spelled
its 2 sites as bare `is_space(c)` rather than `char.is_space(c)`
because when `char.kai` is the harness target, `char` is not in the
module table and a qualified call from `string.kai` (loaded as
prelude) would fail to resolve. Bare names work because `char.kai`'s
pub fns are still resolved through normal prelude scope. Comment
added in `string.kai` lines 19-26 to document this.

### UFCS opportunism — none surfaced

No UFCS fixtures touch `ch_*`-shaped methods (every consumer is a
free-fn call inside a parser/lexer-style loop), so no
UFCS-conversion wins were available.

### Spec ambiguities or interpretive choices

#### `string.kai` uses bare, not qualified

Documented above. The retro of Phase 3 (`docs/lane-experience-issue-203-phase3-option.md`)
established that intrinsic-test target sources cannot use qualified
calls back into themselves. Extension to cross-core-file consumers:
when both files can be the target, prefer bare. Comment in
`string.kai` cites the test-harness reason explicitly.

#### Stage-compiler local helpers untouched

`stage1/compiler.kai` lines 12-17 and `stage2/compiler.kai`
lines 26-31 declare their OWN `fn ch_is_digit` etc. (NOT marked
`pub`, so they don't compete with `stdlib/core/char.kai` exports).
These are internal helpers used by stages 1 and 2 to lex their own
sources. They look like consumers of the stdlib aliases by name but
are independent definitions. Lane scope is explicit ("don't touch
stage1/stage2 compilers"); these stay.

## Subjective summary

**Confidence:** HIGH. All 8 bare exports cleanly migrated, all 8
flat aliases removed, zero surviving `ch_*` references in
non-stage-compiler `*.kai` sources. Six tiers green:
tier0, tier1, tier1-asan, intrinsic harness, selfhost, baseline
demos.

**Hardest:** Deciding whether `stdlib/core/string.kai` should call
`char.is_space` qualified or bare `is_space`. Phase 4's retro made
this clear in retrospect, but it's the interesting tradeoff in this
lane.

**Easiest:** Everything else. The cascading bare-name replacement
across 7 consumer files outside `core/string.kai` was a mechanical
`replace_all`. No collisions, no rebuilds against half-migrated
state, no test-harness surprises.

**Compiler help:** N/A — no errors encountered.

**Compiler hinder:** N/A — no errors encountered.

**Why this lane was easy compared to phase 2-4:** Character
predicates have unique, character-shaped names that nothing else in
the standard library wants to re-use. Once you exit the
collection/option/result triangle (`map`, `filter`, `unwrap_or`,
`and_then`), the #219 footprint shrinks to zero.

## Limitations of this report

- Did not file new evidence on #219. The lane brief flagged HIGH
  risk for `char`'s names; the risk did not materialise. No new
  data point for #219 either way (we did not promote any new
  collision; we landed entirely inside an unclaimed name space).
- Did not benchmark stage 2 self-compile before/after; the change
  is too small (~16 call site renames + 8 alias removals) to
  perturb timing meaningfully and the lane brief does not request
  it.
- Did not touch `stage1/compiler.kai`'s diagnostic strings or local
  `ch_*` helpers; these are out of lane scope and would be
  rewritten under PR 6/7 if at all.
- `selfhost-llvm` is not a make target in this tree (also true in
  Phase 4); reported as N/A.

## Build/test TSV (appended)

```
timestamp	cmd	outcome	elapsed_s
2026-05-04T01:52:22-04:00	tier0-baseline	OK	40
2026-05-04T01:54:58-04:00	tier0-postmig	OK	33
2026-05-04T01:55:08-04:00	test-stdlib-core-intrinsic	OK	3
2026-05-04T01:59:45-04:00	tier1	OK	265
2026-05-04T02:00:40-04:00	tier1-asan	OK	49
2026-05-04T02:01:04-04:00	selfhost	OK	18
```
