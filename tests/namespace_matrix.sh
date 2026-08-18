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

# Every axis label the matrix may carry, with the golden each one reads.
# The `test-namespace-collisions*` targets take their fixture lists from
# these labels (tests/namespace_matrix_axes.sh), so a label spelled wrong
# or a golden missing means a fixture silently drops out of execution —
# both are structural errors here. `modular` has no harness behind it
# yet, so a row carrying only that label counts as unexecuted.
axis_golden() {
  case "$1" in
    c|native|asan) echo main.out.expected ;;
    neg|neg-native) echo main.err.expected ;;
    diag) echo DIAG.expected ;;
    testsym) echo main.test.expected ;;
    check) echo main.check.expected ;;
    modular) echo "" ;;
    *) return 1 ;;
  esac
}

rows=0
todo=0
missing=0
na=0
thin=0
fixtures=0
badaxis=0
declare -a CLAIMED=()

while IFS=$'\t' read -r class shape position polarity target axes; do
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

  # The axes column decides where the row's fixtures run; a fixture-bearing
  # row with no executing axis is counted coverage that never runs.
  declare -a axis_list=()
  executed=0
  IFS=',' read -ra axis_list <<< "${axes:-}"
  for a in "${axis_list[@]}"; do
    if ! axis_golden "$a" > /dev/null; then
      echo "  AXIS   $class / $shape / $position — unknown axis label '$a'"
      badaxis=$((badaxis + 1))
    elif [ -n "$(axis_golden "$a")" ]; then
      executed=1
    fi
  done
  if [ "$executed" -eq 0 ]; then
    echo "  AXIS   $class / $shape / $position — no axis with a harness behind it (axes: '${axes:-}')"
    badaxis=$((badaxis + 1))
  fi

  # A cell may name several fixtures, comma-separated: one example per
  # cell proves the combination is represented, not that the
  # representative is enough. Cells whose defect is live carry more.
  n_here=0
  IFS=',' read -ra targets <<< "$target"
  for t in "${targets[@]}"; do
    t="$(echo "$t" | tr -d ' ')"
    [ -z "$t" ] && continue
    if [ ! -d "$CORPUS/$t" ]; then
      echo "  GONE   $class / $shape / $position — claims '$t', which is not in the corpus"
      missing=$((missing + 1))
      continue
    fi
    CLAIMED+=("$t")
    n_here=$((n_here + 1))
    fixtures=$((fixtures + 1))
    for a in "${axis_list[@]}"; do
      golden="$(axis_golden "$a")"
      [ -z "$golden" ] && continue
      [ -f "$CORPUS/$t/$golden" ] \
        || { echo "  AXIS   $class / $shape / $position — '$t' is on axis '$a' but has no $golden"; badaxis=$((badaxis + 1)); }
    done
  done

  # Two fixtures per cell is the default, not an opt-in. One example says
  # the shape reproduces once, which is exactly how a cell comes to look
  # covered while its variants go untested: `module/named-like-type` was
  # green on a fixture declaring both names in one file, while the same
  # shape across two modules failed. The single fixture is always the
  # benign one, because the benign one is what you reach for first.
  #
  # A cell that genuinely admits one arrangement says so in the polarity
  # column as `shallow: <reason>` — the same discipline `N/A: <reason>`
  # already uses. Depth is what you get by default; being exempt is what
  # you have to argue for.
  #
  # The ceiling is not decoration: past five, a cell is being tested by
  # accumulation rather than by design, and the extras belong in their
  # own cells.
  case "$polarity" in
    shallow:*)
      reason="${polarity#shallow:}"
      if [ -z "$(echo "$reason" | tr -d ' ')" ]; then
        echo "  EMPTY  $class / $shape / $position — shallow without a reason"
        thin=$((thin + 1))
      fi
      ;;
    *)
      if [ "$n_here" -lt 2 ]; then
        echo "  THIN   $class / $shape / $position — $n_here fixture(s), needs 2 (or 'shallow: <reason>')"
        thin=$((thin + 1))
      elif [ "$n_here" -gt 5 ]; then
        echo "  FAT    $class / $shape / $position — $n_here fixtures; past 5, split the cell"
        thin=$((thin + 1))
      fi
      ;;
  esac
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
echo "namespace_matrix: $rows rows, $covered covered ($fixtures fixtures), $na not applicable, $todo uncovered, $missing dangling, $unclaimed unclaimed, $badaxis axis errors, $thin thin"

# Structural errors are absolute: a row naming a fixture that is not there,
# a fixture no row claims, a cell with neither fixture nor reason. None of
# these is debt to pay down — they mean the matrix and the corpus disagree
# right now.
if [ "$todo" -gt 0 ] || [ "$missing" -gt 0 ] || [ "$unclaimed" -gt 0 ] || [ "$badaxis" -gt 0 ]; then
  echo "namespace_matrix: the matrix and the corpus disagree — add the fixture, write the reason, claim the orphan, or fix the axes column"
  exit 1
fi

# Thinness is a backlog, so it ratchets instead of blocking: the count may
# not grow, and every cell deepened lowers the ceiling for good. A hard
# failure on the whole backlog would just get the gate switched off.
thin_baseline_file="tests/namespace_matrix_thin_baseline.txt"
thin_baseline=0
[ -f "$thin_baseline_file" ] && thin_baseline=$(cat "$thin_baseline_file")

if [ "$thin" -gt "$thin_baseline" ]; then
  echo "namespace_matrix FAIL — thin cells $thin > baseline $thin_baseline"
  echo "fix path: give the new cell a second fixture exercising a DIFFERENT"
  echo "          arrangement (the homonym in another module, or against"
  echo "          the stdlib), or mark it 'shallow: <reason>'."
  exit 1
fi

if [ "$thin" -lt "$thin_baseline" ]; then
  echo "namespace_matrix OK (improved) — thin $thin < baseline $thin_baseline"
  echo "  suggest: echo $thin > $thin_baseline_file"
fi
