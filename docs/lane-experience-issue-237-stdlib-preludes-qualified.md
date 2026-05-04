# Lane experience — issue-237-stdlib-preludes-qualified

## Objective metrics

- Branch: `issue-237-stdlib-preludes-qualified` off `main` @ `80af75b`
  (post-#236).
- Files changed: `stage2/compiler.kai` (legacy-prefix table extended),
  `Makefile` (`test-multi-module` iterates over fixture cases),
  `examples/multi-module/issue-237/` (new fixture: `main.kai`,
  `util.kai`, `main.out.expected`).
- Lines: +24 / -9 net across compiler + Makefile.
- Selfhost C: byte-identical. Selfhost LLVM: byte-identical.
- Tier 0: OK (49 s). `test-stdlib-core-intrinsic`: OK across all 6
  core modules (7 s). `test-multi-module`: 2/2 cases green (the
  pre-existing `multi-module` + the new `multi-module/issue-237`).
- Tier 1 / Tier 1-ASAN locally: same preexistent macOS sigharness
  flake reported by the #233 retro (Error 143 / SIGTERM in
  `test-effects` and `test-signal-trap-asan`). Selfhost byte-identical
  + per-target subsuites green tells us the diagnostic is unrelated
  to this fix; CI on Linux is the final arbiter.

## Diagnosis

The issue body listed three hypotheses and asked the agent to
identify which one (or combination) was the actual root cause. None
of them turned out to be the root cause as stated.

**The original repro from issue #237 does not reproduce on `main`
HEAD post-#236.** `cd /tmp/exp237 && bin/kai run main.kai` (with the
math.kai sibling) prints `49` directly. `kai test`, `kai bench`,
`kai build` all succeed. Internal stdlib qualified calls
(`stdlib/random.kai` calling `list.nth`, `list.is_empty`; same in
`stdlib/regexp.kai`; collections etc.) resolve correctly through the
existing `compile_source` two-pass structure: `load_preludes`
populates `pmods` for every `--prelude` file in argument order
*before* `compile_source` runs `rqc_decls` on either the prelude
bodies or the user's body, so the module table is complete when
either pass walks an `EField(EVar(mod), fn)` site. The hypothesised
"register first, type-check second" restructuring was already in
place via #218 (`rqc_decls(prelude_decls, module_table, path)` at
`stage2/compiler.kai:41372`).

What *did* reproduce on `main` HEAD was a narrower gap: user code
calling into a non-core stdlib module whose exports use a legacy
short prefix that does not match the module's basename. Two
concrete examples:

- `regexp.compile("[a-z]+")` — `stdlib/regexp.kai` exports
  `regex_compile`, not `regexp_compile`. `me_lookup_export`
  (`compiler.kai:35925`) tries the verbatim `compile`, then falls
  back to `<mod>_<fname>` = `regexp_compile`. Neither is exported,
  so the rewrite emits `module 'regexp' does not export 'compile'`
  and the typer cascades to `undefined name 'regexp'`.
- `decimal.from_int(42)` — `stdlib/decimal.kai` exports
  `dec_from_int`. Same failure shape.
- `random.shuffle(xs)` — works; `random.kai` exports `shuffle`
  bare, so the verbatim lookup hits before the prefix fallback runs.
- `char.is_digit(c)` — works; `char` already had a per-module
  override (`module_legacy_prefix(\"char\") -> Some(\"ch\")`).

So the bug pattern is "non-core module whose function names use a
legacy short prefix that the resolver does not know about." The
resolver had a single-entry override table covering only `char`;
two more entries were missing.

This was the gap the issue body called the "residual" when it said
PR #236 fixed the `bin/kai` driver path but a residual surfaced.

## Fix shape

Smallest delta. Three additions:

- `module_legacy_prefix` now returns `Some("regex")` for `mod_name
  == "regexp"` and `Some("dec")` for `mod_name == "decimal"`. Same
  shape as the existing `char -> ch` override; no other code path
  touched.
- New regression fixture `examples/multi-module/issue-237/`
  (`main.kai` + `util.kai` + `main.out.expected`) exercises the
  multi-module + qualified-call chain into both `regexp.*` and
  `decimal.*`.
- `test-multi-module` recipe in the root `Makefile` iterates over
  both the existing `multi-module/` and the new
  `multi-module/issue-237/` cases. No new make target — extends
  the one #236 added so the fixture rides the same tier1 gate.

No restructuring of `compile_source`, `rqc_decls`, or
`expand_imports` was required — they already do the right thing.

## Whether #235 also closes

**No.** #235 is independent.

