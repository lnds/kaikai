#!/usr/bin/env bash
# Runs the test blocks of every core module and of stdlib/text.kai with the file as the
# root, where it is its own module: each must compile, and a file that holds `test`
# blocks must run them.
#
#   tools/test-core-modules.sh        env: KAI_TEST_DRIVER (default bin/kai), KAI_TEST_BACKEND (default c)
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="${KAI_TEST_DRIVER:-$ROOT/bin/kai}"
BACKEND="${KAI_TEST_BACKEND:-c}"

files="$( {
  sed -nE '/^pub(\([a-z]+\))? fn core_module_files\(\)/,/^\]/s/^ *"([^"]*\.kai)",*$/\1/p' \
    "$ROOT/stage2/compiler/modules_paths.kai"
  (cd "$ROOT/stdlib" && ls core/*.kai)
  echo text.kai
} | sort -u )"
[ -n "$files" ] || { echo "test-core-modules: no core module listed" >&2; exit 2; }

fail=0
for f in $files; do
  src="$ROOT/stdlib/$f"
  want="$(grep -c '^test ' "$src" || true)"
  out="$(KAI_STDLIB="$ROOT/stdlib" "$KAI" test --backend="$BACKEND" "$src" 2>&1)"
  rc=$?
  ran="$(printf '%s\n' "$out" | sed -nE 's/^([0-9]+)\/([0-9]+) tests passed$/\1 \2/p' | tail -n 1)"
  if [ "$rc" -ne 0 ]; then
    echo "test-core-modules: FAIL $f (exit $rc)"; printf '%s\n' "$out" | head -20; fail=1
  elif [ "$want" -gt 0 ] && { [ -z "$ran" ] || [ "${ran% *}" = 0 ] || [ "${ran% *}" != "${ran#* }" ]; }; then
    echo "test-core-modules: FAIL $f ($want test blocks, ran: ${ran:-none})"; fail=1
  else
    echo "test-core-modules: ok $f (${ran:-no tests})"
  fi
done
[ "$fail" -eq 0 ] && echo "test-core-modules OK"
exit "$fail"
