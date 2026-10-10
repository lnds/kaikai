#!/bin/sh
# A test file whose only declarations are `test` blocks sees its own
# imports, both built alone and grouped with its sibling.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cp -R "$ROOT/tests/test_only_root" "$WORK/pkg"
fail=0
run() { (cd "$WORK/pkg" && XDG_CACHE_HOME="$WORK/cache" "$KAI" test "$@" > "$WORK/out" 2>&1); }
run || { echo "test-only-root FAIL (grouped)"; cat "$WORK/out"; fail=1; }
run tests/open_test.kai || { echo "test-only-root FAIL (open_test alone)"; cat "$WORK/out"; fail=1; }
run tests/pick_test.kai || { echo "test-only-root FAIL (pick_test alone)"; cat "$WORK/out"; fail=1; }
[ "$fail" = 0 ] && echo "test-only-root OK"
exit "$fail"
