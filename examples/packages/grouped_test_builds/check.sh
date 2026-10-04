#!/bin/sh
# examples/packages/grouped_test_builds — the test files of one directory
# build as one binary that runs once per file. Each file still checks alone
# first, so one that does not build on its own fails as it does alone; a
# file's run is the blocks its own build would run, in that build's order; a
# failure that only the shared build hits fails the run before any test runs.
# Also: a block's unused binding is reported in the file that declares it,
# a module that does not parse fails the build even when nothing uses it, and
# a block reaches its own file's functions whatever else the build holds.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
KAI="$ROOT/bin/kai"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

fail() { echo "grouped_test_builds: FAIL — $1" >&2; exit 1; }

mk_pkg() {
  rm -rf "$TMP/$1"
  mkdir -p "$TMP/$1/tests" "$TMP/$1/lib"
  printf 'name = "%s"\nedition = "hanga-roa"\n' "$1" > "$TMP/$1/kai.toml"
  printf 'fn main() {\n  print("m")\n}\n' > "$TMP/$1/main.kai"
}

# Run `kai test .` in package $1 into $TMP/out and $TMP/err; the status in $TMP/rc.
run() {
  rc=0
  (cd "$TMP/$1" && "$KAI" test . > "$TMP/out" 2> "$TMP/err") || rc=$?
  echo "$rc" > "$TMP/rc"
}

# 1 — a file's run is its own build's blocks in its own build's order, though
# another file loads the same modules in another order.
mk_pkg order
printf 'pub fn x() : Int = 1\n\ntest "x block" {\n  assert x() == 1\n}\n' > "$TMP/order/lib/x.kai"
printf 'pub fn y() : Int = 2\n\ntest "y block" {\n  assert y() == 2\n}\n' > "$TMP/order/lib/y.kai"
printf 'import lib.x\n\ntest "a" {\n  assert x.x() == 1\n}\n' > "$TMP/order/tests/a_test.kai"
printf 'import lib.y\nimport lib.x\n\ntest "b" {\n  assert y.y() + x.x() == 3\n}\n' > "$TMP/order/tests/b_test.kai"
run order
[ "$(cat "$TMP/rc")" -eq 0 ] || { cat "$TMP/err"; fail "the ordered suite did not pass"; }
got="$(sed -n '/b_test.kai$/,$p' "$TMP/err" | grep -E '^  ok' | tr -s ' ' | tr '\n' '|')"
[ "$got" = " ok y block| ok x block| ok b|" ] || fail "b_test ran '$got'; want y block, x block, b"
got="$(sed -n '/a_test.kai$/,/b_test.kai$/p' "$TMP/err" | grep -E '^  ok' | tr -s ' ' | tr '\n' '|')"
[ "$got" = " ok x block| ok a|" ] || fail "a_test ran '$got'; want x block, a"

# 2 — a file that does not build alone fails as it does alone: the files before
# it ran, the ones after did not, though the shared build would compile it.
mk_pkg leak
printf 'pub fn base() : Int = 10\n' > "$TMP/leak/lib/core.kai"
printf 'import lib.core\n\ntest "a" {\n  assert core.base() == 10\n}\n' > "$TMP/leak/tests/a_test.kai"
printf 'test "b" {\n  assert core.base() == 10\n}\n' > "$TMP/leak/tests/b_test.kai"
printf 'test "c" {\n  assert 1 == 1\n}\n' > "$TMP/leak/tests/c_test.kai"
run leak
[ "$(cat "$TMP/rc")" -ne 0 ] || fail "a file that does not build alone passed"
grep -q "cannot find \`core\`" "$TMP/err" || fail "the file's own error was not reported"
grep -q "ok   a" "$TMP/err" || fail "the file before it did not run"
if grep -q "ok   c" "$TMP/err"; then fail "the file after it ran"; fi

# 3 — files that each build alone but not together fail the run, before any test runs.
mk_pkg dup
printf 'pub type Shape = Sq(Int)\n' > "$TMP/dup/lib/shape.kai"
for t in a b; do
  printf 'import lib.shape\n\nimpl Show for shape.Shape {\n  fn show(s: shape.Shape) : String = "%s"\n}\n\ntest "%s" {\n  assert show(shape.Sq(1)) == "%s"\n}\n' "$t" "$t" "$t" > "$TMP/dup/tests/${t}_test.kai"
done
run dup
[ "$(cat "$TMP/rc")" -ne 0 ] || fail "test files that do not build together passed"
grep -q "duplicate impl" "$TMP/err" || fail "the shared build's error was not reported"
if grep -q "ok   a" "$TMP/err"; then fail "a test ran although the shared build failed"; fi

# 4 — an unused binding in an imported module's block names that module's file.
mk_pkg warn
printf 'pub fn z() : Int = 1\n\ntest "z" {\n  let idle = 1\n  assert z() == 1\n}\n' > "$TMP/warn/lib/z.kai"
printf 'import lib.z\n\ntest "a" {\n  assert z.z() == 1\n}\n' > "$TMP/warn/tests/a_test.kai"
(cd "$TMP/warn" && "$KAI" test tests/a_test.kai > /dev/null 2> "$TMP/err") || fail "the warning suite failed"
grep -A1 "unused binding \`idle\`" "$TMP/err" | grep -q "lib/z.kai:4:" || fail "the block's unused binding did not name lib/z.kai"

# 5 — a module that does not parse fails the build though nothing uses it.
mk_pkg broken
printf 'pub fn later(x: Int) : Int = todo!()\n' > "$TMP/broken/lib/bad.kai"
printf 'import lib.bad\n\nfn main() {\n  print("ran")\n}\n' > "$TMP/broken/main.kai"
rc=0; (cd "$TMP/broken" && "$KAI" run main.kai > "$TMP/out" 2>&1) || rc=$?
[ "$rc" -ne 0 ] || fail "a module that does not parse built"
if grep -q "^ran$" "$TMP/out"; then fail "the program ran despite the parse error"; fi

# 6 — a file's own function is the one its blocks call, though a protocol op
# has the same name.
mk_pkg shadow
printf 'fn one() : Int = 41\n\ntest "own one" {\n  assert one() + 1 == 42\n}\n' > "$TMP/shadow/tests/a_test.kai"
printf 'test "b" {\n  assert 1 + 1 == 2\n}\n' > "$TMP/shadow/tests/b_test.kai"
run shadow
[ "$(cat "$TMP/rc")" -eq 0 ] || { cat "$TMP/err"; fail "a block called the protocol op instead of its file's function"; }

# 7 — a file that opens with a block reaches its own function, not the one of
# the same name in the file loaded before it.
mk_pkg first
printf 'fn helper() : Int = 1\n\ntest "a" {\n  assert helper() == 1\n}\n' > "$TMP/first/tests/a_test.kai"
printf 'test "b" {\n  assert helper() == 2\n}\n\nfn helper() : Int = 2\n' > "$TMP/first/tests/b_test.kai"
run first
[ "$(cat "$TMP/rc")" -eq 0 ] || { cat "$TMP/err"; fail "a block reached the function of the file loaded before its own"; }

echo "grouped_test_builds: OK"
