#!/bin/sh
# The core the compiler loads is not re-checked against the naming table on
# every run; this gate holds every stdlib file to it instead, by parsing each
# as a root file, where the binder pass runs. Any error from that parse fails.
#
# Usage: tools/stdlib-naming-gate.sh

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
out="${TMPDIR:-/tmp}/stdlib-naming.$$"
fail=0

for f in $(find "$ROOT/stdlib" -name '*.kai' | sort); do
  if ! "$KAIC2" --path "$ROOT/stdlib" --ast "$f" > /dev/null 2> "$out"; then
    echo "stdlib-naming-gate FAIL — $f"
    grep -m1 '^error' "$out" || true
    fail=1
  fi
done

rm -f "$out"
[ "$fail" = 0 ] && echo "stdlib-naming-gate OK"
exit "$fail"
