#!/bin/sh
# examples/packages/module_tests_once — a module's test blocks run once,
# in the package's binary and under the module's own file, however many
# test files import it; a test file runs only its own blocks. Fixtures
# are generated in a tmpdir.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
KAI="$ROOT/bin/kai"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

fail() { echo "module_tests_once: FAIL — $1" >&2; exit 1; }

count() { echo "$2" | grep -c "$1" || true; }

mk_pkg() {
  mkdir -p "$TMP/$1/tests" "$TMP/$1/util"
  printf 'name = "%s"\nedition = "hanga-roa"\n' "$1" > "$TMP/$1/kai.toml"
  printf 'pub fn inc(n: Int) : Int = n + 1\n\ntest "inc" {\n  assert inc(1) == 2\n}\n' > "$TMP/$1/calc.kai"
  printf 'pub fn dbl(n: Int) : Int = n * 2\n\ntest "dbl" {\n  assert dbl(2) == 4\n}\n' > "$TMP/$1/util/num.kai"
  for x in a b; do
    printf 'import calc\nimport util.num\n\ntest "%s own" {\n  assert calc.inc(1) + num.dbl(1) == 4\n}\n' "$x" > "$TMP/$1/tests/${x}_test.kai"
  done
}

# 1 — an entry-less package: each module's blocks run once.
mk_pkg lib
out="$(cd "$TMP/lib" && "$KAI" test . 2>&1)" || fail "'kai test .' failed on the library"
[ "$(count 'ok   inc' "$out")" -eq 1 ] || fail "calc's block did not run exactly once"
[ "$(count 'ok   dbl' "$out")" -eq 1 ] || fail "util/num's block did not run exactly once"
[ "$(count 'ok   a own' "$out")" -eq 1 ] || fail "a_test's own block did not run once"
[ "$(count 'ok   b own' "$out")" -eq 1 ] || fail "b_test's own block did not run once"
echo "$out" | grep -q "not reachable" && fail "an imported module drew the unreachable warning"

# 2 — a module's blocks are reported under the module's own file.
out="$(cd "$TMP/lib" && "$KAI" test --json . 2>/dev/null)"
echo "$out" | grep -q '"id":"calc.kai:inc","file":"calc.kai"' || fail "calc's block is not under calc.kai"
echo "$out" | grep -q '"id":"util/num.kai:dbl","file":"util/num.kai"' || fail "num's block is not under util/num.kai"
[ "$(count '"id":"[a-z_/]*test.kai:inc"' "$out")" -eq 0 ] || fail "a test file reported calc's block"

# 3 — with an entry that reaches calc, the entry's binary runs it once.
mk_pkg app
printf 'import calc\n\nfn main() : Int = calc.inc(0) - 1\n' > "$TMP/app/main.kai"
out="$(cd "$TMP/app" && "$KAI" test . 2>&1)" || fail "'kai test .' failed on the package with an entry"
[ "$(count 'ok   inc' "$out")" -eq 1 ] || fail "calc's block did not run exactly once under an entry"
[ "$(count 'ok   dbl' "$out")" -eq 1 ] || fail "util/num's block did not run exactly once under an entry"

# 4 — a module nobody imports keeps the warning.
printf 'pub fn stray() : Int = 1\n\ntest "stray" {\n  assert stray() == 1\n}\n' > "$TMP/app/stray.kai"
out="$(cd "$TMP/app" && "$KAI" test . 2>&1)" || fail "'kai test .' failed with a stray module"
echo "$out" | grep -q "stray.kai declares test blocks but is not reachable" || fail "an unimported module lost its warning"

echo "module_tests_once: ok"