The fix here changes the qualified-call resolver
(`me_lookup_export`'s legacy-prefix table). #235 is a typer-level
gap: `ty_env_lookup` and `synth_ufcs_dispatch` resolve bare
identifiers (without qualifier) by load order rather than by
receiver type. Retiring `opt_map -> option.map` removes the bare
`opt_map` alias, leaving `option.map` as the only bare `map` from
`option.kai` — but `list.kai` also exports a bare `map`, and the
typer cannot disambiguate `xs.map(f)` (UFCS, no qualifier) by
receiver type.

I did not retire any aliases. The briefing said "If retirement
still fails, leave #235 open as a separate follow-up." The
diagnosis above shows retirement *would* still fail; I left the
10-alias migration for the lane that solves the typer's bare-name
resolution.

## Migration inventory

None. No aliases retired. The 11 surviving aliases (`opt_map`,
`opt_filter`, `opt_zip`, `opt_or`, `result_map`, `result_and_then`,
`result_unwrap_or`, `result_or_else`, `result_unwrap_or_else`,
`result_collect`, `string_repeat`) are all blocked by #235 and
remain in place.

## Compiler errors I encountered

None. The fix was a 4-line edit to a static table; the compiler
parsed it cleanly on first pass, `make all` succeeded, the
existing fixtures held byte-identical selfhost, and the new
fixture printed the expected output on first run.

## Friction points

- **Misleading repro**. The issue body's repro (`math.kai` +
  `main.kai` calling `math.square(7)`) prints `49` directly on
  `main` HEAD post-#236. I burned ~10 minutes confirming this from
  multiple angles (different cwd, fresh tmp dir, full clean
  rebuild) before broadening the search to find the actual
  failing pattern. Suggests the issue was filed speculatively
  during the #236 lane retro and the repro never validated against
  the merged tree.
- **Pre-existing macOS flake**. The `test-effects` /
  `test-signal-trap-asan` SIGTERM (Error 143) on macOS is the same
  flake the #233 retro already documented. Tier 0 + selfhost (C +
  LLVM) byte-identical + `test-stdlib-core-intrinsic` + the
  fixture-bearing `test-multi-module` all green is a strong local
  signal; CI on Linux will be the final word.
- **Hypothesis tunnel-vision risk**. The briefing pointed firmly
  at "two-pass restructure: register first, type-check second" as
  the most likely fix shape. The actual gap was a 4-line table
  extension. Following the briefing's hypothesis without
  re-deriving from the empirical repro would have produced a
  larger PR for a smaller bug.

## Spec ambiguities or interpretive choices

- **Closing #237 with a non-restructure fix**. The briefing's
  framing implied the agent might find a multi-pass restructure;
  what I found was a static-table gap. I closed the issue against
  the *user-visible* symptom (`regexp.compile` and `decimal.*`
  now work) rather than the briefing's hypothesised cause.
- **Fixture scope**. The briefing suggested
  `examples/multi-module/` *or* a new directory. I picked
  `examples/multi-module/issue-237/` so the fixture inherits the
  pre-existing recipe with a single-line for-loop tweak instead
  of a new make target.
- **No `random.choice` fixture**. `random.choice` already works
  through the verbatim-fname path (no prefix mismatch). The
  fixture exercises `regexp.compile` + `regexp.find_all` (which
  needed the new `regexp -> regex` override) and
  `decimal.from_int` (the new `decimal -> dec` override) so each
  added entry has direct coverage.

## Subjective summary

Fifteen-minute fix with twenty-five minutes of diagnostic work to
confirm the briefing's mental model didn't match the actual repro.
The right move was to ignore the multi-pass-restructure framing
once the literal repro worked, then ask the empirical question
"what *does* break in user code that calls a non-core stdlib?" —
which led directly to `me_lookup_export`'s prefix table. Honest
framing call: the fix closes the user-visible failure mode the
issue title promised, even though the internal cause was a
narrower table gap and not the "prelude module-table populated
before body type-check" hypothesis the briefing pinned.

## Limitations of this report

- Only macOS local signal: tier 1 + tier 1-ASAN flake on
  preexisting `sigharness` SIGTERM. CI on Linux required for the
  final tier1 / tier1-asan verdict.
- I did not exhaustively probe every non-core stdlib for further
  prefix mismatches. `regexp` and `decimal` were the two I
  confirmed; the audit script in lane diagnosis flagged
  `random_secure`, `money`, `uuid`, `path` as already-aligned.
- The fix does not address #235. UFCS by receiver type stays
  open as separate scope.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s	notes
2026-05-04T14:25:29-04:00	tier0	OK	49	selfhost byte-identical, demos baseline holds
2026-05-04T14:28:43-04:00	tier1	FLAKE	186	test-effects SIGTERM (preexistent macOS sigharness flake; unrelated to fix)
2026-05-04T14:35:04-04:00	test-stdlib-core-intrinsic	OK	7	all 6 core modules pass
2026-05-04T14:35:51-04:00	selfhost-llvm	OK	27	llvm fixed point byte-identical
2026-05-04T14:36:30-04:00	tier1-asan	FLAKE	31	test-signal-trap-asan SIGTERM (same flake)
2026-05-04T14:40:00-04:00	tier1-retry	FLAKE	147	same flake reproduces
```
