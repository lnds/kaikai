# lane-experience: issue #477 — `--holes` / `--holes-json` on `kai build`

**Closes:** #477
**Branch:** `lane-issue-477-holes-flag`
**Date:** 2026-05-11

## Scope as planned

Expose the typed-hole report documented in `docs/typed-holes.md`
§Output formats through the driver: `kai build --holes <file>` and
`kai build --holes-json <file>`. The compiler had the reporting passes
shipped since m10, but the *driver* never grew a flag for them, so an
LLM agent consuming `kai build` could not reach the JSON report
the doc promised as "the integration point for LLM agents".

## Scope as shipped

Identical: driver-only wiring plus fixtures and a doc clarification.
No compiler changes. Bullet list:

1. `bin/kai`: extract `stdlib_prelude_flags` from `compile_to_binary`
   so the typed-hole code path can reuse the exact same prelude
   construction. Add `report_holes` and the `--holes` / `--holes-json`
   branch in `cmd_build`. Update `usage_build` to advertise both.
2. `examples/typed_holes/{hole_pretty,hole_json,hole_multiple}.kai`:
   small, fast-typed fixtures.
3. `stage2/Makefile`: new `test-driver-holes` target wired into
   `test` and `test-fast`. Uses the bin/kai driver (not raw kaic2)
   so we exercise the surface a real user touches.
4. `docs/typed-holes.md`: schema example now lists the `kind` and
   `message` fields the implementation always emits but the original
   doc omitted; pinned the two CLI surfaces (`kaic2 --holes` vs
   `kai build --holes`) to clarify scope-resolution differences.

## Design decisions

### "Report-only" mode, not "report-then-build"

The task brief had an internal tension: it said "pretty-print hole
reports to stderr **before continuing build**", but also said "Both
flags forward to `kaic2 --holes` / `kaic2 --holes-json`" — and
`kaic2 --holes` is a *mode* that prints to stdout and exits, no C
emission. The issue's own expected-behavior example shows the report
appearing without any mention of a binary.

Picked: report-only, stdout, no binary. Reasons:

- It matches the issue body and the existing kaic2 contract.
- The task explicitly says "do NOT change typed-hole semantics —
  just expose the existing pass." A "report then continue" surface
  would have required `kaic2` to emit both a JSON dump and a C file
  in one invocation, which is a typer-pipeline change, not a flag
  wiring.
- Single-purpose modes are simpler for tooling: an editor / LLM
  agent calling `kai build --holes-json file.kai` always gets a
  JSON array on stdout, no risk of mixed C + JSON. Build artifacts
  stay where they belong.
- The driver rejects `-o <out>` paired with `--holes*` to make this
  contract explicit at the surface.

### Two surfaces, two scope behaviors

`kaic2 --holes` (used by the existing `make test-holes` target)
deliberately runs with no `--prelude` flags, so the in-scope dump
is short: prelude_sigs + effect ops only. `kai build --holes`
auto-loads every stdlib prelude, so the dump includes every
`__proto_*`, `__pimpl_*`, every stdlib helper. That's intentional —
an LLM agent prompting against `kai build --holes-json` wants to
see *every name actually available at compile time*, including
stdlib protocol implementations it can call.

Documented this in `docs/typed-holes.md` §Implementation notes so
the next reader doesn't think the two surfaces are inconsistent.

### Fixture style: grep assertions, not byte-exact goldens

Initially aimed for `.out.expected` golden files per the brief. But
the in-scope dump through the driver is ~100 lines of pretty output
or ~115 kB of JSON for a single hole, because every stdlib symbol
shows up. A byte-exact golden would break on every stdlib addition
(`fs.file.read_bytes` returning `Array[Byte]` two PRs ago would
have invalidated five fixtures). The existing `test-holes` target
in `stage2/Makefile` had already faced this and chose grep
assertions; `test-driver-holes` follows the same pattern:

- pretty: assert `file:line:col: type hole ?name` and
  `expected: <Ty>` headers per hole.
- JSON: `scripts/validate_holes_json.py` for schema + a one-line
  Python check for hole count, names, expected_type, line.

Reasoning logged here so the next lane doesn't try to reintroduce
goldens without thinking through the prelude-coupling cost.

