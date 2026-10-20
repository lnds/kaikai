#!/bin/sh
# Package membership (`pub(pkg)`, intra-package import cycles) holds on the explicit native backend
# for run, build and test: the entry is compiled from its own path, so the nearest kai.toml is the
# package's. `--backend=native` fails outright on a kaic2 without libLLVM instead of degrading to C.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cp -R "$ROOT/tests/pub_pkg" "$WORK/pub_pkg"
cp -R "$ROOT/tests/intra_pkg_cycles" "$WORK/cycles"
export XDG_CACHE_HOME="$WORK/cache"
fail=0

check_pkg() {
  label="$1"; dir="$2"
  want="$(cat "$dir/expect.out")"
  got="$(cd "$dir" && "$KAI" run --backend=native main.kai 2> "$WORK/err")" || {
    echo "native-pkg FAIL ($label run): did not run"; cat "$WORK/err"; fail=1; return; }
  [ "$got" = "$want" ] || { echo "native-pkg FAIL ($label run): got '$got'"; fail=1; }
  (cd "$dir" && "$KAI" build --backend=native main.kai -o "$WORK/$label.bin" 2> "$WORK/err") || {
    echo "native-pkg FAIL ($label build): did not build"; cat "$WORK/err"; fail=1; return; }
  got="$("$WORK/$label.bin")"
  [ "$got" = "$want" ] || { echo "native-pkg FAIL ($label build): got '$got'"; fail=1; }
  [ -z "$(find "$dir" -name '*.o')" ] || { echo "native-pkg FAIL ($label): an object landed in the source tree"; fail=1; }
}

check_pkg pub_pkg "$WORK/pub_pkg/pos/same_package"
check_pkg pub_pkg_tests "$WORK/pub_pkg/pos/with_tests"
for dir in "$WORK"/cycles/pos/*; do check_pkg "cycle_$(basename "$dir")" "$dir"; done

out="$(cd "$WORK/pub_pkg/pos/with_tests" && "$KAI" test --backend=native . 2>&1)" || true
echo "$out" | grep -q "helper is visible to the package's tests" || { echo "native-pkg FAIL (test): $out"; fail=1; }

[ "$fail" = 0 ] && echo "native-pkg OK"
exit "$fail"
