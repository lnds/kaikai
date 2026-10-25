#!/bin/sh
# examples/packages/cycle_field_read — the modules of an import cycle read
# the fields of each other's records whichever sorts first, under `kai test`
# on the default backend.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
KAI="$ROOT/bin/kai"

out="$(cd "$DIR" && "$KAI" test tests/cycle_test.kai 2>&1)" || {
  echo "cycle_field_read: FAIL — $out" >&2
  exit 1
}
echo "$out" | grep -q "1/1 tests passed" || { echo "cycle_field_read: FAIL — $out" >&2; exit 1; }
