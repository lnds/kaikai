#!/usr/bin/env bash
# F2 no-starvation gate: a fiber sleeping on the reactor must wake on time
# while CPU-bound fibers keep the scheduler threads busy.
#
# Under F1 the reactor ran inline on the owner scheduler thread (thread 0),
# draining the timer wheel only when THAT thread returned to its loop
# between fibers. A CPU hog pinning thread 0 therefore stalls every
# reactor waiter until the hog finishes, no matter how many other threads
# are idle — the sleeper's measured elapsed balloons to a full CPU burst.
# Under F2 the reactor drains the wheel on its own thread, independent of
# which scheduler threads are busy, so the sleeper wakes on its deadline.
#
# Calibration — why (nthreads - 1) hogs, not a wall of them: kaikai's
# scheduler is COOPERATIVE and non-preemptive. Once the reactor readies
# the sleeper it still needs a scheduler thread to run it, and a
# non-yielding CPU fiber never relinquishes its thread mid-burst. So if
# every scheduler thread is pinned (hogs >= nthreads), NO runtime — F1 or
# F2 — can dispatch the woken sleeper before a hog finishes; both measure a
# full burst and the metric cannot tell them apart. Leaving exactly one
# free thread isolates the property F2 actually fixes: the reactor draining
# regardless of thread 0's load. With (nthreads - 1) hogs, F1 still starves
# whenever thread 0 draws a hog (the common case — main dispatches one
# after it parks), while F2 wakes on time every run.
#
# The gate is the sleeper's measured elapsed: within a small multiple of
# the requested sleep. Timing is machine-dependent; the ceiling is a
# generous 4x so a slow box still separates ~20ms (F2) from a full CPU
# burst (F1, hundreds of ms).

set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAI="$ROOT/bin/kai"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Default to min(4, nproc): one hog per scheduler thread except one, plus
# the reactor thread, must fit without oversubscribing the box — otherwise
# the "free" scheduler thread that dispatches the woken sleeper competes for
# a core with the hogs and the sleeper's wake latency inflates under pure
# CPU contention (a scheduling artifact, not reactor starvation). An
# explicit KAI_THREADS still wins for local experiments.
NCPU="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
DEF_NTH=4; [ "$NCPU" -lt 4 ] && DEF_NTH="$NCPU"
NTH="${KAI_THREADS:-$DEF_NTH}"
SLEEP_MS=20
if [ "$NTH" -lt 2 ]; then
  echo "run-mn-reactor-bench: SKIP (needs >=2 threads; box has $NCPU cpu)"
  exit 0
fi

# One CPU hog per scheduler thread except one, so a free thread is always
# available to dispatch the reactor-woken sleeper (see calibration above).
HOGS=$((NTH - 1))
hoglist="$(seq -s, 1 "$HOGS")"

cat > "$TMP/starve.kai" <<EOF
import spawn
import time
fn burn(n: Int, acc: Int) : Int = if n <= 0 { acc } else { burn(n - 1, acc + (n % 7)) }
fn io() : Int / Spawn + Clock = {
  let s = monotonic()
  sleep(millis(${SLEEP_MS}))
  let e = monotonic()
  (instant_to_nanos(e) - instant_to_nanos(s)) / 1000000
}
fn suml(xs: [Int], a: Int) : Int = match xs {
  []        -> a
  [x, ...r] -> suml(r, a + x)
}
fn main() : Int / Spawn + Clock + Stdout = nursery { nur ->
  let io_f = nur.spawn(() => io())
  let fs = [${hoglist}] | (i => nur.spawn(() => burn(400000000, i)))
  let ms = nur.await(io_f)
  let _  = suml(fs | (f => nur.await(f)), 0)
  Stdout.print("#{int_to_string(ms)}")
  0
}
EOF

"$KAI" build "$TMP/starve.kai" -o "$TMP/starve" >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 1; }

# median sleeper-elapsed over 5 runs (odd count → unambiguous median; more
# samples than the old 3 so a startup-transient outlier cannot swing it)
samples=()
for i in 1 2 3 4 5; do
  ms="$(KAI_THREADS=$NTH "$TMP/starve" 2>/dev/null | tail -1)"
  samples+=("$ms")
done
med=$(printf '%s\n' "${samples[@]}" | sort -n | awk 'NR==3')

echo "reactor bench (KAI_THREADS=$NTH, ${HOGS} hogs): sleeper asked ${SLEEP_MS}ms, measured ${med}ms under CPU load"

# Gate: the sleeper must wake within a small multiple of its request. 4x
# the sleep (80ms for a 20ms sleep) is well above the on-time case (~22ms)
# and far below the F1 starvation (a full CPU burst, hundreds of ms).
ceiling=$((SLEEP_MS * 4))
if [ "$med" -le "$ceiling" ]; then
  echo "run-mn-reactor-bench: OK (${med}ms <= ${ceiling}ms — reactor drains under CPU load)"
else
  echo "run-mn-reactor-bench: FAIL (${med}ms > ${ceiling}ms — reactor starves behind CPU work)"
  exit 1
fi
