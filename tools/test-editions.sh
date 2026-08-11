#!/bin/sh
# tools/test-editions.sh — edition-selection harness.
#
# Exercises the manifest `edition` surface end to end: per-edition
# behaviour differences, the repo-EDITION fallback when the manifest
# omits the field, and the unknown-edition diagnostic. The compiler's
# selfhost path never reads a manifest edition, so without this
# harness the edition switch has no gate.
#
# Fixture shapes under examples/editions/:
#
#   (a) Positive: <fix>/{kai.toml,main.kai,main.out.expected}.
#       Run the package, diff stdout against the golden.
#
#   (b) Negative: <fix>/main.err.expected. Build must FAIL and every
#       non-empty golden line must appear as a substring of stderr.
#
# edition_cache_invalidation/check.sh is NOT run: it asserts the
# retired KAI_PRELUDE_CACHE_DIR partition layout and fails against
# the current cache. It stays excluded until rewritten against the
# cache layers the driver actually uses.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
ED_DIR="$ROOT/examples/editions"
PASS=0
FAIL=0
FAILED_FIXTURES=""

err() { printf '  FAIL  %s\n' "$1"; FAIL=$((FAIL + 1)); FAILED_FIXTURES="$FAILED_FIXTURES $1"; }
ok()  { printf '  ok    %s\n' "$1"; PASS=$((PASS + 1)); }

run_positive() {
  name="$1"
  dir="$ED_DIR/$name"
  out="$(cd "$dir" && "$KAI" run . 2>&1)" || {
    err "$name"
    echo "$out" | sed 's/^/    /' >&2
    return
  }
  actual="$(echo "$out" | grep -v '^kai:' || true)"
  expected="$(cat "$dir/main.out.expected")"
  if [ "$actual" = "$expected" ]; then
    ok "$name"
  else
    err "$name"
    echo "    --- expected ---" >&2
    echo "$expected" | sed 's/^/    /' >&2
    echo "    --- actual ---" >&2
    echo "$actual" | sed 's/^/    /' >&2
  fi
}

run_negative() {
  name="$1"
  dir="$ED_DIR/$name"
  out="$(cd "$dir" && "$KAI" build . 2>&1)" && {
    err "$name (expected build failure, got success)"
    return
  }
  missing=""
  while IFS= read -r line; do
    [ -n "$line" ] || continue
    echo "$out" | grep -qF "$line" || { missing="$line"; break; }
  done < "$dir/main.err.expected"
  if [ -z "$missing" ]; then
    ok "$name (rejected as expected)"
  else
    err "$name"
    echo "    --- missing diagnostic line ---" >&2
    echo "    $missing" >&2
    echo "    --- actual output ---" >&2
    echo "$out" | sed 's/^/    /' >&2
  fi
}

printf '== edition harness ==\n'

run_positive "edition_hanga_roa_pipe"
run_positive "edition_tongariki_pipe"
run_positive "edition_missing_field"
run_negative "edition_unknown_error"

printf '== summary: %d ok, %d fail ==\n' "$PASS" "$FAIL"
if [ "$FAIL" -gt 0 ]; then
  printf 'failed:%s\n' "$FAILED_FIXTURES" >&2
  exit 1
fi
exit 0
