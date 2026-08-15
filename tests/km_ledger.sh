#!/usr/bin/env bash
# Structural debt ledger for the name resolution refactor.
#
# Two numbers the corpus does not capture:
#
#   1. `km score` on the compiler and on the files the refactor touches.
#      Threading a field through a monolith does not improve it, so a
#      lane that only threads leaves this flat — which is the point of
#      recording it. A lane is expected to leave the score no worse, and
#      to improve it where it can split what it is already touching.
#
#   2. Corrective scope passes still alive. The design removes them by
#      keying tables at construction instead of filtering afterwards, so
#      their count falling to zero is the structural goal.
#
# Run before and after a lane; the delta is what the PR reports.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

score_of() {
  km score "$1" 2>/dev/null | grep "Project Score:" | sed 's/.*Score: *//' || echo "n/a"
}
loc_of() {
  km score "$1" 2>/dev/null | grep "Total LOC:" | sed 's/.*: *//' || echo "n/a"
}

echo "km_ledger — structural debt, $(git rev-parse --short HEAD)"
echo
printf '%-24s %-14s %s\n' "TARGET" "SCORE" "LOC"
printf '%-24s %-14s %s\n' "compiler (all)" "$(score_of stage2/compiler)" "$(loc_of stage2/compiler)"

# The files the refactor threads through. A new one joins this list when
# a step starts touching it.
for f in ast.kai surface_lower.kai modules.kai emit_shared.kai infer.kai \
         perceus.kai emit_c.kai protos.kai; do
  [ -f "stage2/compiler/$f" ] || continue
  printf '%-24s %-14s %s\n' "$f" "$(score_of "stage2/compiler/$f")" "$(loc_of "stage2/compiler/$f")"
done

echo
passes=0
for p in ta_scope unit_scope proto_scope const_scope; do
  if [ -f "stage2/compiler/$p.kai" ]; then
    passes=$((passes + 1))
    printf '  corrective pass alive: %s\n' "$p.kai"
  fi
done
echo "corrective scope passes: $passes (target 0 — tables keyed at construction)"
