#!/bin/bash
# Position-enumeration audit for the ownership walkers.
#
# stage2/compiler/expr_positions.kai is the single description of
# ExprKind's sub-expression positions; ownership walkers descend through
# it instead of enumerating the variants themselves. This audit finds
# functions in the perceus-owned files that still pattern-match the
# pure-traversal canary variants (EMapPipe / EFlatMapPipe / EFilterPipe
# — no walker ever treats those specially, so matching one means the
# function replicates the position enumeration) and compares the set
# against the committed baseline.
#
# - A function NOT in the baseline fails the audit: new walkers must
#   consult xp_positions, not enumerate.
# - A baseline entry whose function no longer enumerates also fails:
#   the baseline is a ratchet — shrink it in the same commit that
#   migrates the walker.
set -u
cd "$(dirname "$0")/.."

BASELINE=tools/perceus-position-baseline.txt
FILES="stage2/compiler/perceus.kai stage2/compiler/perceus_tail_drop.kai \
stage2/compiler/perceus_plant_drop.kai stage2/compiler/perceus_op_arg.kai \
stage2/compiler/emit_tcrec_live.kai"

observed=$(mktemp)
for f in $FILES; do
  [ -f "$f" ] || continue
  grep -n 'EMapPipe(\|EFlatMapPipe(\|EFilterPipe(' "$f" | cut -d: -f1 | while read -r ln; do
    awk -v t="$ln" 'NR<=t && /^(pub )?fn /{n=$2; sub(/\(.*/, "", n)} NR==t{print n; exit}' "$f"
  done | sort -u | sed "s|^|$(basename "$f") |"
done | sort > "$observed"

status=0
new=$(comm -13 <(sort "$BASELINE") "$observed")
gone=$(comm -23 <(sort "$BASELINE") "$observed")
if [ -n "$new" ]; then
  echo "perceus-position-audit FAIL — new ExprKind enumeration site(s):"
  echo "$new" | sed 's/^/  /'
  echo "Walkers must descend through xp_positions (stage2/compiler/expr_positions.kai)."
  status=1
fi
if [ -n "$gone" ]; then
  echo "perceus-position-audit FAIL — stale baseline entry(ies), shrink $BASELINE:"
  echo "$gone" | sed 's/^/  /'
  status=1
fi
rm -f "$observed"
[ $status -eq 0 ] && echo "perceus-position-audit OK ($(wc -l < "$BASELINE" | tr -d ' ') baselined walkers)"
exit $status
