# Local tier1 sweep — PR #2682 (`symid-names-by-type`)

Tree: `a9a24ed80` (C compiler built by `make kaic1 kaic2` from the release boot v0.139.7, macOS arm64, `cc`).
Baseline for "fails on main": `<S>/mainpure-wt` (2490193b), same target / same command, measured.
Logs: `<S>/sweep/{light,root,heavy,nscol,tier0,asan,tsan}/<target>.log`, one `RESULTS` file per directory
(`<rc> <target> <seconds>`); main-side runs in `<S>/sweep/main/`.

How it was run: every make target of every shard as its OWN `make -k` invocation (a `make -k tier1-shard-N`
stops the shard's recipe at its first failing line, so the shard recipes were unrolled): the 294 targets of
`stage2/test-lists/light.txt` (all 8 light slices), each shard's fixed work, every `tier0` prerequisite, the
81 ASAN legs, the tier1-tsan gates. `test-sugars` / `test-stdlib` are per-fixture pattern targets, so `-k`
ran every fixture; `test-stdlib`'s own recipe body was re-run past the failed fixture (passes).

## C backend — failures (7 fixtures, 5 targets)

| # | suite / target | fixture | actual vs expected (first lines) | fails on main | cause (pass / file) | owner |
|---|---|---|---|---|---|---|
| 1 | `test-compiler-intervals` (light) | `stage2/tests/test_intervals.kai` | `type mismatch … expected: (IntervalEnv, LocalId, Interval) -> IntervalEnv / found: (IntervalEnv, String, Interval)` at `ie_extend(…, "x", …)` / `ie_lookup(env, "x")` | no | harness: the unit test still keyed the interval env by spelling | sweep — FIXED `f8464b76` |
| 2 | `test-independence-oracle` (shard 5) | `examples/oracle/lane1/benign.kai` (+ `adversary.kai`) | `oracle SKIP lane1 — both sides failed to typecheck` / `error: cannot find \`tag\` in this scope --> benign.kai:22:28` (the site is `lib/lib.kai:22`, `tag("hi")` with two imported candidates) | no | `--dump-typed-core` (and `--infer`, `--dump-typed`, `--dump-last-use`, `--dump-intervals`, `--dump-mono`, `--dump-mono-out`, `--dump-rawsafe`, `--dump-borrowsafe`, …) type through `infer.kai infer_program`, which passes `fv_empty()` views: a bare name the typer settles on the reader's ladder has no ladder in an imported module. Plain build / `--check` are fine. | sweep — FIXED `1fc133f9` (driver + report entries); `emit_c.kai dump_mono` / `dump_mono_out` left to the emitter owner (below) |
| 3 | `test-negative` (shard 7) | `negative/diagnostic_quality/prelude_call_anchors_at_user` | got `= note: in argument 1 of \`@core.print\``, want `in argument 1 of \`print\`` | no | `infer.kai decl_shown`: a builtin's owner is the synthetic `@core` module, which is neither the reader's home nor `sv_is_core` → qualified as `@core.` | typer agent (same cause as #5) |
| 4 | `test-sugars` (light) | `sugars/contract_method_effectful_rejected` | got ``calls effectful function `.loud` ``, want `` `n.loud` `` | no | known — typer agent | typer agent |
| 5 | `test-sugars` (light) | `sugars/m12_6_uom_refinement` | got `in argument 1 of \`@core.real_to_string\``, want `` `real_to_string` `` | no | known — `infer.kai decl_shown` | typer agent |
| 6 | `test-sugars` (light) | `sugars/pipe_map_genuine_mismatch` | got `in argument 2 of \`map\``, want `` `list.map` `` | no | known — typer agent | typer agent |
| 7 | `test-stdlib` (light) | `stdlib/row_kind_homonym_core_pair` | `kaic2 errored`: `found: (row_kind_homonym_core_pair.Pair) -> ?t10 / union: … = Pair / in argument 1 of \`swap\`` | no | known — record literal head, `infer.kai` | typer agent |

Everything else on the C backend is green:

- light pool: 291 of 294 targets pass (the 3 red ones are rows 1, 4–7);
- shard fixed work: `test-tokens/-ast/-types/-env/-check`, `test-heap-limit`, `test-user-cache`,
  `test-core-cache`, `test-modular-selfhost`, `test-perceus-1131-modular-escape`, the three
  `test-fmt-property` thirds, `test-fmt`, `-fmt-package`, `-fmt-width`, `-fmt-selfhost`, `-fmt-help-scope`,
  `test-packages`, `test-editions`, `test-info`, `test-partition-linearity`, `test-binserialize-linearity`,
  `test-core-text`, `test-http-redirects`, `demos-no-regression`, the whole CLI tail except `test-negative`;
- namespace-collision axes: c 2/349, neg 2/115, modular 2/157, neg-modular 2/36 — each AT its baseline (the
  known pre-existing fixtures); diag 0/48, testsym 0/1, check 0/1; `test-namespace-matrix` OK;
- `tier0`: all 49 prerequisites pass, `selfhost` and `test-symid-names` included;
- root `test` extras: `test-stage0`, `test-stage1`, `test-demos`, `test-multi-module`, `test-import-*`,
  `test-mn-determinism`;
- ASAN: 81 of 81 fixture legs (A: 19, B: 62);
- working tree clean after the sweep.

Also green on C: the tier1-tsan gates (`test-mn-tsan`, `test-mn-reactor-bench`, `test-mn-idle-cpu`,
`test-mn-deadlock-banner`, `test-segment-runtime-tsan`) and `tier1-asan-a` end to end (demos under ASAN + legs).

## Native backend (`make KAI_LLVM=1 kaic2`, Homebrew LLVM 22.1.7) — no new failure

Built on `a9a24ed80` + the unit-test fix + the views-only version of the report-mode fix (later amended into `1fc133f9`). Logs: `<S>/sweep/native/`, `native-rerun/`,
`nmisc/`.

- `test-native-wrapper` list (42), `native-gates-1` (12), `native-gates-2` (97), the tier1-native extras
  (`test-user-cache`, `test-core-cache`, `test-mn-native-soundness`, `test-mn-c-clang-soundness`,
  `test-native-core-obj-bc-state`, `test-perceus-747-native-real-slot-uaf`, `test-perceus-nested-reuse-asan-native`,
  `test-monitor-ref-asan-native`, `test-perceus-asan-native`, `test-trap-pads-asan-native`,
  `test-kai-builtins-asan-native`) and the 19 light targets that SKIP on a C-only compiler: 178 targets, all pass.
  Four went red in the parallel pool (`test-core-cache`, `test-native-1083-inline-gate`,
  `test-core-homonym-native`, `test-perceus-nested-reuse-asan-native`: "runtime_inline.bc missing") because
  `test-native-core-obj-bc-state` hides the runtime bitcode while it runs and CI gives it a step of its own;
  all four pass re-run alone (`native-rerun/RESULTS`).
- `test-namespace-collisions-native`: 2 of 341 failing at baseline 2. `-neg-native`: 2 of 102 failing, baseline 3
  ("OK (improved)", the harness suggests writing 2 to `tools/baselines/nscol/neg-native`; not changed here —
  measured on macOS only).
- parity ratchets (`NATIVE_PARITY_RATCHET=1`): shard 1 pass=357 fail=0 skip=42; shard 1b pass=444 fail=0 skip=34;
  shard 2 pass=293 fail=1 skip=36 — the one is `stdlib/row_kind_homonym_core_pair` (row 7: the C oracle does not
  build it, so the ratchet does not count it; it will stay red in `test-stdlib` until the typer fix).
- shard-2 scripts: `test-http-redirects KAI_TEST_BACKEND=native`, `tests/native_diag_path.sh`,
  `tests/native_emit_jobs.sh`, `tests/broken_import_paths.sh`, `RC_BUDGET_BACKEND=native tools/rc-budget-gate.sh`.
- `tools/test-native-selfhost-gate.sh`: compile + link + run + byte-identical self-compile.
- `tools/run-mn-corpus-determinism.sh` (whole corpus, both backends, N=1 vs N=4): 2180 pairs judged, 0 findings;
  not judged: `row_kind_homonym_core_pair` (does not build), 4 signal fixtures (wait for a signal), 4 pinned.

## Phase 2 — fixed here (branch `agent-acda8bf01042a51ad`, on top of `a9a24ed80`)

- `f8464b76` test(intervals): the unit tests key the interval env by `LocalId` (row 1).
- `1fc133f9` fix(driver): report modes type through `infer_entry.kai infer_report(file, decls, tab, views, protos,
  verbose)` with the driver's `sym_views` and `pv_of(lowered.reg)` (row 2). New light target
  `test-report-modes-imports` (7 modes x 2 shapes; red on the lane compiler without the fix).

Final tree re-verified with a C-only `make kaic2`: light pool 293 of 295 (red: `test-sugars` rows 4-6,
`test-stdlib` row 7), every root target but `test-negative` (row 3), shard fixed work, namespace axes at
baseline, all 50 tier0 targets incl. `selfhost`, `test-symid-names`, `test-km-new-files` (`infer_entry.kai` A-).
Logs: `<S>/sweep/final-{light,root,heavy,tier0}/`.

## Left for the owners

- **emitter owner — `emit_c.kai dump_mono` / `dump_mono_out`** still call `infer_program(file, decls, tab,
  false)` (no views, no registry). `--dump-mono` / `--dump-mono-out` reject 116 corpus fixtures main accepts
  (`<S>/sweep/rc-mono-only.txt`), single files included: `examples/effects/eval_order_left_to_right.kai` ->
  "cannot find `length` in this scope" (core `list.length` / `string.length`). Change: add
  `views: FileViews, protos: ProtoView`, call `infer_report(file, decls, tab, views, protos, false)`, import
  `compiler.scope_file`, `compiler.proto_ids`, `compiler.infer_entry`; `driver.kai` MDumpMono / MDumpMonoOut pass
  `sym_views, pv_of(lowered.reg)`; then append `--dump-mono --dump-mono-out` to `REPORT_MODES` in
  `stage2/Makefile`. After that `infer.kai infer_program` and `infer_program_with_protos` have no caller.
- **typer owner — `infer.kai decl_shown`** (rows 3 and 5): a builtin's owner is the synthetic `@core` module,
  which fails both `sym_same(owner, v.home)` and `sv_is_core(v, owner)`, so the note reads `@core.print`.
  Repro: `fn main() : Int / Stdout = { print(42) 0 }` -> `in argument 1 of \`@core.print\``.
- typer owner — rows 4, 6, 7 (already known).

## Differential beyond the suites (exit codes, this tree vs pure main, every `.kai` under examples/ + demos/)

`<S>/sweep/rcdiff.txt` (before the report-mode fix: `--check`, `--dump-typed`, `--dump-mono`) and `rcdiff2.txt`
(after: `--dump-typed`, `--dump-last-use`, `--dump-rawsafe`).
- `--check`: 33 files differ; every accept -> reject is a negative fixture of this PR, a library file, or an
  accepted behaviour change (`examples/fmt/op_signature.*`: `HashMap` without its import; not compiled by a gate).
- fixed report modes, after the fix: 27 files main accepts and this tree rejects, all negative fixtures
  (`.err.expected` / `DIAG.expected`): the report modes now reject what the build rejects.

## Not run

- `rc-leak-gate` (C and native): run clean by another agent; `test-rc-budget` (C and native) passes here.
- `test-eval-order-gcc` (needs gcc x86_64), Linux-only behaviour (LeakSanitizer under ASAN, Linux TSAN
  interleavings), `rc-detector`, `kaic-boot-verify` (seed boot).
- The native sweep ran before the protocol registry was threaded into `1fc133f9`; that part and the new gate
  were verified on the C-only compiler only (front-end, report modes).
