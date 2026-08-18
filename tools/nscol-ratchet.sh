#!/usr/bin/env bash
# Failure ratchet for one axis of the namespace-collision corpus.
#
# The corpus carries fixtures whose defect is still live, so a plain
# exit-code gate would never be green and would get switched off. Instead
# each axis has an allowed failure count in tools/nscol-baseline.txt
# (`<axis>:<count>`): more failures than the baseline is a regression and
# fails naming the fixtures; fewer is progress and the baseline should
# come down so it cannot bounce back.
#
# Usage: nscol-ratchet.sh <axis> <log>
#   <log> is the captured output of the axis target; every fixture leaves
#   one `namespace-collisions[-axis] OK|FAIL|DIFF <fixture>` line in it.
#
# Writes stage2/build/nscol-<axis>.status (`<axis> <failing> <baseline>`)
# for tests/namespace_matrix.sh, which refuses to pass over a red axis.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BASELINE_FILE="$ROOT/tools/nscol-baseline.txt"

[ $# -eq 2 ] || { echo "usage: $0 <axis> <log>" >&2; exit 2; }
axis="$1"
log="$2"
name="namespace-collisions-$axis"

[ -f "$log" ] || { echo "$name FAIL — no log at $log"; exit 1; }

verdicts="$(grep -E '^namespace-collisions(-[a-z-]+)? (OK|FAIL|DIFF) ' "$log" || true)"
ran=$(echo "$verdicts" | awk 'NF { print $3 }' | sort -u | grep -c . || true)
failing="$(echo "$verdicts" | awk '$2 != "OK" { print $3 }' | sort -u)"
fail=$(echo "$failing" | grep -c . || true)

baseline=$(grep -E "^$axis:" "$BASELINE_FILE" 2>/dev/null | head -1 | cut -d: -f2 | tr -d ' ')
baseline="${baseline:-0}"

status_dir="$(dirname "$log")"
printf '%s %s %s\n' "$axis" "$fail" "$baseline" > "$status_dir/nscol-$axis.status"
printf '%s\n' "$failing" | grep . > "$status_dir/nscol-$axis.fail" || : > "$status_dir/nscol-$axis.fail"

if [ "$ran" -eq 0 ]; then
  echo "$name FAIL — no fixture ran (axis list empty or harness broken); see $log"
  exit 1
fi

if [ "$fail" -gt "$baseline" ]; then
  echo "$name FAIL — $fail of $ran fixtures failing, baseline $baseline; failing:"
  echo "$failing" | sed 's/^/  /'
  echo "--- $log ---"
  cat "$log"
  echo "fix path: the fixture(s) above regressed (or a new red fixture landed"
  echo "          without raising the baseline on purpose); a deliberate new"
  echo "          red-by-design row raises $axis in tools/nscol-baseline.txt"
  exit 1
fi

if [ "$fail" -lt "$baseline" ]; then
  echo "$name OK (improved) — $fail of $ran failing < baseline $baseline; detail: $log"
  echo "  suggest: set $axis:$fail in tools/nscol-baseline.txt"
else
  echo "$name OK — $fail of $ran failing at baseline $baseline; detail: $log"
fi
