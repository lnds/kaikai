#!/bin/sh
# examples/packages/default_inherit_cycle — a handle that omits an op of an
# effect declared in a module it imports in a cycle cannot inherit the
# effect's kaikai default; the build says so.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
KAI="$ROOT/bin/kai"

if out="$(cd "$DIR" && "$KAI" test tests/inherit_test.kai 2>&1)"; then
  echo "default_inherit_cycle: FAIL — expected a compile error" >&2
  exit 1
fi
echo "$out" | grep -qF "import each other" || { echo "default_inherit_cycle: FAIL — $out" >&2; exit 1; }
echo "$out" | grep -q "__dflt" && { echo "default_inherit_cycle: FAIL — leaks __dflt: $out" >&2; exit 1; }
exit 0
