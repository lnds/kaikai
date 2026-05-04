# Lane experience — issue-203-phase3-option

PR 3 of the 7-PR m14 milestone (audit `docs/m14-bootstrap-audit.md`,
PR #214). Migrates `stdlib/core/option.kai` user-facing exports
from flat `opt_*` to module-relative `option.*` form, and sweeps
every consumer in `examples/stdlib/`, `stdlib/core/result.kai`.

## Objective metrics

- Start (lane init):     2026-05-04T00:36:54-04:00
- End (retro written):   2026-05-04T00:54:00-04:00
- Wall clock:            ~17 min
- Build/test invocations:
  - `tier0`: 3 (baseline OK, post-mig-1 FAIL, post-mig-2 OK)
  - `tier1`: 1 OK
  - `tier1-asan`: 1 OK
  - `selfhost`: 1 OK (no-op gate; tier0 already exercises it)
  - `selfhost-llvm`: not a make target in this tree

See appended TSV for the full log.

## Migration inventory

`stdlib/core/option.kai` exports today (flat-name surface, before lane):

```
pub fn opt_is_some
pub fn opt_is_none
pub fn opt_map
pub fn opt_unwrap_or
pub fn opt_and_then
pub fn opt_or
pub fn opt_or_else
pub fn opt_unwrap_or_else
pub fn opt_ok_or
pub fn opt_ok_or_else
pub fn opt_filter
pub fn opt_zip
pub fn opt_collect
```

13 exports total, all migrated targets.

### Counts

- Bare-name exports landed:    9 (`is_some`, `is_none`, `unwrap_or`,
                                  `and_then`, `or_else`,
                                  `unwrap_or_else`, `ok_or`,
                                  `ok_or_else`, `collect`)
- Flat aliases surviving:      4
  - `opt_map`     — bare `map` collides with `list.map` under #219
  - `opt_filter`  — bare `filter` collides with `list.filter` under #219
  - `opt_zip`     — bare `zip` collides with `list.zip` under #219
  - `opt_or`      — bare `or` is a reserved keyword (`TkOr`); the
                    parser rejects `pub fn or(...)`, so this alias
                    is unblocked by #219 and survives until the
                    language exposes a way to spell keywords
                    module-relatively.
- Consumer files modified:    7 (6 examples, 1 stdlib)
- Consumer call sites changed: 24 qualified `option.foo(...)` calls
- Surviving call sites:       11 still spelling `opt_map`, `opt_filter`,
                              `opt_zip`, `opt_or` (intrinsic tests
                              + 4 example fixtures: `option_filter_basic.kai`,
                              `option_zip_basic.kai`, `option_or_basic.kai`,
                              and the option.kai self-tests).

### Files modified

```
stdlib/core/option.kai                          (definitions + tests)
stdlib/core/result.kai                          (7 sites — opt_is_some/none)
examples/stdlib/option_collect_basic.kai        (3 sites)
examples/stdlib/option_ok_or_basic.kai          (2 sites)
examples/stdlib/option_ok_or_else_basic.kai     (2 sites)
examples/stdlib/option_or_else_basic.kai        (3 sites)
examples/stdlib/option_unwrap_or_else_basic.kai (2 sites)
```

### Files NOT modified (deliberately)

- `examples/stdlib/option_filter_basic.kai` — keeps `opt_filter` (alias surviving #219).
- `examples/stdlib/option_zip_basic.kai`    — keeps `opt_zip` (alias surviving #219).
- `examples/stdlib/option_or_basic.kai`     — keeps `opt_or` (keyword block).
- `examples/sequence/pipe_*_rejected.kai`   — comment-only refs that quote
                                              the compiler's diagnostic text
                                              ("opt_and_then"); the diagnostic
                                              source in `stage2/compiler.kai`
                                              still emits that string, and
                                              `pipe_option_rejected.err.expected`
                                              is the golden. Rotating those
                                              touches stage2 (out of lane scope).
- `stage2/compiler.kai`                     — out of scope; uses local names
                                              `opt_escapes`, `opt_contains`,
                                              `opt_expr_has_interp_use` (private
                                              fns, distinct from stdlib exports)
                                              plus the legacy diagnostic strings.

## Compiler errors I encountered

### Class 1 — Typer EModCall ambiguity (#219; same shape as Phase 2)

After landing bare `pub fn map`, `pub fn filter`, `pub fn zip` in
`stdlib/core/option.kai`, the first `tier0` post-migration failed
with **28 type mismatches across every demo**. Sample:

```
error: type mismatch in function call
  --> demos/hello/main.kai:457:31
  = note: expected: (Option[?t9], (?t9) -> ?t10) -> Option[?t10]
  = note: found:    ([?t6], (?t6) -> ?t8 / ?e2) -> ?t11 / ?e3
```

Every `list.map`, `list.filter`, `list.zip` call across the entire
demo suite was being typed against `option.map`/`option.filter`/
`option.zip`'s scheme. This is exactly the gap Phase 2 (#220) hit
with `string.repeat` vs `list.repeat` and the gap documented at
`stage2/compiler.kai:22984` (EModCall branch in `synth_expr`,
which does a flat `ty_env_lookup(st.env, fname)` regardless of
the module qualifier).

Detection: a single `make tier0` run; the failure cascades through
every demo because `list.map` is ubiquitous. ~20 s feedback loop,
much faster than Phase 2 needed.

Resolution: kept `opt_map`, `opt_filter`, `opt_zip` as flat aliases
with inline comments pointing at #219. Three names instead of
Phase 2's one — three more entries on the post-#219 cleanup list
captured at the bottom of this report. **No new evidence filed
on #219**; this is the same shape Phase 2 already documented.

### Class 2 — Reserved-keyword collision: `or`

Independently of #219, `or` is a reserved keyword (`TkOr`).
`parse_fn_decl` (`stage2/compiler.kai:5110`) requires the function
name token to be `TkIdent`, so `pub fn or(...)` would not parse.
Detected by inspection while drafting the rewrite — never reached
the build. Left `opt_or` as a permanent alias with a one-line
explanation in the file header.

This is a NEW class of surviving alias, distinct from the #219
gap. It survives independently and would need a language-level
escape (back-tick-quoted identifiers, or a per-module name aliasing
syntax) to retire — not a typer bug. Not filing an issue: the
existing reserved-keyword design already implies this constraint.

## Friction points

### Did UFCS opportunism create wins?

Did not exercise UFCS in this lane. Same shape as Phase 1+2:
every consumer call already had explicit arguments
(`option.is_some(o)` not `o.option.is_some()`).

### Stage 2 compatibility — the `module_legacy_prefix` table

`stage2/compiler.kai:35834` has a per-module legacy-prefix map
(`option -> opt`, `char -> ch`) that lets `option.is_some(o)` resolve
to the historical `opt_is_some` definition. After this PR most of
the option entries are gone (the bare names are exported directly),
but the table entry STAYS in stage 2 because:

1. The 4 surviving aliases (`opt_map`, `opt_filter`, `opt_zip`,
   `opt_or`) still need it.
2. Editing stage 2 is out of lane scope.
3. The audit's PR 7 is the place to retire `module_legacy_prefix`
   entries (after the per-module migrations have all landed).

So the table remains accurate: `option.map` resolves through it
to `opt_map`, `option.filter` to `opt_filter`, etc., until #219
unblocks dropping those aliases.

### Diagnostic-text rot

`stage2/compiler.kai:24570` still prints "use `x!` for propagation
or `opt_and_then` for explicit chaining" when the user pipes an
`Option`. The function `opt_and_then` no longer exists — it is now
`option.and_then` (or, equivalently, the legacy resolver still
maps `option.and_then` to `opt_and_then`, but only because the
per-module prefix table still says `opt`; once that retires, even
the resolver mapping is gone and the diagnostic literally points
to a nonexistent name).

The corresponding `pipe_option_rejected.err.expected` golden also
still asserts the literal `opt_and_then` text. Both are
out-of-scope for this PR (stage 2 surface), but worth flagging
for the eventual PR 7 cleanup: the diagnostic should say
`option.and_then` once the prefix table retires, with a paired
golden update.

## Spec ambiguities or interpretive choices

### Naming the `or` survivor

The audit assumed every export would migrate to a bare name; it did
not anticipate keyword collisions. The choice between

- (a) keeping `opt_or` as a flat alias (today's choice),
- (b) renaming to `option.either` or `option.or_default`, or
- (c) using a quoted identifier (`option.\`or\``) once the language
  supports them

came down to *minimum disruption*. Option (a) keeps the function
discoverable under its historical name and matches the same-shape
fallback for `opt_map`/`opt_filter`/`opt_zip` (same file, same
header explanation). Renaming the semantic name would break code
across the codebase for purely-cosmetic reasons; quoted identifiers
do not exist in kaikai today.

### Tests inside option.kai

The intrinsic tests at the bottom of `option.kai` are run with
`KAI_NO_STDLIB=1`, so within that compile unit `is_some(x)` resolves
unambiguously to the local definition. Tests for the four surviving
aliases call them by their flat name (`opt_map`, `opt_or`, etc.) —
to do otherwise would require a top-level `import option as ...`
syntax that does not exist; bare `map(o, f)` would type-check
because `list.map` is also exported, repeating the same #219 gap.

## Subjective summary

**Confidence:** HIGH for the 9 migrated symbols. The 4 surviving
aliases are unavoidable given today's typer (3 of them) and the
keyword reservation (1 of them); inline comments + the file
header make the rationale explicit.

**Hardest:** Recognising the keyword issue BEFORE running tier0 the
second time. The first post-migration tier0 (FAIL) caught the
#219 collisions cleanly because the error count was huge and
specific. The keyword collision was caught at file-write time by
a five-second reading of `parse_fn_decl`; if I had not checked,
the parser would have given a "expected function name" error
that would have pointed at the wrong file (option.kai line 22 or
similar) and taken longer to diagnose.

**Easiest:** Migrating the 24 consumer call sites. A targeted perl
one-liner across 7 files was sufficient; the diff is mechanical.

**Compiler help:** The "type mismatch" cascade in tier0 was loud
and instantly traceable to #219. The pre-existing per-module
prefix table (`module_legacy_prefix`) made the migration
incrementally compatible — bare names that did not collide
landed on tier0 without any other change to stage 2.

**Compiler hinder:** None notable. `or` reservation is a known
design constraint. #219 is a known gap.

## Limitations of this report

- Did not file new evidence on #219 — every #219 case here is the
  same shape as Phase 2's `string.repeat` collision (qualified
  call resolved against the wrong module's scheme); listing three
  more instances would not change the fix sketch.
- Did not benchmark how long the post-#219 follow-up PR will take.
  Estimate: ~15 min (4 alias removals, 11 call-site updates in
  the 4 fixture files, plus the option.kai self-tests). The `or`
  alias is excluded from that estimate (separate language-feature
  block).
- Did not touch the `pipe_option_rejected.err.expected` golden or
  the diagnostic text in stage 2; both still mention `opt_and_then`.
  These belong to PR 6 / PR 7 of the audit (per-module legacy
  prefix retirement and stdlib-layout doc rewrite).

## Build/test TSV (appended)

```
timestamp	cmd	outcome	elapsed_s
2026-05-04T00:38:58-04:00	tier0-baseline	OK	39
2026-05-04T00:41:56-04:00	tier0-postmig	FAIL	20
2026-05-04T00:44:15-04:00	tier0-postmig2	OK	34
2026-05-04T00:48:47-04:00	tier1	OK	266
2026-05-04T00:49:46-04:00	tier1-asan	OK	50
2026-05-04T00:50:10-04:00	selfhost	OK	19
2026-05-04T00:50:29-04:00	selfhost-llvm	N/A-no-target	-
```
