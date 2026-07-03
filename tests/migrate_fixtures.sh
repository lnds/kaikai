#!/bin/sh
# tests/migrate_fixtures.sh — exercise `kai migrate` against
# examples/migrate/.
#
# Per fixture:
#   1. migrate(input.kai)    == expected.kai   (the rewrite)
#   2. migrate(expected.kai) == expected.kai   (idempotency)
#   3. the output re-parses                    (never emits non-parsing)
# A fixture with a <name>.report.expected file also asserts the
# stderr `manual:` report matches, proving un-migratable changes are
# reported, not silently dropped.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"

if [ ! -x "$KAIC2" ]; then
  echo "migrate_fixtures: $KAIC2 not built; run 'make kaic2' first" >&2
  exit 2
fi

fail=0
pass=0
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

for input in "$ROOT"/examples/migrate/*.input.kai; do
  name=$(basename "$input" .input.kai)
  expected="${input%.input.kai}.expected.kai"
  report="${input%.input.kai}.report.expected"
  if [ ! -f "$expected" ]; then
    echo "  SKIP $name — missing $expected"
    continue
  fi

  # 1. rewrite matches the golden
  if ! "$KAIC2" --migrate "$input" > "$tmp/out.kai" 2> "$tmp/err"; then
    echo "  FAIL $name — migrate rejected input:"
    sed 's/^/      /' "$tmp/err"
    fail=$((fail + 1)); continue
  fi
  if ! diff -u "$expected" "$tmp/out.kai" > "$tmp/diff"; then
    echo "  FAIL $name — output != expected:"
    sed 's/^/      /' "$tmp/diff"
    fail=$((fail + 1)); continue
  fi

  # 2. idempotency: migrate(expected) == expected
  if ! "$KAIC2" --migrate "$expected" > "$tmp/out2.kai" 2> "$tmp/err"; then
    echo "  FAIL $name — migrate rejected expected (idempotency):"
    sed 's/^/      /' "$tmp/err"
    fail=$((fail + 1)); continue
  fi
  if ! diff -u "$expected" "$tmp/out2.kai" > "$tmp/diff"; then
    echo "  FAIL $name — migrate(expected) != expected (idempotency):"
    sed 's/^/      /' "$tmp/diff"
    fail=$((fail + 1)); continue
  fi

  # 3. the migrated output re-parses (never emits non-parsing source)
  if ! "$KAIC2" --tokens "$tmp/out.kai" > /dev/null 2> "$tmp/err"; then
    echo "  FAIL $name — migrated output failed to re-parse:"
    sed 's/^/      /' "$tmp/err"
    fail=$((fail + 1)); continue
  fi

  # 4. optional: the `manual:` report matches the golden
  if [ -f "$report" ]; then
    "$KAIC2" --migrate "$input" > /dev/null 2> "$tmp/rep"
    if ! diff -u "$report" "$tmp/rep" > "$tmp/diff"; then
      echo "  FAIL $name — manual report != expected:"
      sed 's/^/      /' "$tmp/diff"
      fail=$((fail + 1)); continue
    fi
  fi

  echo "  OK   $name"
  pass=$((pass + 1))
done

if [ "$fail" -gt 0 ]; then
  echo "migrate_fixtures: $pass passed, $fail failed"
  exit 1
fi
echo "migrate_fixtures: $pass passed, 0 failed"
