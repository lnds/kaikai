#!/bin/sh
# examples/packages/orphan_check_bench_discovery — package-mode `kai
# check` and `kai bench` must run the blocks in *_test.kai files
# nothing imports, the same discovery `kai test` does. The failure
# shape this guards: a check/bench block outside the entry's import
# graph never reached its runner, so `kai check .` printed "0/0 checks
# passed" and exited 0 over a property that does not hold. Packages
# are generated in a tmpdir so no committed one carries a deliberately
# failing property.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
KAI="$ROOT/bin/kai"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

fail() { echo "orphan_check_bench_discovery: FAIL — $1" >&2; exit 1; }

mk_pkg() {
  mkdir -p "$TMP/$1"
  printf 'name = "%s"\nedition = "hanga-roa"\n' "$1" > "$TMP/$1/kai.toml"
  printf 'fn main() : Int = 0\n' > "$TMP/$1/main.kai"
  printf 'pub fn doble(n: Int) : Int = n * 2\n' > "$TMP/$1/ops.kai"
}

# 1 — an orphan check block whose property does not hold must fail
# the package run and report the counterexample.
mk_pkg broken
cat > "$TMP/broken/ops_test.kai" <<'EOF'
import ops

check "doble is off by one" with n: Int {
  ops.doble(n) == n + 1
}

fn main() : Int = 0
EOF
status=0
out="$(cd "$TMP/broken" && "$KAI" check . 2>&1)" || status=$?
[ "$status" -ne 0 ] || fail "orphan failing check did not fail 'kai check .' (exit 0)"
echo "$out" | grep -q "counterexample" || fail "'kai check .' did not report the counterexample"

# 2 — an orphan check block that holds keeps the package green and
# is counted, so "0/0" can no longer stand in for "ran nothing".
mk_pkg green
cat > "$TMP/green/ops_test.kai" <<'EOF'
import ops

check "doble is n plus n" with n: Int {
  ops.doble(n) == n + n
}

fn main() : Int = 0
EOF
out="$(cd "$TMP/green" && "$KAI" check . 2>&1)" || fail "passing orphan check broke 'kai check .'"
echo "$out" | grep -q "1/1 checks passed" || fail "passing orphan check did not run"

# 3 — the same discovery arms `kai bench`.
mk_pkg benched
cat > "$TMP/benched/ops_test.kai" <<'EOF'
import ops

bench "doble" {
  ops.doble(7)
}

fn main() : Int = 0
EOF
out="$(cd "$TMP/benched" && "$KAI" bench --iters 2 . 2>&1)" || fail "'kai bench .' failed on the orphan bench package"
echo "$out" | grep -q "1 benches" || fail "orphan bench block did not run"

# 4 — blocks in a file outside the graph that is not *_test.kai
# cannot run as a root; they must at least be named in a warning.
mk_pkg warned
printf 'check "invisible" with n: Int {\n  n == n\n}\nfn main() : Int = 0\n' > "$TMP/warned/loose.kai"
out="$(cd "$TMP/warned" && "$KAI" check . 2>&1)" || fail "'kai check .' failed on the warning-only package"
echo "$out" | grep -q "warning: loose.kai declares check blocks" || fail "unreachable non-convention check file was not warned about"

# 5 — a package with no blocks of the kind says so instead of
# passing silently.
mk_pkg empty
out="$(cd "$TMP/empty" && "$KAI" check . 2>&1)" || fail "'kai check .' failed on the empty package"
echo "$out" | grep -q "warning: no check blocks found" || fail "0-check package did not warn"
out="$(cd "$TMP/empty" && "$KAI" bench . 2>&1)" || fail "'kai bench .' failed on the empty package"
echo "$out" | grep -q "warning: no bench blocks found" || fail "0-bench package did not warn"

echo "orphan_check_bench_discovery: OK"
