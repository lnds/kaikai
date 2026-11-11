#!/bin/sh
# A package name that is not an import segment is refused before anything
# compiles: it never becomes more than one argv element, never reaches a
# path outside the package, and no file the manifest points at is touched.

set -eu

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
KAI="$ROOT/bin/kai"
BASE="$(mktemp -d)"
PROJ="$BASE/pkg"
mkdir "$PROJ"
trap 'rm -rf "$BASE"' EXIT INT TERM

printf 'fn main() : Unit / Console = println("ok")\n' > "$PROJ/main.kai"
printf 'keep\n' > "$PROJ/target"

fail=0

reject() {
  name="$1"
  printf 'name = "%s"\nversion = "0.1.0"\n' "$name" > "$PROJ/kai.toml"
  for verb in build test; do
    set +e
    ( cd "$PROJ" && "$KAI" $verb ) >"$BASE/out" 2>&1
    status=$?
    set -e
    if [ "$status" -eq 0 ]; then
      echo "invalid_package_name: FAIL — kai $verb accepted '$name'" >&2
      fail=1
    elif ! grep -q "invalid package name" "$BASE/out"; then
      echo "invalid_package_name: FAIL — kai $verb gave no diagnostic for '$name'" >&2
      cat "$BASE/out" >&2
      fail=1
    fi
  done
}

reject "demo --test-plan $PROJ/target"
if [ "$(cat "$PROJ/target")" != "keep" ]; then
  echo "invalid_package_name: FAIL — the flag in the name rewrote its target" >&2
  fail=1
fi

reject "../escape"
reject "a/b"
reject ".."

if [ -e "$BASE/escape" ]; then
  echo "invalid_package_name: FAIL — output escaped the package" >&2
  fail=1
fi

printf 'name = "ok"\nversion = "0.1.0"\n\n[native]\npkg-config = ["--version"]\n' > "$PROJ/kai.toml"
set +e
( cd "$PROJ" && "$KAI" build ) >"$BASE/out" 2>&1
status=$?
set -e
if [ "$status" -eq 0 ] || ! grep -q "reads as a flag" "$BASE/out"; then
  echo "invalid_package_name: FAIL — a pkg-config name read as a flag was not refused" >&2
  cat "$BASE/out" >&2
  fail=1
fi

printf 'name = "ok_name-1"\nversion = "0.1.0"\n' > "$PROJ/kai.toml"
( cd "$PROJ" && "$KAI" build ) >"$BASE/out" 2>&1 || {
  echo "invalid_package_name: FAIL — a legal name no longer builds" >&2
  cat "$BASE/out" >&2
  fail=1
}

[ "$fail" -eq 0 ] && echo "invalid_package_name: OK"
exit "$fail"
