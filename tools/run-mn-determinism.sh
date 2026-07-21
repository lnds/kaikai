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
#
# Two observation conditions are load-bearing, because this whole family of
# races is invisible without them:
#
#   * stdout goes to a FILE, never a pipe. Command substitution is a pipe,
#     and a pipe (like a tty) changes the buffering enough to close the
#     window: the same fixture that fails on a majority of runs redirected
#     to a file is clean on every run through a pipe.
#   * both build arms run. `bin/kai` compiles the scheduler into a separate
#     -O0 owner object, which suppresses the races outright; the single-TU
#     -O2 arm is the path the stage2 Makefile recipes take, and the one
#     where they reproduce.

set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAI="$ROOT/bin/kai"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

. "$ROOT/tools/lib/timeout.sh"
. "$ROOT/tools/lib/single-tu.sh"

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
  # Timeout-receive is the one fiber linked into two structures at once: the
  # mailbox recv-waiter chain and the reactor timer wheel. Every message here
  # beats its deadline, so every wake disarms the wheel from a scheduler
  # thread while the reactor owns it — unsynchronized, that strands a sleeper
  # (hang) or corrupts the parked count.
  "examples/effects/mn_recv_timeout_wheel.kai"
  # Fiber wrappers dropping to RC=0 on a thread other than the one each fiber
  # ran on: the free path must not release a stack that is still live.
  "examples/effects/mn_fiber_free_race.kai"
  # A bounded mailbox must mean the same thing on both send paths. Which one a
  # send takes depends only on where the work-stealer put the receiver, so a
  # policy applied on one and not the other makes delivery a function of
  # placement: under BlockSender the sender must park, never discard. A
  # discarded message strands the receiver on a drain that can no longer
  # complete, which is a hang, and the sum witness makes a partial delivery
  # visible even if it somehow completed.
  "examples/effects/mn_cross_thread_block_sender.kai"
)

# kai: the shipped build path (separate -O0 scheduler owner).
# single-tu-O2: emitted C as one TU, the stage2 Makefile's own path.
ARMS=("kai" "single-tu-O2")

if [ "$KAI_TIMEOUT_KIND" = none ]; then
  echo "run-mn-determinism: WARNING — no timeout(1), gtimeout or perl on this host;"
  echo "  a wedged run will block instead of being counted as a hang."
fi

build_arm() {
  case "$1" in
    kai)          "$KAI" build "$2" -o "$3" ;;
    single-tu-O2) kai_build_single_tu "$2" "$3" ;;
    *)            echo "unknown arm $1" >&2; return 1 ;;
  esac
}

# One line of witness, with newlines folded so a multi-line stdout stays
# greppable in the CI log.
witness_of() { head -c 200 "$1" | tr '\n' '|'; }

fail=0
for src in "${FIXTURES[@]}"; do
  name="$(basename "$(dirname "$src")")/$(basename "$src")"
  for arm in "${ARMS[@]}"; do
    tag="$name [$arm]"
    bin="$TMP/$(echo "$name" | tr '/' '_').$arm"
    build_arm "$arm" "$src" "$bin" >/dev/null 2>"$TMP/b.log" \
      || { echo "BUILD FAILED $tag"; cat "$TMP/b.log"; exit 1; }

    ref="$TMP/ref.out"
    kai_timeout "$RUN_TIMEOUT" env KAI_THREADS=1 "$bin" >"$ref" 2>/dev/null \
      || { echo "FAIL $tag: reference run at N=1 did not complete"; fail=1; continue; }

    for n in 4 8; do
      ok=0; diverge=0; empty=0; crash=0; hang=0; witness=""
      for _ in $(seq 1 "$REPEATS"); do
        ec=0
        kai_timeout "$RUN_TIMEOUT" env KAI_THREADS="$n" "$bin" \
          >"$TMP/run.out" 2>"$TMP/run.err" || ec=$?
        if [ "$ec" = 124 ] || [ "$ec" = 137 ]; then
          hang=$((hang+1))
        elif [ "$ec" != 0 ]; then
          crash=$((crash+1))
          [ -n "$witness" ] || witness="$(head -1 "$TMP/run.err")"
        elif [ ! -s "$TMP/run.out" ]; then
          empty=$((empty+1))
        elif ! cmp -s "$TMP/run.out" "$ref"; then
          diverge=$((diverge+1))
          [ -n "$witness" ] || witness="got '$(witness_of "$TMP/run.out")'"
        else
          ok=$((ok+1))
        fi
      done
      if [ "$ok" = "$REPEATS" ]; then
        echo "OK   $tag N=$n ($REPEATS/$REPEATS == N=1: $(witness_of "$ref"))"
      else
        echo "FAIL $tag N=$n: ok=$ok diverge=$diverge empty=$empty crash=$crash hang=$hang of $REPEATS${witness:+ — $witness}"
        fail=1
      fi
    done
  done
done

[ "$fail" = "0" ] && echo "run-mn-determinism: OK" || { echo "run-mn-determinism: FAIL"; exit 1; }
