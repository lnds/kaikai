#!/usr/bin/env bash
# M:N idle-CPU gate: a program that only sleeps must not burn CPU.
#
# Every program enters the M:N scheduler with N workers. While the only
# fibers are parked on reactor timers, every worker is idle; an idle worker
# must block until a producer wakes it. A worker that polls instead (a short
# nanosleep retry loop) burns a measurable slice of a core per idle worker
# for the whole sleep, and the run still passes an output-only check — so
# the gate measures the CPU the process consumed (user + sys, all threads).
#
# N is pinned (not the host default) so the idle-worker count, and with it
# the cost a polling regression shows, is the same on every host. The
# budget is a fraction of the run's wall-clock: blocked workers cost only
# startup, while a 200µs poll at N=8 measured ~19% of a core.

set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAI="$ROOT/bin/kai"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

. "$ROOT/tools/lib/timeout.sh"

FIXTURE="examples/effects/mn_idle_sleep_cpu.kai"
EXPECTED="${FIXTURE%.kai}.out.expected"
NTH="${MN_IDLE_THREADS:-8}"
RUN_TIMEOUT="${MN_IDLE_RUN_TIMEOUT:-60}"
# Percent of wall-clock the whole process may spend on CPU.
BUDGET_PCT="${MN_IDLE_BUDGET_PCT:-5}"
SAMPLES=3

"$KAI" build "$FIXTURE" -o "$TMP/idle" >/dev/null 2>"$TMP/build.log" \
  || { echo "run-mn-idle-cpu: BUILD FAILED"; cat "$TMP/build.log"; exit 1; }

# Best of SAMPLES: host noise only ever adds CPU, so the minimum is the
# run's own cost.
best_cpu=""; best_real=""
for i in $(seq 1 "$SAMPLES"); do
  ec=0
  TIMEFORMAT="%R %U %S"
  { time kai_timeout "$RUN_TIMEOUT" env KAI_THREADS="$NTH" "$TMP/idle" \
      >"$TMP/run.out" 2>"$TMP/run.err"; } 2>"$TMP/time.txt" || ec=$?
  if [ "$ec" = 124 ] || [ "$ec" = 137 ]; then
    echo "run-mn-idle-cpu: FAIL (run $i wedged — no exit within ${RUN_TIMEOUT}s)"
    exit 1
  fi
  if [ "$ec" != 0 ]; then
    echo "run-mn-idle-cpu: FAIL (run $i exited $ec)"; cat "$TMP/run.err"
    exit 1
  fi
  diff -q "$EXPECTED" "$TMP/run.out" >/dev/null \
    || { echo "run-mn-idle-cpu: DIFF (run $i)"; diff "$EXPECTED" "$TMP/run.out"; exit 1; }
  read -r real user sys < <(tail -1 "$TMP/time.txt")
  cpu_ms=$(awk -v u="$user" -v s="$sys" 'BEGIN { printf "%d", (u + s) * 1000 }')
  real_ms=$(awk -v r="$real" 'BEGIN { printf "%d", r * 1000 }')
  if [ -z "$best_cpu" ] || [ "$cpu_ms" -lt "$best_cpu" ]; then
    best_cpu=$cpu_ms; best_real=$real_ms
  fi
done

ceiling=$((best_real * BUDGET_PCT / 100))
echo "idle cpu (KAI_THREADS=$NTH): ${best_cpu}ms CPU over ${best_real}ms wall (best of $SAMPLES)"
if [ "$best_cpu" -le "$ceiling" ]; then
  echo "run-mn-idle-cpu: OK (${best_cpu}ms <= ${ceiling}ms — idle workers block)"
else
  echo "run-mn-idle-cpu: FAIL (${best_cpu}ms > ${ceiling}ms — idle workers burn CPU while every fiber sleeps)"
  exit 1
fi
