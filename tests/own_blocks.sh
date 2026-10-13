#!/bin/sh
# A package's test, bench and check runs hold its own blocks and no other:
# none of a stdlib module or a dependency it imports, directly or through
# another. A module of the package runs its blocks under each root that
# loads it.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cp -R "$ROOT/tests/own_blocks" "$WORK/pkgs"
fail=0

# The run's block names, one per line, sorted: `want` is what the package owns.
expect() {
  label="$1"; want="$2"; got="$3"
  if [ "$got" != "$want" ]; then
    echo "own-blocks FAIL ($label)"; echo "  want:"; echo "$want" | sed 's/^/    /'
    echo "  got:"; echo "$got" | sed 's/^/    /'; fail=1
  fi
}

run() { (cd "$WORK/pkgs/app" && XDG_CACHE_HOME="$WORK/cache" "$KAI" "$@"); }

# The C backend, then the default one: native, or C again on a kaic2 without libLLVM.
for backend in --backend=c ""; do
  ids="$(run test $backend --json . 2> "$WORK/err" | sed -n 's/^{"type":"test","id":"\([^"]*\)".*/\1/p' | sort)"
  expect "test $backend" "main.kai:app: main
main.kai:helper: twice
solo_test.kai:helper: twice
solo_test.kai:solo: own
tests/a_test.kai:a: own
tests/a_test.kai:common: three
tests/a_test.kai:helper: twice
tests/b_test.kai:b: own
tests/b_test.kai:common: three
tests/b_test.kai:helper: twice
tests/common.kai:common: three" "$ids"

  benches="$(run bench $backend --iters 2 . 2>&1 | sed -n 's/^  \(.*\): [0-9]* iter \/ .*/\1/p' | sort)"
  expect "bench $backend" "app: main
helper: twice" "$benches"

  checks="$(run check $backend . 2>&1 | sed -n 's/^  \(.*\): [0-9]* iter, OK$/\1/p' | sort)"
  expect "check $backend" "app: main
helper: twice" "$checks"
done
[ "$fail" = 0 ] && echo "own-blocks OK"
exit "$fail"
