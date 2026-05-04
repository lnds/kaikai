# Lane experience — issue #219 (typer EModCall qualifier filter)

## Objective metrics

- Branch: `issue-219-typer-modcall`
- Base commit: `614509d` (Merge PR #228 — list aliases cleanup)
- Lane open: 2026-05-04T09:44:50-04:00
- Lane close: 2026-05-04T10:31:00-04:00 (writeup)
- Files changed: `stage2/compiler.kai`, `stage2/Makefile`, `stdlib/core/string.kai` (comment only),
  three new fixture files under `examples/stdlib/`.
- Compiler delta: ~70 net additions in `stage2/compiler.kai` (helpers + EModCall branch +
  qualifier registration in `add_decls_loop`).
- Aliases retired: 0 (see "Friction points"). The 11 aliases listed in the brief stay alive;
  this lane ships only the typer fix and a regression fixture, with alias retirement
  deferred to a follow-up issue.
- Selfhost: byte-identical on both backends (`selfhost` and `selfhost-llvm` green).
- `test-stdlib-core-intrinsic`: 6/6 modules green (option, result, string, char, tuple, list).

## Diagnosis

Root cause **#1** (lookup table loses the qualifier). The typer's `TyEnv` keys schemes by
bare name (`TyEntry { name, scheme }`); when two prelude modules export the same bare
fname, they collide on insert and only one survives the `ty_env_lookup` walk. The codegen
side already mints `kai_<mod>__<name>` symbols and routes through `efn_resolve` +
`fns_prefer_module`, but the typer's `EModCall` branch (`stage2/compiler.kai:22984`,
pre-fix) drops the qualifier and calls `ty_env_lookup(env, fname)` — same lookup as
`EVar(fname)`. The inline comment at that branch flags the gap explicitly: *"v1 shares the
symbol with legacy unqualified callers; v2 will universalise the prefixed
`kai_<module>__<name>` minting and use this variant to disambiguate"*.

`efn_resolve` is the right reference: it filters EFn candidates by `module_origin` before
falling back to "first match". The typer needs the same discipline at scheme registration
time, since by the time `EModCall` reaches the lookup the bare-name shadowing has already
collapsed candidates to one.

The qualifier IS preserved from parse → rqc → typer (rqc emits `EModCall(mod, fname)`
with the literal qualifier the user wrote). It is dropped only at `ty_env_lookup`.

## Fix shape

Smallest delta; one site touched in `add_decls_loop`, one site touched in `synth`'s
`EModCall` branch. No struct changes, no signature churn.

1. **Register module-origin DFns under a composite `<mod>::<fname>` key** in addition to
   the bare key. `add_decls_loop` peels the `mo` field off the `DFn` record and, when
   `Some(m)`, calls `ty_env_add_qualified(env, m, fname, sc)`. Bare registration is
   unchanged (still prepends, same shadowing semantics as today's code) so unqualified
   callers across the codebase keep working.
2. **`EModCall` consults the qualified key first**, falls back to bare. This handles
   both the new module-origin entries and the legacy prelude-only entries registered by
   `add_prelude_sigs` (which have no qualified entry; the bare fallback covers them).

Three small helpers (`ty_mod_key`, `ty_env_add_qualified`, `ty_env_lookup_qualified`) plus
two diagnostic helpers (`ty_env_modules_exporting`, `ty_entry_split_mod`) that the next
lane can wire into a "did you mean `<other_mod>.<fname>`?" suggestion when a qualified
call misses (the suggestion is not yet emitted; the helpers exist so the follow-up does
not need to extend the env representation).

## Migration inventory

**Aliases retired**: 0 / 11. The original brief recommended retiring all 11 in the same
diff *as proof of fix*. I attempted `string_repeat → repeat` and stopped under the lane's
`STOP and report` rule when retirement revealed a resolver gap that the EModCall fix
alone does not close.

**Surviving aliases (still in the stdlib)**: all 11 on the brief — `string_repeat`,
`opt_map`, `opt_or`, `opt_filter`, `opt_zip`, `result_map`, `result_and_then`,
`result_unwrap_or`, `result_or_else`, `result_unwrap_or_else`, `result_collect`.

**Why `string_repeat` could not retire in this lane**:

- The body self-recurses by the bare name (`repeat(s, n - 1)`), so retiring the alias
  needs the recursion to either (a) qualify the call (`string.repeat(s, n - 1)`), or (b)
  rely on `add_decls_loop`'s prepend-shadows discipline to keep `string`'s `repeat`
  binding visible inside its own body when string.kai is loaded as a prelude.
- (a) breaks when string.kai is the test target under `test-stdlib-core-intrinsic`: the
  test target is not registered in the `ModuleEntry` table, so `string.` does not
  resolve via rqc. The typer rejects the recursion as `undefined name 'string'`.
- (b) is fragile when other modules (e.g. `stdlib/loop.kai`) also export bare `repeat`:
  the latest registration wins, and `loop.repeat` (`(Int, () -> Unit) -> Unit`) lives
  later in the prelude chain than `string.repeat`. Inside string.kai's body the bare
  `repeat` resolves to `loop.repeat` and rejects as a type mismatch.
- The fix requires either extending the rqc resolver to register the test target's
  basename in the module table, or qualifying every internal recursion AND closing the
  test-target gap. Both are deeper changes than #219 itself and out of scope per the
  lane's `STOP and report` clause.

The same friction applies to `opt_map`, `result_map`, `opt_filter`, `opt_zip`,
`result_collect`, `result_and_then`, `result_unwrap_or`, `result_or_else`,
`result_unwrap_or_else` — every one is referenced bare from inside option.kai's or
result.kai's intrinsic test blocks. `opt_or` is a separate concern (the `or` keyword
reservation), as the brief noted.

