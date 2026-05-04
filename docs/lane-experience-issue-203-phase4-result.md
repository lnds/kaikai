# Lane experience — issue-203-phase4-result

PR 4 of the 7-PR m14 milestone (audit `docs/m14-bootstrap-audit.md`,
PR #214). Migrates `stdlib/core/result.kai` user-facing exports
from flat `result_*` to module-relative `result.*` form, sweeps
every consumer in `examples/stdlib/`, `examples/ufcs/`, and updates
the UFCS golden that mentions `result_is_ok`.

## Objective metrics

- Start (lane init):     2026-05-04T01:13:33-04:00
- End (retro written):   2026-05-04T01:37:23-04:00
- Wall clock:            ~24 min
- Build/test invocations:
  - `tier0`:                       4 (baseline OK, post-mig-1 FAIL,
                                      post-mig-2 OK, post-mig-3 OK)
  - `test-stdlib-core-intrinsic`:  2 (one transient FAIL on
                                      result-target unwrap_or runtime
                                      mismatch, one OK after restoring
                                      flat aliases)
  - `tier1`:                       2 (one FAIL on UFCS chain_basic
                                      after #219 collisions, one OK)
  - `tier1-asan`:                  1 OK
  - `selfhost`:                    1 OK
  - `selfhost-llvm`:               not a make target in this tree

See appended TSV for the full log.

## Migration inventory

`stdlib/core/result.kai` exports today (flat-name surface, before lane):

```
pub fn result_is_ok
pub fn result_is_err
pub fn result_map
pub fn result_and_then
pub fn result_unwrap_or
pub fn result_map_err
pub fn result_or_else
pub fn result_unwrap_or_else
pub fn result_ok
pub fn result_err
pub fn result_transpose
pub fn result_collect
```

12 exports total.

### Counts

- Bare-name exports landed:    6
  - `is_ok`, `is_err`, `map_err`, `ok`, `err`, `transpose`
- Flat aliases surviving:      6 (all #219-blocked)
  - `result_map`             — `map` collides with `list.map`
  - `result_and_then`        — `and_then` collides with `option.and_then`
  - `result_unwrap_or`       — `unwrap_or` collides with `option.unwrap_or`
  - `result_or_else`         — `or_else` collides with `option.or_else`
  - `result_unwrap_or_else`  — `unwrap_or_else` collides with `option.unwrap_or_else`
  - `result_collect`         — `collect` collides with `option.collect`
- Consumer files modified:     12 (7 examples/stdlib, 4 examples/ufcs,
                                   1 UFCS .err.expected golden)
- Consumer call sites changed: ~17 qualified `result.foo(...)` calls
                               in `examples/stdlib/` + 10 UFCS
                               method-name renames (`result_is_ok` →
                               `is_ok`, `result_map_err` → `map_err`)

### Files modified

```
stdlib/core/result.kai                            (definitions + tests)
examples/stdlib/result_collect_basic.kai          (3 sites; collect kept FLAT-aliased)
examples/stdlib/result_err_basic.kai              (2 sites + local rename `err` → `bad`)
examples/stdlib/result_map_err_basic.kai          (2 sites)
examples/stdlib/result_ok_basic.kai               (2 sites + local rename `ok` → `good`)
examples/stdlib/result_or_else_basic.kai          (3 sites)
examples/stdlib/result_transpose_basic.kai        (3 sites)
examples/stdlib/result_unwrap_or_else_basic.kai   (2 sites)
examples/ufcs/basic.kai                           (2 sites: `r.is_ok()`)
examples/ufcs/chain_basic.kai                     (chain `r.result_map().map_err().is_ok()`)
examples/ufcs/no_partial_application.kai          (1 site: `r.is_ok`)
examples/ufcs/no_partial_application.err.expected (golden update)
examples/ufcs/postfix_interaction.kai             (3 sites: `produce().map_err(...)`)
```

### Files NOT modified (deliberately)

- `examples/sequence/pipe_result_rejected.kai`   — comment-only ref
                                                   to `result_and_then`
                                                   (the alias survives;
                                                   diagnostic source
                                                   in stage 2 still
                                                   emits that string).
- `stage2/compiler.kai`, `stage1/compiler.kai`   — out of lane scope.
                                                   The `result_and_then`
                                                   diagnostic text at
                                                   `stage2:24549` and
                                                   `:24575` is correct
                                                   today (alias still
                                                   exists); it would
                                                   need rewording when
                                                   the alias retires
                                                   under PR 6/7.

## Compiler errors I encountered

### Class 1 — Typer EModCall ambiguity (#219; same shape as Phase 2 + 3)

After landing all 12 bare exports in `stdlib/core/result.kai`, the
first post-migration `tier0` cascaded: **28 demos failed**. Sample:

```
error: type mismatch in function call
  --> demos/hello/main.kai:457:31
  = note: expected: (Result[?t9, ?t10], (?t10) -> ?t11) -> Result[?t9, ?t11]
  = note: found:    ([?t6], (?t6) -> ?t8 / ?e2) -> ?t12 / ?e3
warning: bare name 'map' is exported by multiple modules with no root-file shadow: list, result; use a qualified call (e.g. list.map(...)) to disambiguate
```

`list.map(...)` calls were typing against `result.map`'s scheme.
Same gap as Phase 2 (#220, `string.repeat` vs `list.repeat`) and
Phase 3 (`opt_map` etc.). Identical shape; no new evidence to file
on #219.

Resolution: kept `result_map` and `result_collect` as flat aliases
with inline comments pointing at #219.

### Class 2 — Runtime non-exhaustive match in `test-stdlib-core-intrinsic`

After Class 1, demos+tier0 went green but the intrinsic harness
panicked: when `result.kai` is the test target, every other core
file (including `option.kai`) is loaded as `--prelude`. `option`
exports bare `unwrap_or`, `and_then`, `or_else`, `unwrap_or_else`,
and `collect` since Phase 3. Inside result.kai's tests (which had
landed bare `unwrap_or` etc.), the resolver picked one scheme by
name; the calls compiled but at runtime the option implementation
ran against a Result value, leading to:

```
ok   result_map on Err leaves the error untouched (bottom)
panic: non-exhaustive match
make: *** [test-stdlib-core-intrinsic] Error 1
```

This is the "test-stdlib-core-intrinsic gotcha" the lane brief
warned about, with a worse failure mode than Phase 3's: not a
compile error pointing at a missing qualifier, but a runtime panic
*inside* a passing test sequence. Detection took one harness run
(~1 s).

Resolution: restored 4 more flat aliases (`result_and_then`,
`result_unwrap_or`, `result_or_else`, `result_unwrap_or_else`) and
rewrote the affected intrinsic tests to call them by their flat
name. After this, the harness reports `OK` for all 6 core modules.

### Class 3 — Local-binding shadow vs bare export (`ok` / `err`)

Three example fixtures bound a local `let ok : Result[…]` or
`let err : Result[…]`, then called `result.ok(ok)` /
`result.err(err)`. The qualified call resolves through
`me_lookup_export` to the bare export `ok`/`err`. **But** the
typer (or some downstream lookup) appears to still attribute the
binding scope at the field-position parser level: the error reads

```
error: type mismatch in function call
  --> result_err_basic.kai:13:26
  = note: expected: Result[String, Int]
  = note: found:    (Result[String, Int]) -> ?t8 / ?e2
```

i.e. the typer sees the *function* `err` (arg type `Result -> ?`)
where it expects the local *value* `err: Result`. Argument-position
resolution looks like it's flipping bindings → fns somewhere, even
though the call site was clearly `result.err(<arg>)`.

Detection: tier1 (this came after tier0+harness were green; the
fixtures only run under `test-stdlib`).

Resolution: renamed `let err` → `let bad`, `let ok` → `let good` in
the two affected fixtures. Out of lane scope to chase the resolver
behaviour; the rename is purely cosmetic and unblocks the lane.

This *might* be a real resolver bug worth filing under #203's
follow-ups (the qualified-call path should not consult ident-
shadowing scope for the argument list separately from the callee).
Not filing in this lane per "if you find a NEW resolver/typer gap,
STOP and report" — flagging here for the integrator.

### Class 4 — UFCS receiver lookup is name-based

`r.map(...)` in `examples/ufcs/chain_basic.kai` (with `r: Result`)
failed because UFCS rewrites to `map(r, ...)` and consults the
in-scope free-fn name. After the migration, bare `map` exists only
on `list` — and the receiver is a `Result`, so lookup found no fit:

```
error: method `map` not found for type `Result`
  --> examples/ufcs/chain_basic.kai:13:16
```

Detection: tier1 (tier0 doesn't exercise this fixture). Resolution:
spelled the chain `r.result_map((x) => x + 1).map_err(...).is_ok()`
— the surviving flat alias is the only name that today resolves to
`Result`'s `map`-shape under UFCS. UFCS-with-#219 has no clean
spelling for "the Result version of map" until the typer is fixed.

## Friction points

### Did UFCS opportunism create wins?

The 4 ufcs fixtures (`basic`, `chain_basic`, `no_partial_application`,
`postfix_interaction`) DID exercise UFCS; renaming method names was
mechanical. No new UFCS coverage opportunities surfaced — every site
already had the right shape.

### test-stdlib-core-intrinsic gotcha — DID bite (Class 2)

The lane brief explicitly flagged the harness gotcha, but the
manifestation here was new: not a compile error from "qualifier
doesn't resolve when target is the test source", but a *runtime*
non-exhaustive match because option's scheme silently won the
bare-name resolver tie. The brief's example was about
`result.is_ok(r)` failing to qualify; what actually broke was bare
`unwrap_or(r, dflt)` in result's own tests dispatching to option's
implementation.

The fix (use flat aliases for collision-prone names in cross-target
test asserts) is the same shape Phase 3 PR #222 documented;
Phase 4 caught it on the first harness run thanks to that prior
art and the brief's heads-up.

### #219 collisions with `option`

Phase 3 created collisions for *5* bare names by promoting option's
flat exports (`and_then`, `unwrap_or`, `or_else`, `unwrap_or_else`,
`collect`). All 5 land as #219 blockers in Phase 4 because `result`'s
twins occupy the same names. Phase 5 (list) and Phase 6 (rename)
will need to decide whether to keep the per-module legacy prefix
fallback or finally fix #219.

### Spec ambiguities or interpretive choices

#### Intrinsic-test naming convention

The pre-migration tests were named after the flat fn (`test "result_map on Ok"`).
Tests calling bare names got renamed (`test "is_ok / is_err on Ok"`),
tests calling surviving flat aliases kept their flat name. This
keeps the test name and the assert call site visually consistent
but means the test names mix bare and flat conventions in the same
file; subjectively fine since the per-test header explains what's
being exercised.

#### `let ok` / `let err` rename

Section "Class 3" documents this. Could have spelled the call
`result.ok(my_ok)` etc. instead of renaming the binding; chose the
binding rename because it's a smaller diff and avoids burying the
shadow workaround in a comment.

## Subjective summary

**Confidence:** HIGH for the 6 migrated symbols. The 6 surviving
aliases are all #219-blocked; the rationale is in the file header
+ an inline comment per export. No keyword collisions in `result`
(unlike `or` in Phase 3).

**Hardest:** Class 3 (the local-binding shadow). The error text
("expected `Result`, found `(Result) -> ?t`") was specific enough
to recognise but pointed at the call site, not at the shadow. Took
~3 min to recognise; could be a future #219 follow-up.

**Easiest:** The bare-name migration of the 6 non-colliding exports.
Mechanical replace, no surprises.

**Compiler help:** The disambiguation warning ("bare name `map` is
exported by multiple modules") is a perfect signpost for #219
collisions; without it Class 1 would have been harder. The
intrinsic-harness panic message ("non-exhaustive match" + the
preceding test name) was enough to locate Class 2 in one read.

**Compiler hinder:** Class 3 (the local-binding shadow on `ok` /
`err`) feels like a resolver inconsistency — argument-position
resolution should not weaken a qualified call's named lookup.

## Limitations of this report

- Did not file new evidence on #219 — collisions for `map`,
  `and_then`, `unwrap_or`, `or_else`, `unwrap_or_else`, `collect`
  are the same shape Phase 2/3 already documented.
- Did not file new evidence on the Class 3 resolver behaviour. The
  lane brief is explicit about NOT chasing new resolver/typer gaps;
  flagging in §"Class 3" so the integrator can decide whether to
  open an issue.
- Did not benchmark the post-#219 cleanup PR's expected size for
  result. Estimate: ~10 min (6 alias removals; ~5 example call
  sites still spelled `result_*`; the intrinsic tests revert to
  bare names; `pipe_result_rejected` golden untouched).
- Did not touch `stage2/compiler.kai`'s diagnostic strings
  (`result_and_then` at lines 24549/24575); those follow the alias
  to PR 6/7.

## Build/test TSV (appended)

```
timestamp	cmd	outcome	elapsed_s
2026-05-04T01:14:33-04:00	tier0-baseline	OK	39
2026-05-04T01:17:46-04:00	tier0-postmig	FAIL	20
2026-05-04T01:19:04-04:00	tier0-postmig2	OK	34
2026-05-04T01:22:26-04:00	tier0-postmig3	OK	33
2026-05-04T01:22:26-04:00	test-stdlib-core-intrinsic	OK	3
2026-05-04T01:35:33-04:00	tier1	OK	268
2026-05-04T01:36:26-04:00	tier1-asan	OK	48
2026-05-04T01:36:49-04:00	selfhost	OK	18
2026-05-04T01:36:55-04:00	selfhost-llvm	N/A-no-target	-
```
