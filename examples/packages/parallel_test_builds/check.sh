#!/bin/sh
# examples/packages/parallel_test_builds — `kai test -j <n>` builds the
# package's test binaries in parallel but must read exactly as a build
# in turn: same stdout, stderr and exit status as `-j 1`, results in
# file order, a mid-suite compile error stopping where it does today,
# and every run rebuilt from the sources as they are now.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
KAI="$ROOT/bin/kai"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

fail() { echo "parallel_test_builds: FAIL — $1" >&2; exit 1; }

# Run `kai test <args>` in package $1 at -j 1 and -j 4; both must agree byte for byte.
same() {
  pkg="$1"; shift
  for j in 1 4; do
    rc=0
    (cd "$TMP/$pkg" && "$KAI" test -j "$j" "$@" > "$TMP/out.$j" 2> "$TMP/err.$j") || rc=$?
    echo "$rc" > "$TMP/rc.$j"
  done
  cmp -s "$TMP/rc.1" "$TMP/rc.4" || fail "$pkg $*: exit $(cat "$TMP/rc.1") at -j 1, $(cat "$TMP/rc.4") at -j 4"
  cmp -s "$TMP/out.1" "$TMP/out.4" || { diff "$TMP/out.1" "$TMP/out.4" >&2; fail "$pkg $*: stdout differs"; }
  cmp -s "$TMP/err.1" "$TMP/err.4" || { diff "$TMP/err.1" "$TMP/err.4" >&2; fail "$pkg $*: stderr differs"; }
}

# The same, with stdout and stderr in one stream: the replayed builds keep their place.
mixed() {
  pkg="$1"; shift
  for j in 1 4; do
    (cd "$TMP/$pkg" && "$KAI" test -j "$j" "$@" > "$TMP/mix.$j" 2>&1) || true
  done
  cmp -s "$TMP/mix.1" "$TMP/mix.4" || { diff "$TMP/mix.1" "$TMP/mix.4" >&2; fail "$pkg $*: interleaved output differs"; }
}

mk_suite() {
  mkdir -p "$TMP/$1/tests" "$TMP/$1/lib"
  printf 'name = "%s"\nedition = "hanga-roa"\n' "$1" > "$TMP/$1/kai.toml"
  printf 'pub fn doble(n: Int) : Int = n * 2\n' > "$TMP/$1/lib/ops.kai"
  for t in a b c; do
    printf 'import lib.ops\n\ntest "%s doubles" {\n  let unused = 1\n  assert ops.doble(2) == 4\n}\n' "$t" > "$TMP/$1/tests/${t}_test.kai"
  done
  printf 'import lib.ops\n\ntest "b fails" {\n  assert ops.doble(2) == 5\n}\n' >> "$TMP/$1/tests/b_test.kai"
  printf 'import lib.ops\n\ntest "orphan" {\n  assert ops.doble(3) == 6\n}\n' > "$TMP/$1/lib/ops_test.kai"
  printf 'test "invisible" {\n  assert 1 == 1\n}\n' > "$TMP/$1/lib/loose.kai"
}

# 1 — an app: entry, tests/, an orphan *_test.kai and a warned file; one failing block.
mk_suite app
printf 'import lib.ops\n\nfn main() {\n  print(int_to_string(ops.doble(1)))\n}\n' > "$TMP/app/main.kai"
same app .
mixed app .
[ "$(cat "$TMP/rc.4")" -ne 0 ] || fail "the failing block did not fail the run"
grep -q "FAIL b fails" "$TMP/err.4" || fail "the failing block was not reported"
grep -q "warning: unused binding" "$TMP/err.4" || fail "build warnings were not replayed"
same app --json .
same app --only "tests/c_test.kai:c doubles" .

# 2 — a library (no entry): the first build settles the backend before any runs ahead.
mk_suite lib
same lib .

# 3 — a compile error mid-suite stops the run there, after the files before it ran.
printf 'test "broken" {\n  assert nope() == 1\n}\n' >> "$TMP/app/tests/b_test.kai"
same app .
mixed app .
[ "$(cat "$TMP/rc.4")" -ne 0 ] || fail "a compile error did not fail the run"
grep -q "a doubles" "$TMP/err.4" || fail "the files before the error did not run"
if grep -q "c doubles" "$TMP/err.4"; then fail "a file after the compile error ran"; fi

# 4 — every run builds the sources as they are now: an edit to the package or to a test shows.
printf 'pub fn doble(n: Int) : Int = n * 2\n' > "$TMP/lib/lib/ops.kai"
printf 'import lib.ops\n\ntest "b passes" {\n  assert ops.doble(2) == 4\n}\n' > "$TMP/lib/tests/b_test.kai"
(cd "$TMP/lib" && "$KAI" test -j 4 . > /dev/null 2>&1) || fail "the fixed suite did not pass"
printf 'pub fn doble(n: Int) : Int = n * 3\n' > "$TMP/lib/lib/ops.kai"
rc=0; (cd "$TMP/lib" && "$KAI" test -j 4 . > /dev/null 2> "$TMP/err.edit") || rc=$?
[ "$rc" -ne 0 ] || fail "a package edit did not reach the next run"
grep -q "FAIL c doubles" "$TMP/err.edit" || fail "the edited package was not rebuilt for every test file"

echo "parallel_test_builds: OK"
