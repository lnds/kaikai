#!/bin/sh
# Native-modular objects are shared by every entry of a package: each test
# file under tests/ partitions like the entry does, and a partition two files
# project identically is emitted once. The gate is on what each run adds to
# the object cache: a second test file adds only its own partitions, a test
# edit leaves the other files' objects alone, a module edit re-emits only that
# module, and a module that shadows another is never served the shadowed one's
# object.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
KAIC2="$ROOT/stage2/kaic2"
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

fail() { echo "nmodular_shared_test_roots FAIL — $1"; exit 1; }

native_capable=0
if [ -x "$KAIC2" ]; then
  printf 'fn main() : Unit = ()\n' > "$PROJ/probe.kai"
  "$KAIC2" $EDITION_FLAG --emit=native --path "$ROOT/stdlib" --path "$PROJ" "$PROJ/probe.kai" \
    >/dev/null 2>"$PROJ/native-probe.err" || true
  grep -q "not built into this compiler" "$PROJ/native-probe.err" 2>/dev/null || native_capable=1
fi
if [ "$native_capable" != "1" ]; then
  echo "nmodular_shared_test_roots SKIP — kaic2 has no libLLVM native backend"
  exit 0
fi

PKG="$PROJ/pkg"
mkdir -p "$PKG/lib" "$PKG/tests"
printf 'name = "pkg"\nedition = "hanga-roa"\n' > "$PKG/kai.toml"
# Recursive, so the inliner keeps each module's fn in its own partition.
mod_x() { printf 'pub fn twice(n: Int) : Int = if n <= 0 { %s } else { 2 + twice(n - 1) }\n' "$1" > "$2"; }
mod_x 0 "$PKG/lib/x.kai"
printf 'pub fn thrice(n: Int) : Int = if n <= 0 { 0 } else { 3 + thrice(n - 1) }\n' > "$PKG/lib/y.kai"
printf 'import lib.x\n\ntest "a" {\n  assert x.twice(2) == 4\n}\n' > "$PKG/tests/a_test.kai"
printf 'import lib.x\nimport lib.y\n\ntest "b" {\n  assert x.twice(1) + y.thrice(1) == 5\n}\n' > "$PKG/tests/b_test.kai"

export KAI_NATIVE_MODULAR_CACHE_DIR="$PROJ/nmcache" KAI_CORE_CACHE_DIR="$PROJ/core-cache" KAI_BACKEND=native

objs() { find "$PROJ/nmcache" -name '*.o' -not -path '*/runtime/*' 2>/dev/null | sort; }

# Run `kai test <file>` (must exit $2) and print how many objects it added.
added() {
  objs > "$PROJ/before"
  rc=0; (cd "$PKG" && "$KAI" test "$1" > "$PROJ/out" 2>&1) || rc=$?
  [ "$rc" -eq "$2" ] || { cat "$PROJ/out"; fail "kai test $1 exited $rc, want $2"; }
  objs > "$PROJ/after"
  comm -13 "$PROJ/before" "$PROJ/after" | wc -l | tr -d ' '
}

n=$(added tests/a_test.kai 0)
[ "$n" -ge 2 ] || fail "a test file under tests/ did not partition ($n objects)"
n=$(added tests/b_test.kai 0)
[ "$n" -eq 2 ] || fail "b_test added $n objects; want 2 (its root and lib.y) beside a_test's lib.x"
n=$(added tests/a_test.kai 0)
[ "$n" -eq 0 ] || fail "an unchanged rerun emitted $n objects"

printf 'import lib.x\nimport lib.y\n\ntest "b" {\n  assert x.twice(2) + y.thrice(1) == 7\n}\n' > "$PKG/tests/b_test.kai"
n=$(added tests/a_test.kai 0)
[ "$n" -eq 0 ] || fail "editing b_test re-emitted $n objects for a_test"
n=$(added tests/b_test.kai 0)
[ "$n" -eq 1 ] || fail "editing b_test emitted $n objects; want 1 (its root)"

mod_x 1 "$PKG/lib/x.kai"
n=$(added tests/a_test.kai 1)
[ "$n" -eq 1 ] || fail "editing lib/x emitted $n objects for a_test; want 1 (lib.x)"
n=$(added tests/b_test.kai 1)
[ "$n" -eq 0 ] || fail "b_test re-emitted $n objects after a_test already rebuilt lib.x"

# A module beside the test file shadows the package's: its own object, never the shadowed one.
mkdir -p "$PKG/tests/lib"
mod_x 0 "$PKG/tests/lib/x.kai"
added tests/a_test.kai 0 > /dev/null

echo "nmodular_shared_test_roots OK — test files share the package's partition objects"
