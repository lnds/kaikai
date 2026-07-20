#!/usr/bin/env bash
# High-repeat M:N soundness stress for one fixture. Maintainer tool, not a
# gate: the run counts here are the ones an acceptance claim needs (hundreds
# per thread count), which is minutes of wall clock, so it never rides tier1.
#
# Every run lands in exactly one bucket — ok / bad / crash / hang. Collapsing
# them loses the difference between a wedged scheduler and a fiber that
# dropped its output, which is usually the difference between two unrelated
# bugs. The classification is the point of this script; a bare loop counting
# "output != expected" produced the untrustworthy numbers documented in
# docs/lane-experience-issue-1258.md.
#
# Run ONE arm at a time. Two concurrent stress loops change the contention
# they are measuring, so their numbers are not comparable to serial ones.
#
#   tools/run-mn-stress.sh <fixture.kai> [runs] [threads...]
#
# `bin/kai` honours CFLAGS, so a sanitizer arm is the same command with
# CFLAGS="-std=c11 -g -O1 -fsanitize=address" in front.
#
# The expected output is the fixture's .out.expected when present, otherwise
# the fixture's own KAI_THREADS=1 run.

set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

SRC="${1:?usage: run-mn-stress.sh <fixture.kai> [runs] [threads...]}"
RUNS="${2:-500}"
shift $(( $# > 2 ? 2 : $# ))
THREADS=("$@")
[ "${#THREADS[@]}" -gt 0 ] || THREADS=(8 16)

. "$ROOT/tools/lib/timeout.sh"
RUN_TIMEOUT="${MN_RUN_TIMEOUT:-60}"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
BIN="$TMP/fixture"

"$ROOT/bin/kai" build "$SRC" -o "$BIN" >/dev/null 2>"$TMP/build.log" \
  || { echo "BUILD FAILED $SRC"; cat "$TMP/build.log"; exit 1; }

EXPECTED_FILE="${SRC%.kai}.out.expected"
if [ -f "$EXPECTED_FILE" ]; then
  expected="$(cat "$EXPECTED_FILE")"
else
  expected="$(kai_timeout "$RUN_TIMEOUT" env KAI_THREADS=1 "$BIN")" \
    || { echo "FAIL: reference run at N=1 did not complete"; exit 1; }
fi

if [ "$KAI_TIMEOUT_KIND" = none ]; then
  echo "run-mn-stress: WARNING — no timeout(1), gtimeout or perl on this host;"
  echo "  a wedged run will block instead of being counted as a hang."
fi

# One run -> $BUCKET, plus the first non-ok run's evidence in $WITNESS. Sets
# globals rather than echoing: a command substitution would run this in a
# subshell and lose the witness.
classify_run() {
  local ec=0 got
  got="$(kai_timeout "$RUN_TIMEOUT" env KAI_THREADS="$1" "$BIN" 2>"$TMP/run.err")" || ec=$?
  if   [ "$ec" = 124 ] || [ "$ec" = 137 ]; then BUCKET=hang
  elif [ "$ec" != 0 ];            then BUCKET=crash; : "${WITNESS:=$(head -1 "$TMP/run.err")}"
  elif [ "$got" != "$expected" ]; then BUCKET=bad;   : "${WITNESS:=got '$got'}"
  else                                 BUCKET=ok
  fi
}

fail=0
for n in "${THREADS[@]}"; do
  declare -A count=([ok]=0 [bad]=0 [crash]=0 [hang]=0)
  WITNESS=""
  for _ in $(seq 1 "$RUNS"); do
    classify_run "$n"
    count[$BUCKET]=$(( count[$BUCKET] + 1 ))
  done
  echo "N=$n runs=$RUNS ok=${count[ok]} bad=${count[bad]} crash=${count[crash]}" \
       "hang=${count[hang]}${WITNESS:+ — $WITNESS}"
  [ "${count[ok]}" = "$RUNS" ] || fail=1
done

[ "$fail" = 0 ] && echo "run-mn-stress: OK $(basename "$SRC")" \
                || { echo "run-mn-stress: FAIL $(basename "$SRC")"; exit 1; }
