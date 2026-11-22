#!/bin/sh
# `kai test` and `kai build` agree on a qualified op shared by two effects in a
# signed row, and a diagnostic in an imported module names that module's file.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cp -R "$ROOT/tests/qualified_op_test" "$WORK/pkgs"
export XDG_CACHE_HOME="$WORK/cache"
fail=0

for backend in --backend=c ""; do
  dir="$WORK/pkgs/pos"
  (cd "$dir" && "$KAI" build $backend . -o "$WORK/pos.bin" > /dev/null 2> "$WORK/err") || {
    echo "qualified-op-test FAIL (build $backend):"; cat "$WORK/err"; fail=1; }
  (cd "$dir" && "$KAI" test $backend lib_test.kai > /dev/null 2> "$WORK/err") || {
    echo "qualified-op-test FAIL (test $backend):"; cat "$WORK/err"; fail=1; }
done

for name in neg_bare neg_import; do
  dir="$WORK/pkgs/$name"
  for backend in --backend=c ""; do
    if (cd "$dir" && "$KAI" test $backend lib_test.kai > /dev/null 2> "$WORK/err"); then
      echo "qualified-op-test FAIL ($name $backend): accepted"; fail=1; continue
    fi
    while IFS= read -r line; do
      grep -qF -- "$line" "$WORK/err" || { echo "qualified-op-test FAIL ($name $backend): no '$line' in:"; cat "$WORK/err"; fail=1; }
    done < "$dir/expect.err"
    if grep -q "lib_test.kai:" "$WORK/err"; then
      echo "qualified-op-test FAIL ($name $backend): diagnostic named the test file"; fail=1
    fi
  done
done

[ "$fail" = 0 ] && echo "qualified-op-test OK"
exit "$fail"
