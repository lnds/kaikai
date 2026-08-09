#!/bin/sh
# examples/packages/orphan_test_discovery — package-mode `kai test`
# must run *_test.kai files nothing imports. The failure shape this
# guards: a test file outside the entry's import graph was silently
# skipped, so `kai test .` / `kai test ./...` reported success over a
# broken suite. Fixtures are generated in a tmpdir so no committed
# package carries a deliberately failing test.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
KAI="$ROOT/bin/kai"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

fail() { echo "orphan_test_discovery: FAIL — $1" >&2; exit 1; }

mk_pkg() {
  mkdir -p "$TMP/$1"
  printf 'name = "%s"\nedition = "hanga-roa"\n' "$1" > "$TMP/$1/kai.toml"
  printf 'fn main() : Int = 0\n' > "$TMP/$1/main.kai"
}

# 1 — an orphan *_test.kai with a failing assert must fail the
# package run and name the failure.
mk_pkg broken
printf 'pub fn doble(n: Int) : Int = n * 2\n' > "$TMP/broken/ops.kai"
cat > "$TMP/broken/ops_test.kai" <<'EOF'
import ops

test "orphan failing assert" {
  assert ops.doble(7) == 999
}
EOF
status=0
out="$(cd "$TMP/broken" && "$KAI" test . 2>&1)" || status=$?
[ "$status" -ne 0 ] || fail "orphan failing test did not fail 'kai test .' (exit 0)"
echo "$out" | grep -q "FAIL" || fail "'kai test .' did not report the failing assert"

# 2 — the recursive walk must propagate that failure.
status=0
out="$(cd "$TMP" && "$KAI" test ./... 2>&1)" || status=$?
[ "$status" -ne 0 ] || fail "orphan failing test did not fail 'kai test ./...' (exit 0)"
echo "$out" | grep -q "FAIL" || fail "'kai test ./...' did not report the failing assert"

# 3 — an orphan *_test.kai that passes keeps the package green.
mk_pkg green
printf 'pub fn doble(n: Int) : Int = n * 2\n' > "$TMP/green/ops.kai"
cat > "$TMP/green/ops_test.kai" <<'EOF'
import ops

test "orphan passing assert" {
  assert ops.doble(7) == 14
}
EOF
out="$(cd "$TMP/green" && "$KAI" test . 2>&1)" || fail "passing orphan test broke 'kai test .'"
echo "$out" | grep -q "1/1 tests passed" || fail "passing orphan test did not run"

# 4 — test blocks in a file outside the graph that is not *_test.kai
# cannot run as a root; they must at least be named in a warning.
mk_pkg warned
printf 'test "invisible" {\n  assert 1 == 1\n}\n' > "$TMP/warned/loose.kai"
out="$(cd "$TMP/warned" && "$KAI" test . 2>&1)" || fail "'kai test .' failed on the warning-only package"
echo "$out" | grep -q "warning: loose.kai declares test blocks" || fail "unreachable non-convention test file was not warned about"

# 5 — a package with no test blocks anywhere says so.
mk_pkg empty
out="$(cd "$TMP/empty" && "$KAI" test . 2>&1)" || fail "'kai test .' failed on the empty package"
echo "$out" | grep -q "warning: no test blocks found" || fail "0-test package did not warn"

echo "orphan_test_discovery: OK"
