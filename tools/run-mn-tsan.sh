#!/usr/bin/env bash
# M:N ThreadSanitizer gate. Builds the multi-actor concurrency fixtures
# with -fsanitize=thread (C backend) and runs them at KAI_THREADS=4. A
# data race on a mis-partitioned runtime global surfaces here even when
# stdout-diff is blind to it (the corruption is nondeterministic under
# load). Zero races is the merge gate for the M:N scheduler.
#
# Also asserts determinism: the deterministic fixtures print the same
# output at N=1 and N=4, so a scheduling-order-dependent divergence is
# caught, not silently accepted.
#
# Runs off the main tier1 light path (TSAN is slow); wired into a
# dedicated CI tier, never TEST_LIGHT.

set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

KAI="$ROOT/bin/kai"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

. "$ROOT/tools/lib/timeout.sh"

# A scheduler deadlock wedges the fixture instead of failing it, and an
# unbounded run then burns the whole CI job ceiling and reports `cancelled`
# — indistinguishable from a routine supersede. Bound each run so a wedge
# fails here, naming the fixture. TSAN slows execution ~30-50x and the
# fixture still finishes in seconds, so the default is ~10x headroom.
RUN_TIMEOUT="${MN_TSAN_RUN_TIMEOUT:-120}"

# The failures this gate catches are intermittent at the percent level, and a
# single run per job lets one ride for dozens of merges before anyone looks.
# The fixtures cost seconds each, so repeating them is the cheapest detection
# probability available.
REPS="${MN_TSAN_REPS:-10}"

if [ "$KAI_TIMEOUT_KIND" = none ]; then
  echo "run-mn-tsan: WARNING — no timeout(1), gtimeout or perl on this host;"
  echo "  a wedged run will block instead of being reported as a hang."
fi

# TSAN-instrumented, C backend (the portable oracle runtime).
TSAN_CFLAGS="-std=c11 -Wno-unused-function -Wno-unused-variable -g -O1 -fsanitize=thread"

# The cross-thread copy stress fixture exercises every shared-state path
# — work-stealing deques, the mailbox lock, deep-copy-on-send, the
# idle/wake protocol, and shutdown — with light per-message work, so TSAN
# (which slows execution ~30-50x) finishes fast. The heavy speedup bench
# (parallel_actors, 300M/worker) is for wall-clock, not race detection;
# running it under TSAN would time out with no extra coverage.
#
# The deep-stack migration fixture covers the other half: fibers that yield
# deep inside a non-tail call chain, so a steal moves a tall stack of live
# frames between OS threads. That is the shape any per-OS-thread bookkeeping
# gets wrong when it cannot see a ucontext switch — TSAN's included, which is
# why the runtime annotates every fiber switch for it.
FIXTURES=(
  "examples/effects/mn_cross_thread_copy_stress.kai"
  "examples/effects/mn_deep_stack_migration.kai"
)

fail=0
for src in "${FIXTURES[@]}"; do
  name="$(basename "$(dirname "$src")")/$(basename "$src")"
  bin="$TMP/$(echo "$name" | tr '/' '_').tsan"
  echo "== building $name with TSAN (C backend) =="
  CFLAGS="$TSAN_CFLAGS" KAI_BACKEND=c "$KAI" build "$src" -o "$bin" --backend=c \
    >/dev/null 2>"$TMP/build.log" || { echo "BUILD FAILED"; cat "$TMP/build.log"; exit 1; }

  # N=1 reference output (no threads, no TSAN cross-thread reports).
  ec=0
  out1="$(kai_timeout "$RUN_TIMEOUT" env KAI_THREADS=1 "$bin")" || ec=$?
  if [ "$ec" = 124 ] || [ "$ec" = 137 ]; then
    echo "HANG: $name wedged at KAI_THREADS=1 (no exit within ${RUN_TIMEOUT}s)"; fail=1; continue
  elif [ "$ec" != 0 ]; then
    echo "FAIL: $name exited $ec at KAI_THREADS=1"; fail=1; continue
  fi

  # N=4 under TSAN: exitcode=99 makes a data race fail the process.
  echo "== running $name at KAI_THREADS=4 under TSAN, $REPS reps =="
  bad=0
  for rep in $(seq 1 "$REPS"); do
    ec=0
    out4="$(kai_timeout "$RUN_TIMEOUT" \
        env TSAN_OPTIONS='halt_on_error=0 exitcode=99' KAI_THREADS=4 "$bin" \
        2>"$TMP/tsan.log")" || ec=$?
    races="$(grep -c 'WARNING: ThreadSanitizer' "$TMP/tsan.log" || true)"

    if [ "$ec" = 124 ] || [ "$ec" = 137 ]; then
      echo "HANG: $name wedged at KAI_THREADS=4 under TSAN on rep $rep (no exit within ${RUN_TIMEOUT}s)"
      sed -n '1,40p' "$TMP/tsan.log"; bad=1; break
    fi
    if [ "$races" != "0" ] || [ "$ec" = "99" ]; then
      echo "TSAN: $races race(s) in $name (rep $rep)"; sed -n '1,80p' "$TMP/tsan.log"; bad=1; break
    fi
    # A run that printed the right answer and then died on a signal is a
    # failure the race and determinism checks both miss: out4 is already
    # captured and the log is empty. Only the exit status shows it.
    if [ "$ec" != 0 ]; then
      echo "FAIL: $name exited $ec at KAI_THREADS=4 with no TSAN report (rep $rep)"
      sed -n '1,40p' "$TMP/tsan.log"; bad=1; break
    fi
    if [ "$out1" != "$out4" ]; then
      echo "DETERMINISM: $name diverged N=1 ('$out1') vs N=4 ('$out4') on rep $rep"; bad=1; break
    fi
  done
  if [ "$bad" != 0 ]; then fail=1; continue; fi
  echo "OK $name — TSAN clean, deterministic (N=1==N=4: $out4)"
done

[ "$fail" = "0" ] && echo "run-mn-tsan: OK (M:N TSAN clean + deterministic)" || { echo "run-mn-tsan: FAIL"; exit 1; }