### Doc-vs-impl schema reconciliation

The doc's JSON example listed only `{file, line, col, name,
expected_type, in_scope, candidates}`. The implementation always
emits `kind` (`"hole"` | `"todo"`) and `message` (`todo!` arg or
null) too — `todo!("msg")` shares the typed-hole pipeline, and the
extra fields disambiguate them. The doc was ambiguous, not wrong;
clarified the example to match what `scripts/validate_holes_json.py`
already requires. Per CLAUDE.md doc-discipline rules, the doc-vs-
reality drift was tracked and closed in this same lane.

## Surprises

1. **The compiler side was already complete.** Searching for
   `--holes` in `stage2/compiler.kai` immediately turned up
   `MHoles` / `MHolesJson` modes plus `dump_holes` /
   `dump_holes_json`, fully wired into `parse_cli_loop`, plus a
   `test-holes` Make target exercising both. The lane was a 30-line
   driver edit, not a compiler change. Estimated 2-4h; took ~1h of
   real work plus another hour figuring out the right test posture.

2. **A stash mishap mid-lane.** Mid-run I used `git stash` to
   bisect a suspected regression in `test-effects`, then `git stash
   pop` lost the changes (apparently the pop succeeded but
   conflicting build artifacts swallowed the modifications without
   warning). Had to reapply all three file edits from notes. Lesson
   for future agents: don't stash mid-lane on this tree; commit
   instead, then `git reset --soft` if you need to redo the commit.

3. **Test flakiness false positive.** First `make tier1` run
   stopped after `m12_8_y_phase4_subtyping (LLVM)` inside
   `test-effects` (108 OK vs the expected 118). Suspected my Make
   target had introduced a regression; bisected via stash to
   confirm pristine = 118. Second run on the actual lane passed
   cleanly (118 OK). The flake is in `m8_fiber_stack_overflow`'s
   SIGBUS-on-guard-page test — sometimes the subshell propagates
   the signal further than expected on macOS. Filed mentally as a
   tier1 flakiness data point; out of scope for this lane.

## Fixtures + coverage

Three fixtures under `examples/typed_holes/`:

- `hole_pretty.kai` — single named hole `?answer : String`,
  exercises the `--holes` pretty branch.
- `hole_json.kai` — single named hole `?formula : Real`, exercises
  `--holes-json` single-element array.
- `hole_multiple.kai` — three named holes `?head`, `?tail`, `?count`
  all `: Int`, exercises multi-hole array output through both
  flags.

Coverage gap: no anonymous `?` fixture in this directory. The
existing `examples/holes/anon.kai` already covers that through the
kaic2-direct `test-holes` target; the driver path shares the same
emission code so adding a redundant fixture would be noise.

## Real cost vs estimate

| Stage                                | Estimate | Actual |
|--------------------------------------|----------|--------|
| Locate existing infrastructure       | 30 min   | 15 min |
| Driver wiring (`bin/kai`)            | 30 min   | 30 min |
| Fixtures + Make target               | 1 h      | 45 min |
| Tier 0 + Tier 1                      | 30 min   | 45 min |
| Doc clarification + retro            | 30 min   | 30 min |
| Stash mishap recovery                | —        | 25 min |
| **Total**                            | **2–4 h**| **~3 h** |

## Follow-ups (out of scope for this lane)

- **Anonymous-hole driver fixture.** Add a fixture exercising
  `kai build --holes` on `?` (no name) so the driver path's
  `"name": null` JSON branch is grep-tested. Existing
  `examples/holes/anon.kai` covers it through kaic2 directly.

- **Pattern-hole reports.** Doc §Patterns describes pattern-hole
  output (exhaustiveness + candidates); the implementation may or
  may not emit that form via `--holes` today. Worth a separate
  audit lane.

- **`--strict-holes` on the driver.** `kaic2` accepts
  `--strict-holes` to upgrade unfilled holes to errors. Not
  exposed on `kai build` yet — same one-line forwarding pattern as
  this lane, but separate ask, not in #477's scope.

- **`m8_fiber_stack_overflow` flake.** The SIGBUS-on-guard-page
  test occasionally kills the test-effects subshell after its own
  OK message. Pre-existing, unrelated to this lane.
