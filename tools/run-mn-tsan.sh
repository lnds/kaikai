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

# TSAN-instrumented, C backend (the portable oracle runtime).
TSAN_CFLAGS="-std=c11 -Wno-unused-function -Wno-unused-variable -g -O1 -fsanitize=thread"

# The cross-thread copy stress fixture exercises every shared-state path
# — work-stealing deques, the mailbox lock, deep-copy-on-send, the
# idle/wake protocol, and shutdown — with light per-message work, so TSAN
# (which slows execution ~30-50x) finishes fast. The heavy speedup bench
# (parallel_actors, 300M/worker) is for wall-clock, not race detection;
# running it under TSAN would time out with no extra coverage.
FIXTURES=(
  "examples/effects/mn_cross_thread_copy_stress.kai"
)

fail=0
for src in "${FIXTURES[@]}"; do
  name="$(basename "$(dirname "$src")")/$(basename "$src")"
  bin="$TMP/$(echo "$name" | tr '/' '_').tsan"
  echo "== building $name with TSAN (C backend) =="
  CFLAGS="$TSAN_CFLAGS" KAI_BACKEND=c "$KAI" build "$src" -o "$bin" --backend=c \
    >/dev/null 2>"$TMP/build.log" || { echo "BUILD FAILED"; cat "$TMP/build.log"; exit 1; }

  # N=1 reference output (no threads, no TSAN cross-thread reports).
  out1="$(KAI_THREADS=1 "$bin")"

  # N=4 under TSAN: exitcode=99 makes a data race fail the process.
  echo "== running $name at KAI_THREADS=4 under TSAN =="
  set +e
  out4="$(TSAN_OPTIONS='halt_on_error=0 exitcode=99' KAI_THREADS=4 "$bin" 2>"$TMP/tsan.log")"
  ec=$?
  set -e
  races="$(grep -c 'WARNING: ThreadSanitizer' "$TMP/tsan.log" || true)"

  if [ "$races" != "0" ] || [ "$ec" = "99" ]; then
    echo "TSAN: $races race(s) in $name"; sed -n '1,80p' "$TMP/tsan.log"; fail=1; continue
  fi
  if [ "$out1" != "$out4" ]; then
    echo "DETERMINISM: $name diverged N=1 ('$out1') vs N=4 ('$out4')"; fail=1; continue
  fi
  echo "OK $name — TSAN clean, deterministic (N=1==N=4: $out4)"
done

[ "$fail" = "0" ] && echo "run-mn-tsan: OK (M:N TSAN clean + deterministic)" || { echo "run-mn-tsan: FAIL"; exit 1; }
