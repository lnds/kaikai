#!/bin/sh
# tests/fmt_fixtures.sh — exercise `kai fmt` against examples/fmt/.
#
# Two checks per fixture:
#   1. fmt(input.kai)    == expected.kai   (canonical output)
#   2. fmt(expected.kai) == expected.kai   (idempotency)
# Plus a roundtrip sanity check on all formattable examples — the
# formatted output must re-parse without errors.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"

if [ ! -x "$KAIC2" ]; then
  echo "fmt_fixtures: $KAIC2 not built; run 'make kaic2' first" >&2
  exit 2
fi

fail=0
pass=0
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

for input in "$ROOT"/examples/fmt/*.input.kai; do
  name=$(basename "$input" .input.kai)
  expected="${input%.input.kai}.expected.kai"
  if [ ! -f "$expected" ]; then
    echo "  SKIP $name — missing $expected"
    continue
  fi
  if ! "$KAIC2" --fmt "$input" > "$tmp/out.kai" 2> "$tmp/err"; then
    echo "  FAIL $name — fmt rejected input:"
    sed 's/^/      /' "$tmp/err"
    fail=$((fail + 1))
    continue
  fi
  if ! diff -u "$expected" "$tmp/out.kai" > "$tmp/diff"; then
    echo "  FAIL $name — output != expected:"
    sed 's/^/      /' "$tmp/diff"
    fail=$((fail + 1))
    continue
  fi
  if ! "$KAIC2" --fmt "$expected" > "$tmp/out2.kai" 2> "$tmp/err"; then
    echo "  FAIL $name — fmt rejected expected (idempotency):"
    sed 's/^/      /' "$tmp/err"
    fail=$((fail + 1))
    continue
  fi
  if ! diff -u "$expected" "$tmp/out2.kai" > "$tmp/diff"; then
    echo "  FAIL $name — fmt(expected) != expected (idempotency):"
    sed 's/^/      /' "$tmp/diff"
    fail=$((fail + 1))
    continue
  fi
  echo "  OK   $name"
  pass=$((pass + 1))
done

# Roundtrip on the examples/minimal/ corpus: every file the formatter
# accepts must produce output that parses back without error. Catches
# silent grammar-side regressions like the (1..100) round-trip break.
for f in "$ROOT"/examples/minimal/*.kai \
         "$ROOT"/examples/quickstart/01_hello.kai \
         "$ROOT"/examples/quickstart/02_fizzbuzz.kai \
         "$ROOT"/examples/quickstart/03_calculator.kai \
         "$ROOT"/examples/phase4/*.kai; do
  [ -f "$f" ] || continue
  name=$(basename "$f")
  if ! "$KAIC2" --fmt "$f" > "$tmp/rt.kai" 2> "$tmp/err"; then
    # Unsupported constructs are reported via stderr + exit 1; that is
    # an explicit refusal, not a failure of the formatter.
    if grep -q "kai fmt:" "$tmp/err"; then
      echo "  SKIP $name — unsupported subset (expected)"
      continue
    fi
    echo "  FAIL $name — fmt errored unexpectedly:"
    sed 's/^/      /' "$tmp/err"
    fail=$((fail + 1))
    continue
  fi
  # Re-parse: if the formatter's output cannot be parsed, that is
  # a fatal silent breakage of the round-trip invariant.
  if ! "$KAIC2" --tokens "$tmp/rt.kai" > /dev/null 2> "$tmp/err"; then
    echo "  FAIL $name — formatted output failed to re-parse:"
    sed 's/^/      /' "$tmp/err"
    fail=$((fail + 1))
    continue
  fi
  if ! "$KAIC2" --fmt "$tmp/rt.kai" > "$tmp/rt2.kai" 2> "$tmp/err"; then
    echo "  FAIL $name — fmt(fmt) errored:"
    sed 's/^/      /' "$tmp/err"
    fail=$((fail + 1))
    continue
  fi
  if ! diff -u "$tmp/rt.kai" "$tmp/rt2.kai" > "$tmp/diff"; then
    echo "  FAIL $name — fmt is not idempotent:"
    sed 's/^/      /' "$tmp/diff"
    fail=$((fail + 1))
    continue
  fi
  echo "  OK   $name (roundtrip)"
  pass=$((pass + 1))
done

if [ "$fail" -gt 0 ]; then
  echo "fmt_fixtures: $pass passed, $fail failed"
  exit 1
fi
echo "fmt_fixtures: $pass passed, 0 failed"
