#!/bin/sh
# examples/packages/init_invalid_names — assert `kai init` rejects
# package names that fall outside [a-z][a-z0-9_-]* (issue #419).
# A bad name must:
#   1. exit non-zero
#   2. NOT create kai.toml
#   3. emit a message naming the grammar
# A good name must succeed and write kai.toml as before.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

BAD_NAMES="
name with spaces
name/with/slashes
name@version
../name
123digit-start
-dash-start
Name-Capital
"

fail=0

# IFS=newline so multi-word entries like "name with spaces" stay
# whole; iterating word-by-word would split them.
OLD_IFS="$IFS"
IFS='
'
for name in $BAD_NAMES; do
  [ -z "$name" ] && continue
  rm -rf "$TMP"/* 2>/dev/null || true

  set +e
  ( cd "$TMP" && "$ROOT/bin/kai" init "$name" >"$TMP/.stdout" 2>"$TMP/.stderr" )
  status=$?
  set -e

  if [ "$status" -eq 0 ]; then
    echo "init_invalid_names: FAIL — '$name' was accepted (exit 0)" >&2
    cat "$TMP/.stdout" >&2
    fail=1
    continue
  fi

  if [ -f "$TMP/kai.toml" ]; then
    echo "init_invalid_names: FAIL — '$name' rejected but kai.toml was created" >&2
    cat "$TMP/kai.toml" >&2
    fail=1
    continue
  fi

  if ! grep -q "invalid package name" "$TMP/.stderr"; then
    echo "init_invalid_names: FAIL — '$name' rejected but stderr lacks the grammar hint" >&2
    cat "$TMP/.stderr" >&2
    fail=1
    continue
  fi
done
IFS="$OLD_IFS"

# Positive control: a valid name still works.
rm -rf "$TMP"/* 2>/dev/null || true
( cd "$TMP" && "$ROOT/bin/kai" init my-valid-pkg >"$TMP/.stdout" 2>"$TMP/.stderr" )
if [ ! -f "$TMP/kai.toml" ]; then
  echo "init_invalid_names: FAIL — valid name 'my-valid-pkg' did not produce kai.toml" >&2
  cat "$TMP/.stderr" >&2
  fail=1
fi

if [ "$fail" -ne 0 ]; then
  exit 1
fi

echo "init_invalid_names: OK"
