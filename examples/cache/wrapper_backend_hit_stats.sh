#!/bin/sh
# Issue #1430 — core-cache liveness gate for the bin/kai WRAPPER itself,
# as opposed to corec_monolithic_hit_stats.sh (kaic2 invoked directly).
# The wrapper captures kaic2's stderr to a file on the native paths and
# only cats it on failure, which silently swallowed the
# --core-cache-stats hit/miss line on every successful native build
# (the cache stayed dead for two releases with no signal). This gate
# builds the same file twice through `bin/kai build` with
# KAI_CORE_CACHE_STATS=1 and asserts the second build reports a hit —
# on the C backend and, where a native-capable kaic2 is present, on the
# native backend too. A negative case with KAI_CORE_CACHE=0 proves the
# gate actually fails loud when the cache cannot engage, rather than
# passing vacuously on a missing line.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

cat > "$PROJ/main.kai" <<'EOF'
fn main() : Unit / Console {
  let xs = [1, 2, 3]
  print("sum=#{int_to_string(list.sum(xs))}")
}
EOF

out="$PROJ/out"
ccdir="$PROJ/core-cache"

assert_hit_on_second_build() {
  label="$1"; backend_flag="$2"
  rm -rf "$ccdir"
  mkdir -p "$ccdir"
  # shellcheck disable=SC2086
  KAI_CORE_CACHE_STATS=1 KAI_CORE_CACHE_DIR="$ccdir" \
    "$KAI" build $backend_flag "$PROJ/main.kai" -o "$out" 2>"$PROJ/$label.cold.err" 1>/dev/null
  if ! grep -q '^kaic2: core-parse-cache:' "$PROJ/$label.cold.err"; then
    echo "wrapper_backend_hit_stats FAIL ($label) — cold build printed no core-parse-cache stats line through the wrapper"
    cat "$PROJ/$label.cold.err"
    exit 1
  fi
  # shellcheck disable=SC2086
  KAI_CORE_CACHE_STATS=1 KAI_CORE_CACHE_DIR="$ccdir" \
    "$KAI" build $backend_flag "$PROJ/main.kai" -o "$out" 2>"$PROJ/$label.warm.err" 1>/dev/null
  if ! grep -q 'core-parse-cache: hit' "$PROJ/$label.warm.err"; then
    echo "wrapper_backend_hit_stats FAIL ($label) — warm build did not report a core-parse-cache hit through the wrapper"
    cat "$PROJ/$label.warm.err"
    exit 1
  fi
  "$out" > "$PROJ/$label.out" 2>/dev/null || true
  if [ "$(cat "$PROJ/$label.out")" != "sum=6" ]; then
    echo "wrapper_backend_hit_stats FAIL ($label) — program output wrong (got '$(cat "$PROJ/$label.out")')"
    exit 1
  fi
  echo "wrapper_backend_hit_stats OK ($label) — cold miss/off -> warm hit through bin/kai"
}

assert_hit_on_second_build c-backend --backend=c

# Native is the default backend; only assert it when this kaic2 was
# built with libLLVM (kaic2 --emit=native prints the "not built into
# this compiler" sentinel otherwise, and the wrapper degrades to C —
# same object, no separate stats line to check).
KAIC2="$ROOT/stage2/kaic2"
native_capable=0
if [ -x "$KAIC2" ]; then
  printf 'fn main() : Unit = ()\n' > "$PROJ/probe.kai"
  "$KAIC2" --emit=native --path "$ROOT/stdlib" --path "$PROJ" "$PROJ/probe.kai" \
    >/dev/null 2>"$PROJ/native-probe.err" || true
  grep -q "not built into this compiler" "$PROJ/native-probe.err" 2>/dev/null || native_capable=1
fi
if [ "$native_capable" = "1" ]; then
  assert_hit_on_second_build native-backend --backend=native
else
  echo "wrapper_backend_hit_stats SKIP (native-backend) — kaic2 has no libLLVM native backend"
fi

# Negative proof: with KAI_CORE_CACHE=0, bin/kai never passes
# --core-cache-stats to kaic2 at all (the cache cannot engage, so
# there is nothing to report), so no stats line should appear. This is
# the control that shows assert_hit_on_second_build's grep is a real
# gate reacting to presence/absence, not one that would pass no matter
# what kaic2 prints — manually reverting the emit_core_cache_stats
# redirect-order fix (>&2 2>/dev/null -> 2>/dev/null >&2, which sends
# both stdout and stderr to /dev/null) makes assert_hit_on_second_build
# FAIL on the native case with exactly this "no stats line" symptom,
# which is the bug this fixture exists to catch.
rm -rf "$ccdir"
KAI_CORE_CACHE_STATS=1 KAI_CORE_CACHE=0 \
  "$KAI" build --backend=c "$PROJ/main.kai" -o "$out" 2>"$PROJ/off.err" 1>/dev/null
if grep -q '^kaic2: core-parse-cache:' "$PROJ/off.err"; then
  echo "wrapper_backend_hit_stats FAIL (negative) — KAI_CORE_CACHE=0 should suppress the --core-cache-stats flag entirely (bin/kai never asks kaic2 for it), but a stats line was printed"
  cat "$PROJ/off.err"
  exit 1
fi
echo "wrapper_backend_hit_stats OK (negative) — KAI_CORE_CACHE=0 disables the flag as designed"

exit 0
