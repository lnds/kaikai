#!/bin/sh
# A package whose `entry` sits in a subdirectory (`entry = "src/main.kai"`)
# resolves its modules from that directory for every file it owns, not
# for the entry alone: `kai test` finds `tests/` beside the manifest and
# beside the entry, and a test wherever it sits imports the package's
# modules as the entry does. `kai build`, `kai run` and `kai typecheck`
# of a file that is not the entry resolve the same way, and so does a
# package that depends on this one by path. `kai mutate` names a module
# as those imports do.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

pkg="$work/demo"
mkdir -p "$pkg/src/demo/core" "$pkg/src/tests" "$pkg/tests" "$pkg/bench"
printf 'name = "demo"\nversion = "0.1.0"\nentry = "src/main.kai"\n' > "$pkg/kai.toml"
printf '#[doc("Doubles.")]\npub fn twice(n: Int) : Int = n * 2\n' > "$pkg/src/demo/core/math.kai"
printf 'import demo.core.math\n\nfn main() : Int / Stdout {\n  Stdout.print("#{twice(21)}")\n  0\n}\n' > "$pkg/src/main.kai"
printf '#[doc("Halves.")]\npub fn half(n: Int) : Int = n / 2\n' > "$pkg/bench/half.kai"
printf 'import demo.core.math\n\ntest "beside the manifest" {\n  assert twice(2) == 4\n}\n' > "$pkg/tests/math_test.kai"
printf 'import demo.core.math\n\ntest "beside the entry" {\n  assert twice(3) == 6\n}\n' > "$pkg/src/tests/entry_side.kai"
printf 'import demo.core.math\n\ntest "beside the module" {\n  assert twice(4) == 8\n}\n' > "$pkg/src/demo/core/math_test.kai"

tool="$pkg/tool"
mkdir -p "$tool/tests"
printf 'name = "tool"\nversion = "0.1.0"\nentry = "main.kai"\n\n[dependencies]\ndemo = { path = ".." }\n' > "$tool/kai.toml"
printf 'import demo.core.math\n\nfn main() : Int / Stdout {\n  Stdout.print("#{twice(50)}")\n  0\n}\n' > "$tool/main.kai"
printf 'import demo.core.math\n\ntest "from a dependent" {\n  assert twice(5) == 10\n}\n' > "$tool/tests/dep_test.kai"

fail=0
# check <label> <expected substring> <kai args...>, run from the package
check() { check_in "$pkg" "$@"; }

# check_in <dir> <label> <expected substring> <kai args...>
check_in() {
  dir="$1"; label="$2"; want="$3"; shift 3
  rc=0
  (cd "$dir" && env KAI_BACKEND=c KAI_CORE_CACHE_DIR="$work/core" "$KAI" "$@") > "$work/out" 2>&1 || rc=$?
  if [ "$rc" -ne 0 ] || { [ -n "$want" ] && ! grep -q "$want" "$work/out"; }; then
    echo "  FAIL $label (status $rc, expected '$want')"; sed 's/^/    /' "$work/out" | tail -8
    fail=$((fail + 1))
  else
    echo "  ok   $label"
  fi
}

check "run" '^42$' run
check "test, whole package" 'tests passed' test
grep -q 'ok   beside the manifest' "$work/out" && grep -q 'ok   beside the entry' "$work/out" \
  && grep -q 'ok   beside the module' "$work/out" \
  || { echo "  FAIL test, whole package: a test directory or file was not run"; fail=$((fail + 1)); }
check "test, file beside the manifest" '1/1 tests passed' test tests/math_test.kai
check "test, file beside the entry" '1/1 tests passed' test src/tests/entry_side.kai
check "test, file beside the module" '1/1 tests passed' test src/demo/core/math_test.kai
check "typecheck, a test file" '' typecheck tests/math_test.kai
check "mutate, a module is named as it is imported" '"enclosing": "demo.core.math.twice/1"' \
  mutate --list --json --module src/demo/core/math.kai
check "mutate, a module outside the entry's directory is named from the package" '"enclosing": "bench.half.half/1"' \
  mutate --list --json --module bench/half.kai

check "dependent, run from the dependency" '^100$' run ./tool
check_in "$tool" "dependent, run" '^100$' run .
check_in "$tool" "dependent, build" '' build -o "$work/tool-bin"
[ "$("$work/tool-bin" 2> /dev/null)" = 100 ] || { echo "  FAIL dependent, build: the binary does not print 100"; fail=$((fail + 1)); }
check_in "$tool" "dependent, test" 'ok   from a dependent' test

if [ "$fail" -ne 0 ]; then
  echo "src-entry FAIL ($fail)"
  exit 1
fi
echo "src-entry OK"
