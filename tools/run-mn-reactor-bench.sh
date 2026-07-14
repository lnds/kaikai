#!/usr/bin/env bash
# F2 no-starvation gate: a fiber sleeping on the reactor must wake on
# time even while long CPU-bound fibers pin every scheduler thread.
#
# Under F1 the reactor ran inline on the owner scheduler thread (thread
# 0), draining the timer wheel only when that thread returned to its
# loop between fibers. A wall of long CPU hogs (>> nthreads) keeps
# thread 0 pinned, so a concurrent short sleeper starves — its measured
# elapsed balloons far past the sleep it asked for (~18x in practice).
# Under F2 the reactor is on its own thread and drains the timer wheel
# independently, so the sleeper wakes on time regardless of CPU load.
#
# The gate is the sleeper's measured elapsed: it must stay within a
# small multiple of the requested sleep. Not wired into tier1 (timing is
# machine-dependent); run locally to validate the F2 reactor extraction.

set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAI="$ROOT/bin/kai"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

NTH="${KAI_THREADS:-4}"
SLEEP_MS=20

cat > "$TMP/starve.kai" <<'EOF'
import spawn
import time
fn burn(n: Int, acc: Int) : Int = if n <= 0 { acc } else { burn(n - 1, acc + (n % 7)) }
fn io() : Int / Spawn + Clock = {
  let s = monotonic()
  sleep(millis(20))
  let e = monotonic()
  (instant_to_nanos(e) - instant_to_nanos(s)) / 1000000
}
fn suml(xs: [Int], a: Int) : Int = match xs {
  []        -> a
  [x, ...r] -> suml(r, a + x)
}
fn main() : Int / Spawn + Clock + Stdout = nursery { nur ->
  let io_f = nur.spawn(() => io())
  let fs = [1,2,3,4,5,6,7,8] | (i => nur.spawn(() => burn(400000000, i)))
  let ms = nur.await(io_f)
  let _  = suml(fs | (f => nur.await(f)), 0)
  Stdout.print("#{int_to_string(ms)}")
  0
}
EOF

"$KAI" build "$TMP/starve.kai" -o "$TMP/starve" >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 1; }

# median sleeper-elapsed over 3 runs
samples=()
for i in 1 2 3; do
  ms="$(KAI_THREADS=$NTH "$TMP/starve" 2>/dev/null | tail -1)"
  samples+=("$ms")
done
med=$(printf '%s\n' "${samples[@]}" | sort -n | awk 'NR==2')

echo "reactor bench (KAI_THREADS=$NTH): sleeper asked ${SLEEP_MS}ms, measured ${med}ms under CPU load"

# Gate: the sleeper must wake within a small multiple of its request.
# A ceiling of 4x the sleep (80ms for a 20ms sleep) is comfortably above
# the on-time case (~22ms) and far below the F1 starvation (~363ms).
ceiling=$((SLEEP_MS * 4))
if [ "$med" -le "$ceiling" ]; then
  echo "run-mn-reactor-bench: OK (${med}ms <= ${ceiling}ms — reactor drains under CPU load)"
else
  echo "run-mn-reactor-bench: FAIL (${med}ms > ${ceiling}ms — reactor starves behind CPU work)"
  exit 1
fi
