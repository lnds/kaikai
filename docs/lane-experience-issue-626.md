# Lane experience — issue #626: discoverable markers for negative fixtures in positive dirs

## Scope as planned vs as shipped

**Planned (issue #626):** give the ~40 fixtures listed in `tools/backend-parity-skips.txt`
with marker `:626:` a discoverable marker, and retire the hand-rolled `stage2/Makefile`
loops that gate the negative ones. The issue named two cures (move to `examples/negative/`
+ golden, or in-place `.err.expected`) and ~7 stdlib fixture-rot cases to diagnose.

**Shipped:** the same outcome, but the real shape of the work was different from the issue's
framing — most of the "~40" were not what their skip rationale claimed:

- **16 genuine compile-reject negatives** (all in `examples/effects/`) moved to
  `examples/negative/{parser_syntax,effects_phase2,subsumption,diagnostic_quality}/` with
  their goldens; their 16 hand-rolled Makefile loops deleted from the `test-effects` target.
- **~20 fixtures were mislabeled** as negative / aspirational / rot. Verified empirically
  (compiled + ran + checked C↔LLVM parity with `bin/kai`) that they are **live positives**;
  de-listed from the skip file so the parity harness runs them as ordinary positives.
- **8 library / harness-mode** fixtures (no `main`) + 3 package-mode: skip is legitimate;
  relabeled off `:626:` to `:357:` (suite-membership, not parity bug).
- The `:626:` marker is now **fully gone** from `tools/backend-parity-skips.txt`.

## The structural surprise the brief did not anticipate

The issue described the negatives as "gated by hand-rolled loops … **no `.err.expected`**".
That was false: **all 16 already carried `.err.expected` siblings.** The golden was
*decorative* — the real gate was a `grep -q "<substring>"` hardcoded in the Makefile loop,
and nobody read the golden. Worse, `test-backend-parity.sh:is_skipped` auto-skips any fixture
with a sibling golden and its comment claims "exercised by tools/test-negative.sh" — but
`test-negative.sh` only walks `examples/negative/`, so the in-place goldens were exercised by
*nobody* except the Makefile loop. The migration was half-done and silently inconsistent.

This reframed the coverage-preservation check: before deleting each loop, confirm the golden
contained the loop's hardcoded substring(s). It did in every case (verified line by line). For
the 3 multi-`grep` loops (`m7a_6f_resume_arg_type` ×3, `m7a_8_diag_effect_not_handled` ×4,
`m8_spawn_row_mismatch` whose substring `() -> ` was a *note* line, not line 1) the
single-line `.err.expected` contract would have dropped coverage — those were promoted to
`.diag.expected`, where the harness asserts every non-comment line as a substring of stderr.

## Three "fixture-rot" cases that were not rot

The issue flagged `os_basic`, `os_process_basic`, `time_clock_default` as rot ("env→Env
rename, missing `duration_lt`"). Diagnosed case by case: **every "renamed/missing" symbol
exists in stdlib today** (`effect Env`, `env.get`, `args.argv`, `process.start/wait`, the
`Exited/Signaled` builtins, `time.duration_lt`). All three compile, run, and pass C↔LLVM
parity. The skip rationale was stale, not the fixtures. They were de-listed; **nothing was
deleted** (the brief's rule: fixing a broken reference ≠ converting to negative; delete only
with written justification — and there was no rot to delete).

The 11 `demos/9d9l/*` + `demos/vs/*` programs were likewise skip-listed "aspirational; FAIL
until <feature> lands". Verified: every one builds and passes parity today (the features
shipped). De-listed. `demos/9d9l/vectores` panics `file.read: cannot open file` with no input
present — but exit 1 is identical on both backends, so it still passes the parity check (which
compares the two backends to each other, not to a golden).

## Design decisions & alternatives considered

**A (move) vs B (in-place + extend `test-negative.sh`'s `find`).** Chose A, validated with the
architect. B would have given one golden two owners with two matching semantics (substring vs
the byte-exact `diff` that `test-violations` uses on `.run.err.expected`), reverting the #511
separation where `test-negative.sh` walks exactly one tree. A is the convention #626 cites;
`examples/negative` is already excluded from the parity walk by directory, so the moved
negatives leave the parity skip list for the *right* reason (location), not a per-fixture line.
No moved fixture was multi-file or imported by a positive, and none appears in
`tools/native-parity-baseline.txt`, so the path churn was contained and conflict-free with the
parallel native-parity-burndown-4 lane.

**Deviation from the approved plan: no orphan `.out.expected` goldens added.** The plan said to
add `.out.expected` for `m8x_7`, `02_fizzbuzz`, `05_concurrent`. While implementing, confirmed
that **no harness consumes `.out.expected` under `examples/effects` or `examples/quickstart`**:
the parity harness compares the C and LLVM backends *to each other*, not to a golden, and
`test-llvm-coverage` only walks `sugars/units/minimal/llvm`. Adding those goldens would have
created orphans no harness reads. De-listing alone (so the parity harness runs them C↔LLVM) is
the coverage that matters, and all three pass. Recorded here because it diverges from the plan.

## Fixtures added / coverage ledger (before → after)

| Suite | Before | After | Δ |
|---|---|---|---|
| `examples/negative/**` (`.kai`) | 126 | 142 | +16 (the moved negatives) |
| `test-negative.sh` summary | 110 PASS / 0 FAIL / 0 MISS | 126 PASS / 0 FAIL / 0 MISS | +16 PASS |
| `stage2/Makefile` hand-rolled negative loops (#626) | 16 | 0 | −16 |
| `:626:` skip lines | 53 | 0 | −53 |
| Active parity skip lines (total) | 59 | 17 | −42 (de-listed live positives + relabels) |

No coverage dropped: the 16 negatives are still gated (now by `test-negative.sh` against their
goldens, with the multi-substring ones strengthened to `.diag.expected`); the ~20 de-listed
positives gained real C↔LLVM coverage they did not have while skip-listed.

## Verification

- `cd stage2 && make test-effects` → green (the 16 loops gone, positive/C-shape/runtime-abort
  loops untouched).
- `./tools/test-negative.sh` → 126 PASS / 0 FAIL / 0 MISS.
- `KAI=./bin/kai ./tools/test-backend-parity.sh` → skip count drops by the de-listed lines, no
  new FAIL (the de-listed positives pass C↔LLVM).
- `make tier0 && make tier1` → green (tier1-backend-parity is the #626 gate).

## Follow-ups left for next lanes

- `examples/effects/row_pure_to_effectful.out.expected` contains `42`, but `main` *returns* 42
  (it does not print) — that golden is an exit-code golden no file-mode harness diffs. Out of
  #626 scope; left as a separate cleanup if a harness ever starts diffing it.
- The remaining `:357:` suite-membership skips (library/harness-mode files with no `main`) are
  legitimate but could be excluded by a dedicated entry-point detector rather than a skip line,
  if a future lane wants `backend-parity-skips.txt` to hold only true parity exemptions.
