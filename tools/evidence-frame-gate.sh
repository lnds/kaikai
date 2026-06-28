#!/bin/sh
# KAI_EVIDENCE_FRAME_ONLY gate (#820) — grep-oracle for the hybrid dispatch model.
#
# A user / value-transportable effect's named instance must resolve through its
# capability slot (`kai_<recv>` for a §6 named instance, `__evf[]` for a frame
# slot), NEVER through the by-name walk (`kai_evidence_lookup_node`). The walk is
# retained only for fiber-local builtins (Cancel/Link/Monitor/Spawn/Actor) and
# Ffi, and `lookup_or_default` only for default-bearing builtins in a frameless
# context (main, clause roots). This oracle compiles the named-instance fixtures
# and asserts the user effect they declare never reaches the walk.
#
# Binary: emitted C for a §6 named-instance fixture contains zero
# `kai_evidence_lookup_node("<UserEff>")` and zero by-id lookup. A regression
# (a user effect falling back to the walk) flips this immediately.

set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
KAIC2="$ROOT/stage2/kaic2"
STDLIB="$ROOT/stdlib"
FAIL=0

# Each entry: <fixture> <user-effect-that-must-not-walk>
check() {
  fixture="$1"
  eff="$2"
  name=$(basename "$fixture" .kai)
  c=$("$KAIC2" --path "$STDLIB" "$fixture" 2>/dev/null) || {
    echo "GATE FAIL $name: kaic2 errored"; FAIL=1; return
  }
  walk=$(printf '%s' "$c" | grep -c "kai_evidence_lookup_node(\"$eff\")" || true)
  byid=$(printf '%s' "$c" | grep -c "kai_evidence_lookup_node_by_id" || true)
  if [ "$walk" -ne 0 ]; then
    echo "GATE FAIL $name: user effect '$eff' resolved by the by-name walk ($walk site(s)) — must use its capability slot"
    FAIL=1
  elif [ "$byid" -ne 0 ]; then
    echo "GATE FAIL $name: a by-id lookup leaked into a named-instance fixture ($byid) — user effects pass capabilities by slot"
    FAIL=1
  else
    echo "gate OK $name ('$eff' resolves by capability slot, no walk)"
  fi
}

check "$ROOT/examples/effects/two_instances_through_call.kai" "Cell"

if [ "$FAIL" -ne 0 ]; then
  echo "KAI_EVIDENCE_FRAME_ONLY gate: FAILED"
  exit 1
fi
echo "KAI_EVIDENCE_FRAME_ONLY gate: PASS"
