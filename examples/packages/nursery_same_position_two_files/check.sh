#!/bin/sh
# Two modules hold a nursery at one line:col, and only one mixes nurseries.
# That module alone is rejected, whichever of the two comes first.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
KAI="$ROOT/bin/kai"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

fail() { echo "nursery_same_position_two_files: FAIL — $1" >&2; exit 1; }

check() {
  order="$1"; bad="$2"; sound="$3"
  cp -R "$DIR/$order" "$TMP/$order"
  if out="$(cd "$TMP/$order" && "$KAI" build main.kai -o "$TMP/bin" 2>&1)"; then
    fail "$order builds"
  fi
  echo "$out" | grep -qF "brand mismatch on \`await\`" || fail "$order: no mismatch reported"
  echo "$out" | grep -F -- "-->" | grep -qF "$bad" || fail "$order: $bad not flagged"
  if echo "$out" | grep -F -- "-->" | grep -qF "$sound"; then
    fail "$order: flagged the sound module $sound"
  fi
}

check bad_first ma.kai mb.kai
check bad_second mb.kai ma.kai
