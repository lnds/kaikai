#!/usr/bin/env bash
# Prints the fixtures of tests/namespace_matrix.tsv that carry the given
# execution axis, one per line, in matrix order, deduplicated.
#
# The corpus targets in stage2/Makefile read their fixture lists through
# this script, so a fixture is executed exactly where the matrix says it
# is: no second list to fall behind, no fixture counted but never run.
#
# Usage: namespace_matrix_axes.sh <axis>   (see the matrix header for labels)

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MATRIX="$ROOT/tests/namespace_matrix.tsv"

[ $# -eq 1 ] || { echo "usage: $0 <axis>" >&2; exit 2; }

awk -F'\t' -v axis="$1" '
  /^#/ || NF < 6 || $5 ~ /^(N\/A|TODO)/ { next }
  {
    n = split($6, ax, ",")
    hit = 0
    for (i = 1; i <= n; i++) if (ax[i] == axis) hit = 1
    if (!hit) next
    m = split($5, fx, ",")
    for (j = 1; j <= m; j++) {
      f = fx[j]; gsub(/ /, "", f)
      if (f != "" && !(f in seen)) { seen[f] = 1; print f }
    }
  }' "$MATRIX"
