#!/bin/sh
# `#[unstable]` fixtures: an unacknowledged export warns at the call site with
# the lines of main.warn.expected (a fixture without that golden stays
# silent), and a misplaced attribute is rejected with main.err.expected.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

fail=0
for dir in "$ROOT"/examples/unstable/*/; do
  name="$(basename "$dir")"
  rc=0
  (cd "$dir" && "$KAI" build main.kai -o "$tmp/$name" > /dev/null 2> "$tmp/$name.err") || rc=$?
  if [ -f "$dir/main.err.expected" ]; then
    [ "$rc" -ne 0 ] || { echo "unstable-warnings FAIL $name (expected a rejection)"; fail=1; }
    while IFS= read -r line || [ -n "$line" ]; do
      grep -qF -- "$line" "$tmp/$name.err" || { echo "unstable-warnings FAIL $name (missing: $line)"; fail=1; }
    done < "$dir/main.err.expected"
    continue
  fi
  if [ "$rc" -ne 0 ]; then
    echo "unstable-warnings FAIL $name (build failed)"
    cat "$tmp/$name.err"
    fail=1
    continue
  fi
  if [ -f "$dir/main.warn.expected" ]; then
    while IFS= read -r line || [ -n "$line" ]; do
      grep -qF -- "$line" "$tmp/$name.err" || { echo "unstable-warnings FAIL $name (missing: $line)"; fail=1; }
    done < "$dir/main.warn.expected"
  elif grep -q 'is #\[unstable\]' "$tmp/$name.err"; then
    echo "unstable-warnings FAIL $name (unexpected warning)"
    fail=1
  fi
  "$tmp/$name" > "$tmp/$name.out"
  if [ -f "$dir/main.out.expected" ] && ! cmp -s "$dir/main.out.expected" "$tmp/$name.out"; then
    echo "unstable-warnings FAIL $name (stdout differs from main.out.expected)"
    fail=1
  fi
done
[ "$fail" -eq 0 ] && echo "unstable-warnings OK"
exit "$fail"
