#!/bin/sh
# `pub(pkg)` is seen by the modules of the declaring package, its tests included, and by no one
# else; a `pub` signature may not expose one; `kai doc` leaves it out; the formatter keeps it.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cp -R "$ROOT/tests/pub_pkg" "$WORK/pkgs"
export XDG_CACHE_HOME="$WORK/cache"
fail=0

entry_of() { if [ -d "$1/consumer" ]; then echo "$1/consumer"; else echo "$1"; fi; }

for dir in "$WORK"/pkgs/pos/*; do
  name="$(basename "$dir")"
  [ -f "$dir/expect.out" ] || continue
  for backend in --backend=c ""; do
    got="$(cd "$(entry_of "$dir")" && "$KAI" run $backend main.kai 2> "$WORK/err")" || {
      echo "pub-pkg FAIL ($name $backend): did not run"; cat "$WORK/err"; fail=1; continue; }
    [ "$got" = "$(cat "$dir/expect.out")" ] || { echo "pub-pkg FAIL ($name $backend): got '$got'"; fail=1; }
  done
done

for dir in "$WORK"/pkgs/neg/*; do
  name="$(basename "$dir")"
  if (cd "$(entry_of "$dir")" && "$KAI" run --backend=c main.kai > /dev/null 2> "$WORK/err"); then
    echo "pub-pkg FAIL ($name): compiled"; fail=1; continue
  fi
  while IFS= read -r line; do
    grep -qF -- "$line" "$WORK/err" || { echo "pub-pkg FAIL ($name): no '$line' in:"; cat "$WORK/err"; fail=1; }
  done < "$dir/expect.err"
done

out="$(cd "$WORK/pkgs/pos/with_tests" && "$KAI" test --backend=c . 2>&1)" || true
echo "$out" | grep -q "helper is visible to the package's tests" || { echo "pub-pkg FAIL (tests/ dir): $out"; fail=1; }

docs="$(cd "$WORK/pkgs/pos/documented" && "$KAI" doc util 2>&1)"
echo "$docs" | grep -q "double" || { echo "pub-pkg FAIL (doc): pub item missing"; fail=1; }
if echo "$docs" | grep -q "helper\|undocumented"; then echo "pub-pkg FAIL (doc): a pub(pkg) item is listed"; fail=1; fi

"$KAI" fmt --check "$WORK/pkgs/fmt/sample.kai" > /dev/null || { echo "pub-pkg FAIL (fmt): pub(pkg) did not survive"; fail=1; }

[ "$fail" = 0 ] && echo "pub-pkg OK"
exit "$fail"
