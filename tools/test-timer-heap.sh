#!/bin/sh
# Parking a fiber on a timer costs O(log n) in the sleepers already parked:
# 40K concurrent sleepers take a few times what 10K do, not sixteen times,
# at one thread and at the default. Usage: test-timer-heap.sh <c|native>

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
backend="${1:?usage: test-timer-heap.sh <c|native>}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM
fx="$ROOT/examples/runtime/timer_sleepers.kai"

for n in 10000 40000; do
  sed "s/\[1\.\.10000\]/[1..$n]/" "$fx" > "$work/s$n.kai"
  "$ROOT/bin/kai" build --backend="$backend" "$work/s$n.kai" -o "$work/s$n" 2> "$work/build.err" \
    || { echo "timer-heap FAIL ($backend: build of $n sleepers failed)"; cat "$work/build.err"; exit 1; }
done

# Milliseconds of the best of three runs.
best_ms() {
  best=""
  for _ in 1 2 3; do
    t0=$(date +%s%N 2> /dev/null || echo 0)
    "$@" > /dev/null
    t1=$(date +%s%N 2> /dev/null || echo 0)
    ms=$(( (t1 - t0) / 1000000 ))
    [ -z "$best" ] || [ "$ms" -lt "$best" ] && best=$ms
  done
  echo "$best"
}

for t in 1 ""; do
  export KAI_THREADS="$t"
  for n in 10000 40000; do
    out="$("$work/s$n")"
    [ "$out" = "done tasks=$n" ] \
      || { echo "timer-heap DIFF ($backend, KAI_THREADS=$t, $n sleepers)"; echo "$out"; exit 1; }
  done
  small=$(best_ms "$work/s10000")
  large=$(best_ms "$work/s40000")
  [ "$large" -lt $(( small * 10 )) ] \
    || { echo "timer-heap FAIL ($backend, KAI_THREADS=$t: 40K sleepers ${large} ms vs 10K ${small} ms, over 10x)"; exit 1; }
  echo "timer-heap info ($backend, KAI_THREADS=$t: 10K ${small} ms, 40K ${large} ms)"
done
echo "timer-heap OK ($backend: 40K concurrent sleepers within 10x of 10K, at 1 thread and the default)"
