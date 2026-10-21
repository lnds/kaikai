#!/bin/sh
# A package build builds the kai-pkg helper under a lock before anything else.
# In a tree that does not carry the helper's source that lock cannot even be
# created; the wrapper used to read the failed mkdir as another invocation
# holding it and waited two minutes before giving up. It must fail at once and
# name what is missing.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TREE="$(mktemp -d)"
trap 'rm -rf "$TREE"' EXIT INT TERM

# A development layout: stage0/ and stdlib/ beside bin/kai, without tools/kai-pkg.
mkdir -p "$TREE/bin" "$TREE/stage0" "$TREE/stage2"
cp "$ROOT/bin/kai" "$TREE/bin/kai"
ln -s "$ROOT/stdlib" "$TREE/stdlib"
ln -s "$ROOT/stage2/kaic2" "$TREE/stage2/kaic2"
cp "$ROOT/VERSION" "$ROOT/EDITION" "$TREE/"

PROJ="$TREE/proj"
mkdir -p "$PROJ"
printf 'name = "p"\nversion = "0.1.0"\nedition = "%s"\n' "$(cat "$ROOT/EDITION")" > "$PROJ/kai.toml"
printf 'fn main() : Unit = ()\n' > "$PROJ/main.kai"

start=$(date +%s)
set +e
(cd "$PROJ" && perl -e 'alarm 30; exec @ARGV' "$TREE/bin/kai" typecheck) >"$TREE/out" 2>&1
rc=$?
set -e
elapsed=$(( $(date +%s) - start ))

if [ "$rc" -eq 0 ] || [ "$rc" -eq 142 ] || [ "$elapsed" -ge 20 ]; then
  echo "wrapper_pkg_helper_missing FAIL — rc=$rc after ${elapsed}s, expected a prompt failure"
  cat "$TREE/out"
  exit 1
fi
grep -q "kai-pkg source missing" "$TREE/out" || {
  echo "wrapper_pkg_helper_missing FAIL — the failure does not name the missing helper source"
  cat "$TREE/out"
  exit 1
}
echo "wrapper_pkg_helper_missing OK — a tree without the kai-pkg source fails in ${elapsed}s and says so"
