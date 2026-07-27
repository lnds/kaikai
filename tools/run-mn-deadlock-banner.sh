#!/usr/bin/env bash
# M:N deadlock-banner determinism gate.
#
# A wedged scheduler is a WHOLE-machine state, so every idle worker observes
# it in the same instant. The report has to be claimed by exactly one of them
# or the banner count becomes a function of how many workers reach the print
# before the process tears down — which made the parity corpus diff a fixture
# against a non-deterministic oracle.
#
# The property gated here is invariance, not a count that happens to be 1
# today: repeat the same binary many times at several thread counts and
# require an identical observation every time. A single run proves nothing
# about a race, so the repeat count is the gate.
#
# The negative half matters as much: exit 1 plus one banner asserts the
# detection still FIRES. Suppressing the banner entirely would satisfy a
# count check that only looked for "not more than one".

set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAI="$ROOT/bin/kai"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

. "$ROOT/tools/lib/timeout.sh"
. "$ROOT/tools/lib/single-tu.sh"

RUN_TIMEOUT="${MN_RUN_TIMEOUT:-60}"
REPEATS="${MN_BANNER_REPEATS:-20}"

FIXTURE="examples/llvm/cancel_raise_in_fiber_under_mailbox.kai"

# Two banners report the same verdict from the two dispatch paths: N=1 drains
# its run queue with fibers parked, N>1 confirms global quiescence. Only the
# M:N one could ever repeat, but the gate matches either — what the parity
# corpus observes is "the program reported deadlock once", and pinning the
# wording per thread count would make the gate a spelling test.
BANNER="— deadlock"

# `kai` builds the scheduler as a separate -O0 owner object; single-tu-O2 is
# the single-TU path the stage2 Makefile recipes take. The window this gate
# watches opens differently under each, so both arms run.
ARMS=("kai" "single-tu-O2")

build_arm() {
  case "$1" in
    kai)          "$KAI" build "$2" -o "$3" ;;
    single-tu-O2) kai_build_single_tu "$2" "$3" ;;
    *)            echo "unknown arm $1" >&2; return 1 ;;
  esac
}

if [ "$KAI_TIMEOUT_KIND" = none ]; then
  echo "run-mn-deadlock-banner: WARNING — no timeout(1), gtimeout or perl on this host;"
  echo "  a wedged run will block instead of being counted as a hang."
fi

fail=0
for arm in "${ARMS[@]}"; do
  bin="$TMP/deadlock.$arm"
  build_arm "$arm" "$FIXTURE" "$bin" >/dev/null 2>"$TMP/b.log" \
    || { echo "BUILD FAILED [$arm]"; cat "$TMP/b.log"; exit 1; }

  for n in 1 2 4 8 default; do
    if [ "$n" = default ]; then run_env=(env -u KAI_THREADS); else run_env=(env KAI_THREADS="$n"); fi
    ok=0; badcount=0; badexit=0; hang=0; witness=""
    for _ in $(seq 1 "$REPEATS"); do
      ec=0
      kai_timeout "$RUN_TIMEOUT" "${run_env[@]}" "$bin" \
        >"$TMP/run.out" 2>"$TMP/run.err" || ec=$?
      banners="$(grep -cF -e "$BANNER" "$TMP/run.err" || true)"
      if [ "$ec" = 124 ] || [ "$ec" = 137 ]; then
        hang=$((hang+1))
      elif [ "$ec" != 1 ]; then
        badexit=$((badexit+1))
        [ -n "$witness" ] || witness="exit $ec"
      elif [ "$banners" != 1 ]; then
        badcount=$((badcount+1))
        [ -n "$witness" ] || witness="$banners banners"
      else
        ok=$((ok+1))
      fi
    done
    if [ "$ok" = "$REPEATS" ]; then
      echo "OK   deadlock-banner [$arm] N=$n ($ok/$REPEATS: exactly 1 banner, exit 1)"
    else
      echo "FAIL deadlock-banner [$arm] N=$n: ok=$ok badcount=$badcount badexit=$badexit hang=$hang of $REPEATS${witness:+ — $witness}"
      fail=1
    fi
  done
done

[ "$fail" = "0" ] && echo "run-mn-deadlock-banner: OK" || { echo "run-mn-deadlock-banner: FAIL"; exit 1; }
