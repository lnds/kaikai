#!/bin/sh
# examples/packages/default_inherit_lib — a handle in an imported module inherits
# the kaikai default of an effect declared in another module, with the core pruned
# or whole, on both backends.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
KAI="$ROOT/bin/kai"

for prune in on off; do
  for backend in c native; do
    out="$(cd "$DIR" && KAI_CORE_PRUNE=$prune "$KAI" test --backend=$backend tests/inherit_test.kai 2>&1)" || {
      echo "default_inherit_lib: FAIL (prune=$prune $backend) — $out" >&2
      exit 1
    }
    echo "$out" | grep -q "1/1 tests passed" || {
      echo "default_inherit_lib: FAIL (prune=$prune $backend) — $out" >&2
      exit 1
    }
  done
done
exit 0
