#!/usr/bin/env bash
# The design→matrix class contract, mechanically checked.
#
# docs/namespaces-design.md §2 enumerates every name class the language
# has. This gate holds the explicit mapping from each design class to
# the matrix class slug(s) that cover it, and fails when either side
# drifts: a design class whose name vanished from the doc (the doc was
# rewritten and the mapping is stale), or a design class with zero rows
# in the matrix (a whole class of collisions with no coverage at all).
#
# The mapping is deliberately IN this file, visible and auditable — the
# point is that adding a class to the design forces a decision here,
# instead of coverage silently depending on whoever remembers.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DESIGN="$ROOT/docs/namespaces-design.md"
MATRIX="$ROOT/tests/namespace_matrix.tsv"

# design-class-name (must appear verbatim in the doc) | matrix slug(s)
MAP='
Function|function
Constant|constant
Axiom|axiom
Extern function|extern-fn
Variant constructor|constructor
Record field|field
Effect operation|effect-op
Protocol operation|protocol
Module|module
Habitant introducer|introducer
Kind|kind
Theory|kind
Protocol|protocol
Protocol law set|protocol
Impl|impl
type|type
effect|effect
unit|unit
currency|habitant
perm|habitant
layout|habitant
'

fails=0
while IFS='|' read -r cls slugs; do
  [ -z "$cls" ] && continue
  if ! grep -qF "$cls" "$DESIGN"; then
    echo "  DRIFT  design class '$cls' is in this mapping but not in the design doc"
    fails=$((fails + 1)); continue
  fi
  covered=0
  for slug in $slugs; do
    if awk -F'\t' -v s="$slug" '!/^#/ && $1==s {found=1} END{exit !found}' "$MATRIX"; then
      covered=1
    fi
  done
  if [ "$covered" -eq 0 ]; then
    echo "  GAP    design class '$cls' has NO rows in the matrix (expected slug: $slugs)"
    fails=$((fails + 1))
  fi
done <<EOF
$MAP
EOF

# Matrix classes outside the mapping are finer-grained cells (ladder,
# cache, symbol, ...) — listed so a typo'd slug cannot hide as "extra".
extra=$(awk -F'\t' '!/^#/ && NF>=5 {print $1}' "$MATRIX" | sort -u | while read -r c; do
  echo "$MAP" | grep -q "|.*\b$c\b" || echo "$c"
done | tr '\n' ' ')
echo "namespace_matrix_classes: mapped classes checked; matrix-only classes: $extra"

if [ "$fails" -gt 0 ]; then
  echo "namespace_matrix_classes: $fails class(es) drifted or uncovered"
  exit 1
fi
echo "namespace_matrix_classes OK — every design class has matrix coverage"
