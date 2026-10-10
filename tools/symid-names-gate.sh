#!/bin/sh
# Names stay out of identity.
#
# After name resolution a pass decides which declaration, type, effect,
# constructor or operator something is by its id or its constructor, never
# by comparing text. Four checks hold that:
#
#   1. no type definition holds a String beside an id slot;
#   2. the characters of a written name leave it only in the listed files;
#   3. every read of a name out of identity-bearing data, and every string
#      literal a comparison decides on, is listed with its reason;
#   4. no `==` or `!=` compares a value that holds a written name, in the
#      compiler as monomorphised (`--check-written-eq`).
#
# The ledgers live in tools/symid-census/. A new legitimate name read
# (surface text, a diagnostic, a spelled C symbol) is added there with its
# reason; anything else is converted to an id.
#
# Usage: symid-names-gate.sh [--final]
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CENSUS="$ROOT/tools/symid-census"

python3 "$CENSUS/gate.py" "$ROOT/stage2/compiler" "$@"

found=$("$ROOT/stage2/kaic2" --edition "$(cat "$ROOT/EDITION")" --check-written-eq "$ROOT/stage2/main.kai" 2>&1 >/dev/null \
          | grep '^written-eq ' | awk '{ print $2 }' | sort -u || true)
extra=$(printf '%s\n' "$found" | grep . | grep -vxF -f "$CENSUS/written-eq.txt" || true)
if [ -n "$extra" ]; then
  echo "symid-names FAIL — a written name is compared in:" >&2
  printf '%s\n' "$extra" | sed 's/^/  /' >&2
  exit 1
fi
echo "symid-names OK"
