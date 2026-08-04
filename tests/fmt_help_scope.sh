#!/bin/sh
# tests/fmt_help_scope.sh — keep `kai fmt --help` honest about scope.
#
# The help text once advertised a "Tongariki MVP scope" that refused
# effects, handlers, protocols, impls, units, dim types, refinements,
# axioms, generic parameters, var and parametric effect rows. The
# formatter grew past that envelope (#670 filled every refusal arm,
# #786 ratcheted it in) and the help was never updated, so it promised
# a guard that does not exist.
#
# Two assertions, one per direction of the drift:
#
#   1. `examples/fmt/help_scope.input.kai` — one construct per name in
#      that list — formats and is idempotent. If a refusal comes back,
#      the help's claim becomes true and this fails.
#   2. The help text does not re-acquire refusal language. If someone
#      restores the stale paragraph without restoring the guard, this
#      fails.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
FIXTURE="$ROOT/examples/fmt/help_scope.input.kai"

if [ ! -x "$KAIC2" ]; then
  echo "fmt_help_scope: $KAIC2 not built; run 'make kaic2' first" >&2
  exit 2
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

fail=0

# 1. Every construct the help claims to format actually formats.
if ! "$KAIC2" --fmt "$FIXTURE" > "$tmp/p1" 2> "$tmp/err"; then
  echo "  FAIL help_scope fixture — fmt refused a construct the help claims to format:"
  sed 's/^/      /' "$tmp/err"
  fail=$((fail + 1))
else
  if ! "$KAIC2" --fmt "$tmp/p1" > "$tmp/p2" 2> "$tmp/err"; then
    echo "  FAIL help_scope fixture — fmt refused on second pass:"
    sed 's/^/      /' "$tmp/err"
    fail=$((fail + 1))
  elif ! diff -u "$tmp/p1" "$tmp/p2" > "$tmp/diff"; then
    echo "  FAIL help_scope fixture — fmt is not idempotent:"
    sed 's/^/      /' "$tmp/diff"
    fail=$((fail + 1))
  fi
fi

# 2. The help text does not promise a refusal the formatter never makes.
help="$("$ROOT/bin/kai" fmt --help 2>&1 || true)"
for phrase in 'refused with an explicit error' 'Tongariki MVP scope'; do
  if printf '%s' "$help" | grep -qF "$phrase"; then
    echo "  FAIL kai fmt --help re-acquired stale refusal language: \"$phrase\""
    echo "      The formatter refuses only unparseable files; see examples/fmt/help_scope.input.kai."
    fail=$((fail + 1))
  fi
done

if [ "$fail" -gt 0 ]; then
  echo "fmt_help_scope: $fail failed"
  exit 1
fi
echo "fmt_help_scope: 2 passed, 0 failed"
