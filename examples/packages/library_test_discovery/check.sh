#!/bin/sh
# examples/packages/library_test_discovery — a package with no entry
# point must still run its tests in package mode. The failure shape
# this guards: the entry was resolved before discovery got a chance,
# so `kai test` died on the missing entry and `kai test ./...` skipped
# the package, leaving a library with no package-mode way to run its
# suite. Fixtures are generated in a tmpdir so no committed package
# carries a deliberately failing test.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
KAI="$ROOT/bin/kai"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

fail() { echo "library_test_discovery: FAIL — $1" >&2; exit 1; }

# Assertion helpers: each keeps one `kai` invocation and its
# expectations to a single line at the call site, so the cases below
# read as a table of behaviours rather than a chain of conditionals.
saw() { echo "$2" | grep -q "$1" || fail "$3"; }
missing() { echo "$2" | grep -q "$1" && fail "$3"; return 0; }

# Run `kai $@` in package dir $1; stdout+stderr on stdout, must succeed.
ok_run() {
  d="$1"; shift
  (cd "$TMP/$d" && "$KAI" "$@" 2>&1) || fail "'kai $*' failed in $d"
}

# Same, but the run must fail — a suite that cannot report a failure
# is worse than no suite.
bad_run() {
  d="$1"; shift
  st=0
  out="$(cd "$TMP/$d" && "$KAI" "$@" 2>&1)" || st=$?
  [ "$st" -ne 0 ] || fail "'kai $*' unexpectedly succeeded in $d"
  printf '%s' "$out"
}

# A library: a manifest, modules, tests — and no entry file.
mk_lib() {
  mkdir -p "$TMP/$1"
  printf 'name = "%s"\nedition = "hanga-roa"\n' "$1" > "$TMP/$1/kai.toml"
  printf 'pub fn doble(n: Int) : Int = n * 2\n' > "$TMP/$1/ops.kai"
}

mk_lib lib
cat > "$TMP/lib/ops_test.kai" <<'EOF'
import ops

test "library test runs without an entry" {
  assert ops.doble(7) == 14
}
EOF

# 1 — `kai test` with no argument runs the suite instead of dying on
# the missing entry.
out="$(ok_run lib test)"
saw "1/1 tests passed" "$out" "'kai test' did not run the library's test"
missing "does not exist" "$out" "'kai test' still reports the missing entry"

# 2 — `kai test .` is the same path with the spec spelled out.
out="$(ok_run lib test .)"
saw "1/1 tests passed" "$out" "'kai test .' did not run the library's test"

# 3 — the recursive walk runs the library rather than skipping it.
out="$(ok_run . test ./...)"
saw "1/1 tests passed" "$out" "'kai test ./...' did not run the library's test"
missing "SKIP" "$out" "'kai test ./...' still skips an entry-less package with tests"

# 4 — a failing library test still fails every form.
mk_lib broken
cat > "$TMP/broken/ops_test.kai" <<'EOF'
import ops

test "library failing assert" {
  assert ops.doble(7) == 999
}
EOF
out="$(bad_run broken test .)"
saw "FAIL" "$out" "'kai test .' did not report the failing assert"
bad_run . test ./... >/dev/null
rm -rf "$TMP/broken"

# 5 — check and bench share the entry resolver, so they gain the same
# reach. A library's property must be checked, not skipped.
mk_lib props
cat > "$TMP/props/ops_test.kai" <<'EOF'
import ops

check "doubling is even" with n: Int {
  ops.doble(n) % 2 == 0
}

bench "doubling" {
  ops.doble(21)
}
EOF
out="$(ok_run props check .)"
saw "1/1 checks passed" "$out" "'kai check .' did not run the library's check"
out="$(ok_run props bench . --iters 1)"
saw "doubling" "$out" "'kai bench .' did not run the library's bench"

# 6 — a package with neither an entry nor tests is still skipped by
# the walk: the fix widens what `./...` reaches, it does not make
# every directory buildable.
mk_lib bare
rm -f "$TMP/bare/ops_test.kai"
out="$(ok_run . test ./...)"
saw "SKIP bare" "$out" "'kai test ./...' did not skip an entry-less package with no tests"

# 7 — `kai build` / `kai run` keep erroring on the missing entry: a
# library has nothing to build, and only the block runners are
# meaningful without an entry.
out="$(bad_run lib build .)"
saw "does not exist" "$out" "'kai build .' lost the missing-entry diagnostic"
bad_run lib run . >/dev/null

echo "library_test_discovery: ok"
