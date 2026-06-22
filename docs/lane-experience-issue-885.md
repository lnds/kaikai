# Lane experience — harden the native-parity recheck (issue #885)

**Scope:** close the flaky-escape-hatch in `tools/test-backend-parity.sh`
so a DETERMINISTIC parity divergence can no longer be false-greened as
"flaky" by a single passing recheck. Shell-only: one file
(`tools/test-backend-parity.sh`), no compiler/runtime/stdlib/Makefile/YAML
touched. The harness is a runtime test driver, so the change takes effect
with no rebuild and is transparent to all three native CI shards.

## The bug

The ratchet re-verified each candidate "new gap" with `recheck_diverges`,
which re-ran the target up to 3 times against one oracle build and counted
a regression **only if it diverged ALL THREE times**. The first converging
run short-circuited to `return 1` ("flaky, not a regression") and the
fixture was dropped from the gate with a quiet log line:

> `ratchet: <fixture> diverged once but passed on recheck — flaky, not a regression`

The "intermittence" is not in the bug — it is in the parallel walk's
evaluation ORDER. A 100%-reproducible divergence can happen to converge on
a recheck depending on scheduling/RC/hash-order noise, and the old logic
took that single pass as proof of flakiness. `04_effect.kai` (#883) and
`match_shared_tag_subdiscrimination.kai` (#884) were proven 100%
reproducible at two different commits (`a26cf197`, `3bbcb2ca`) yet both
were labelled "flaky" in the GREEN tier1-native run on #881. The green was
FALSE: it masked two real bugs that only surfaced when #880 happened to
land on the bad side of the non-deterministic recheck.

## The fix — invert the short-circuit, make flaky the expensive verdict

`recheck_diverges` is rewritten so the cheap/early exit is **REAL gap**,
not flaky:

- The first recheck that diverges (or a target build that fails) returns
  immediately as a real gap (`return 0`). A divergence that reproduces
  even ONCE across the rechecks is a real failure — exactly as it would be
  with no recheck at all.
- A **flaky** verdict (`return 1`) is only reached after **N consecutive
  clean rechecks**, `PARITY_RECHECKS` (default **2**). With N=2 a
  divergence is downgraded only after THREE total confirmations the
  failure is gone: the original walk plus 2 rechecks.

### Why N = 2

2 is the sound default. A deterministic failure reproduces well within two
extra runs — the whole #883/#884 class diverges on essentially every run,
so it never survives even one recheck, let alone two. A genuine 1-in-many
timing flake is rare; the cost of mis-classifying one as a gap once is a
red CI that a re-run clears, which is strictly safer than the inverse
(false green hiding a real bug). N is overridable via `PARITY_RECHECKS` if
a future flake profile warrants more rechecks, but the floor is 2 (total
≥ 3 confirmations) — anything lower is the bug we just fixed.

### Loud flaky surfacing

When a verdict does land flaky, it is no longer swallowed into the green.
`flaky_warn` emits a GitHub `::warning::` annotation (guarded on
`$GITHUB_ACTIONS`, so it surfaces on the run summary, not buried in the
log) **and** an unmistakable `ratchet WARNING (flaky): …` log line. The
message states plainly that a divergence the harness deems flaky is still
a parity gap worth investigating.

### Third verdict — unjudgeable

The old code returned `1` (flaky) when the oracle itself failed to build
on recheck. That would now emit a misleading flaky warning ("converged on
N rechecks") for a fixture we never actually judged. Added a third verdict
code `2` = unjudgeable (oracle won't build here): no flaky warning, the
caller's pre-recheck observation stands, and the fixture is not
manufactured into a new gap. Both call-sites switch on the verdict via a
`case` instead of the old boolean `if`.

### `set -e` trap

The old call-sites used `if recheck_diverges …; then`, which is exempt
from `set -e`. Capturing a three-valued exit needs `verdict=0;
recheck_diverges "$g" || verdict=$?` — the `|| verdict=$?` keeps a
non-zero return from aborting the script under `set -eu` while preserving
the exact code. Verified in an isolated stub harness (below).

## Validation

- `bash -n tools/test-backend-parity.sh` — clean.
- Isolated unit test of the verdict logic (stub `recheck_diverges` driven
  by a per-attempt diverge/converge pattern), N=2 and N=3:
  - `DD`, `DC`, `CD`, `CCD` → REAL (any single divergence ⇒ gap). The
    `DC` case (diverge first, converge after) is exactly the #883/#884
    false-green, now correctly REAL.
  - `CC`, `CCC` → FLAKY (clean on all N rechecks) ⇒ loud warning.
  - oracle-fail ⇒ UNJUDGEABLE (no misleading warning).
  - `set -e` survives every non-zero return.
- Live native run on `examples/perceus` with a native-capable kaic2,
  `NATIVE_PARITY_RATCHET=1 BACKEND_PARITY_JOBS=1` — confirms a
  known-deterministic failure is reported FAIL, not flaky. (See PR for the
  captured output.)
- Native-unavailable SKIP path on a C-only checkout still exits 0
  cleanly — the change does not disturb C-only checkouts.

## Expected CI color change

This makes tier1-native HONEST. It will now go **RED** on the real
deterministic failures it was masking (#883/#884, plus the gap3/huffman
SIGSEGV from #882 if those lanes have not merged yet). **That red is the
harness telling the truth, not a regression this lane introduced.** If
#882/#883/#884 land first, the corpus is clean and tier1-native is green;
either way the harness is now honest. The integrator should read a new red
here as "a previously-masked deterministic bug is now visible", not as a
harness regression.

## Invariants preserved

- The empty baseline (`tools/native-parity-baseline.txt`) stays empty —
  the masked fixtures were NOT added to silence them (that is the opposite
  of the fix). Full-corpus parity remains the flip invariant.
- Existing flags / env contract untouched: `TARGET_BACKEND`,
  `ORACLE_BACKEND`, `BACKEND_PARITY_JOBS`, `BACKEND_PARITY_DIRS` (still
  shard-overridable per #881), `NATIVE_PARITY_RATCHET`. New optional knob:
  `PARITY_RECHECKS` (default 2).
- Pass/fail/skip accounting and the ratchet-vs-baseline logic are
  unchanged — only the flaky escape hatch is tightened.

## Follow-ups

None for this lane. The new tier1-native red (if any) is downstream work
for #882/#883/#884, not this harness change.
