#!/usr/bin/env bash
# Verifies the namespace collision coverage matrix.
#
# Coverage stops being a judgement call: every cell of
# tests/namespace_matrix.tsv either names a fixture that exists, or
# carries a written reason why the cell cannot be expressed. A cell with
# neither fails this gate.
#
# It also runs the check in the other direction — a fixture on disk that
# no row claims is reported, so the matrix cannot fall behind the corpus.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MATRIX="$ROOT/tests/namespace_matrix.tsv"
CORPUS="$ROOT/examples/namespace-collisions"

rows=0
todo=0
missing=0
na=0
declare -a CLAIMED=()

while IFS=$'\t' read -r class shape position polarity target; do
  case "$class" in ''|'#'*) continue ;; esac
  rows=$((rows + 1))

  case "$target" in
    TODO)
      echo "  TODO   $class / $shape / $position / $polarity — no fixture and no reason"
      todo=$((todo + 1))
      continue
      ;;
    N/A:*)
      reason="${target#N/A:}"
      if [ -z "$(echo "$reason" | tr -d ' ')" ]; then
        echo "  EMPTY  $class / $shape / $position — N/A without a reason"
        todo=$((todo + 1))
      fi
      na=$((na + 1))
      continue
      ;;
  esac

  if [ ! -d "$CORPUS/$target" ]; then
    echo "  GONE   $class / $shape / $position — claims '$target', which is not in the corpus"
    missing=$((missing + 1))
    continue
  fi
  CLAIMED+=("$target")
done < "$MATRIX"

# Reverse direction: fixtures nothing claims.
unclaimed=0
for d in "$CORPUS"/*/; do
  name="$(basename "$d")"
  found=0
  for c in "${CLAIMED[@]}"; do
    [ "$c" = "$name" ] && { found=1; break; }
  done
  if [ "$found" -eq 0 ]; then
    echo "  ORPHAN $name — a fixture no matrix row claims"
    unclaimed=$((unclaimed + 1))
  fi
done

covered=$((rows - todo - na))
echo "namespace_matrix: $rows rows, $covered covered, $na not applicable, $todo uncovered, $missing dangling, $unclaimed unclaimed"

if [ "$todo" -gt 0 ] || [ "$missing" -gt 0 ] || [ "$unclaimed" -gt 0 ]; then
  echo "namespace_matrix: the matrix and the corpus disagree — add the fixture, write the reason, or claim the orphan"
  exit 1
fi