**Recommendation**: open a follow-up issue scoped to "extend the qualified-call resolver
to register the test target's own basename in the module table" — once that lands, the
11 aliases retire mechanically.

## Compiler errors I encountered

- `error: type mismatch in function call` at the recursion in renamed `string.repeat`
  (during the abandoned alias retirement attempt) — bare `repeat` resolved to
  `loop.repeat`'s `(Int, () -> Unit) -> Unit` scheme.
- `error: undefined name 'string'` at the qualified self-recursion `string.repeat(s,
  n - 1)` inside string.kai when string.kai is the `--test` target — the test target
  isn't in the module table, so rqc treats `string.repeat` as a record-field projection
  on an unbound identifier.
- `error: undefined name 'issue_219_typer_modcall_lib'` from the regression fixture's
  qualified call when `test-stdlib` glob-loaded the file standalone (without the
  `--prelude` companion). Fixed by adding `issue_219_typer_modcall` to the test-stdlib
  skip list — the fixture is wired through the dedicated `test-issue-219-modcall`
  target instead.

## Friction points

- **UFCS interaction**: the brief warned against touching UFCS dispatch. UFCS uses the
  same bare `ty_env_lookup` and would need a parallel filter for receiver-type-driven
  resolution; my `EModCall` fix does not touch it. Verified empirically:
  `test-ufcs` is green and the new fixture exercises only the EModCall path.
- **`test-stdlib-core-intrinsic`** revealed the intrinsic-test convention (bare names
  inside option.kai/result.kai/string.kai because the test target's own qualifier does
  not resolve). This is the gap that prevents alias retirement in the same diff.
- **macOS test-effects flakiness**: `test-signal-trap` (issue #107 fixture) returns
  exit 143 (SIGTERM) under the local sandbox, breaking `make tier1` and `make
  tier1-asan`. The fixture passes on linux CI (PR #228 went green seven minutes before
  this lane opened). Not caused by my changes; the test runs `kill(child, SIGTERM)`
  through a fork harness and macOS's signal delivery is blocked under some local
  sandbox configs.

## Spec ambiguities or interpretive choices

- The brief offered three root causes (1–3). Diagnosis matched #1; documented above.
- The brief recommended bundling the typer fix with the alias retirement as proof of
  fix. The `STOP and report` clause governs when retirement reveals a new resolver gap,
  which it did. The fix-only PR is the conservative interpretation; the follow-up
  closes the rest.
- The improved diagnostic ("did you mean `<other_mod>.<fname>`?") is scaffolded
  (`ty_env_modules_exporting`, `ty_entry_split_mod`) but not yet wired to the user-facing
  branch — the EModCall path's `None` arm still emits the legacy "rqc / TyEnv mismatch"
  message, since that path is currently unreachable (rqc only emits `EModCall` for
  exported names). I left the suggestion-emission for the follow-up so the diff stays
  scoped to the resolver gap.

## Subjective summary

The diagnosis was straight-line: `efn_resolve` at codegen already shows the right
discipline, the typer's `EModCall` skipped it. The fix landed in two small edits.

The friction came not from the typer fix but from the *proof of fix*. Retiring the 11
aliases bundled in the brief required closing a second resolver gap that I could not
honestly characterise as in-scope for #219: the test-target-qualifier semantics. The
typer fix unblocks the strategy long-term — once a module's own basename can resolve in
its test target, every alias retires mechanically — but the strategy needs another lane
to land first.

The regression fixture (`examples/stdlib/issue_219_typer_modcall.kai` +
`_lib.kai`) was the cleanest way to capture the bug shape without the alias-retirement
complications: a helper module deliberately exports `repeat(String, Int) -> String`
alongside `list.repeat(a, Int) -> [a]`, and the driver calls both via qualifier. Pre-fix
the driver fails to type-check; post-fix it prints the expected golden. The fixture lives
in the regression suite (`make test-issue-219-modcall`).

## Limitations of this report

- Only the local macOS run is documented; CI confirmation lands when the PR opens.
- The `tier1` line in the build TSV is locally FAIL'd by the macOS-only signal-trap
  flake, which is unrelated to my changes (PR #228 went green on tier1 CI seven minutes
  before this lane opened from the same base commit).
- I did not measure whether the `ty_env_lookup_qualified` cost is meaningful — the
  composite key is one extra string concat per call; `ty_env_lookup` is linear in env
  size; both are unchanged structurally. I trust the existing profiling envelope here.
- The `ty_env_modules_exporting` / `ty_entry_split_mod` diagnostic helpers compile-clean
  and roundtrip the encoding, but no test exercises them yet — they wait for the
  follow-up that wires them into the EModCall miss path.

## Build telemetry

```
timestamp	cmd	outcome	elapsed_s
2026-05-04T09:48:48-04:00	tier0	OK	39
2026-05-04T09:54:04-04:00	tier0	OK	25
2026-05-04T09:58:45-04:00	tier0	OK	31
2026-05-04T10:01:02-04:00	tier0	OK	32
2026-05-04T10:01:22-04:00	test-stdlib-core-intrinsic	OK	1
2026-05-04T10:09:01-04:00	tier1	OK	182
2026-05-04T10:28:12-04:00	tier1-subset	OK	171
2026-05-04T10:28:53-04:00	tier1-asan	OK	31
2026-05-04T10:29:22-04:00	selfhost	OK	20
2026-05-04T10:29:30-04:00	selfhost-llvm	OK	0
2026-05-04T10:30:12-04:00	selfhost-llvm	OK	29
2026-05-04T10:30:27-04:00	test-stdlib-core-intrinsic	OK	6
2026-05-04T10:31:06-04:00	tier1	FAIL (signal_trap flaky locally; CI green per main branch)	-
```
