#!/usr/bin/env bash
# M:N determinism gate (no TSAN, fast — rides tier1). A deterministic
# concurrent program must print the SAME output at KAI_THREADS=1 and
# KAI_THREADS=4: a divergence is either a real missing serialization point
# or a latent scheduling-order dependence, and this gate surfaces it. Also
# asserts the cross-thread run terminates (no shutdown hang) and is stable
# across repeats.
#
# Every run is classified into exactly one bucket — ok / diverge / empty /
# crash / hang. Collapsing them loses the distinction between a wedged
# scheduler and a fiber that dropped its output, which is the difference
# between two unrelated bugs.

set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAI="$ROOT/bin/kai"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

. "$ROOT/tools/lib/timeout.sh"

RUN_TIMEOUT="${MN_RUN_TIMEOUT:-60}"
REPEATS="${MN_REPEATS:-3}"

FIXTURES=(
  "demos/parallel_actors/main.kai"
  "examples/effects/mn_cross_thread_copy_stress.kai"
  # F2 reactor: I/O (reactor timer wheel) and CPU fibers make progress
  # concurrently, and the summed total is identical at N=1/N=4/N=8. Guards
  # the dedicated-reactor handback against a scheduling-order divergence or
  # a lost-wakeup that would strand a sleeper (the shutdown-hang check below
  # catches the deadlock this lane closed).
  "examples/effects/mn_reactor_io_cpu_mix.kai"
  # Work-stealing must not corrupt a parked fiber's identity: the resume
  # store of kai_active_fiber has to re-resolve its thread-local slot on the
  # thread that now runs, not the one that parked. Many short sleepers +
  # CPU contention maximise park -> steal -> resume hops; a rotated identity
  # truncates the timer wheel (hang) or reports Actor unhandled.
  "examples/effects/mn_park_resume_steal.kai"
)

if [ "$KAI_TIMEOUT_KIND" = none ]; then
  echo "run-mn-determinism: WARNING — no timeout(1), gtimeout or perl on this host;"
  echo "  a wedged run will block instead of being counted as a hang."
fi

fail=0
for src in "${FIXTURES[@]}"; do
  name="$(basename "$(dirname "$src")")/$(basename "$src")"
  bin="$TMP/$(echo "$name" | tr '/' '_')"
  "$KAI" build "$src" -o "$bin" >/dev/null 2>"$TMP/b.log" \
    || { echo "BUILD FAILED $name"; cat "$TMP/b.log"; exit 1; }

  ref="$(kai_timeout "$RUN_TIMEOUT" env KAI_THREADS=1 "$bin")" \
    || { echo "FAIL $name: reference run at N=1 did not complete"; fail=1; continue; }

  for n in 4 8; do
    ok=0; diverge=0; empty=0; crash=0; hang=0; witness=""
    for _ in $(seq 1 "$REPEATS"); do
      ec=0
      got="$(kai_timeout "$RUN_TIMEOUT" env KAI_THREADS="$n" "$bin" 2>"$TMP/run.err")" || ec=$?
      if [ "$ec" = 124 ]; then
        hang=$((hang+1))
      elif [ "$ec" != 0 ]; then
        crash=$((crash+1))
        [ -n "$witness" ] || witness="$(head -1 "$TMP/run.err")"
      elif [ -z "$got" ]; then
        empty=$((empty+1))
      elif [ "$got" != "$ref" ]; then
        diverge=$((diverge+1))
        [ -n "$witness" ] || witness="got '$got'"
      else
        ok=$((ok+1))
      fi
    done
    if [ "$ok" = "$REPEATS" ]; then
      echo "OK   $name N=$n ($REPEATS/$REPEATS == N=1: $ref)"
    else
      echo "FAIL $name N=$n: ok=$ok diverge=$diverge empty=$empty crash=$crash hang=$hang of $REPEATS${witness:+ — $witness}"
      fail=1
    fi
  done
done

[ "$fail" = "0" ] && echo "run-mn-determinism: OK" || { echo "run-mn-determinism: FAIL"; exit 1; }
