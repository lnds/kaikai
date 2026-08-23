#!/bin/sh
# tests/fmt_ledger.sh — the formatter's quality ledger.
#
# Two axes, one report. A rewrite of the writer is only an improvement
# if BOTH move in the right direction, and each hides the other's
# regressions when read alone:
#
#   OUTPUT   what `kai fmt` does to real code — over-budget lines it
#            could have broken (`soft`), and how far the worst one
#            runs. Measured over the width corpus (the fixtures written
#            for it) and over stdlib/ + examples/ (code nobody wrote to
#            make the formatter look good).
#
#   CODE     what the writer costs to read — `km score` and LOC per
#            formatter module. A document model that fixes the output
#            by growing a 1,500-line module into a 3,000-line one has
#            not paid for itself.
#
# A REPORT, not a gate: run it before and after a lane and put the
# delta in the PR. The gate is tests/fmt_width.sh, which ratchets the
# width-corpus numbers; this ledger is the wider view around it.
#
# `km` (github.com/lnds/code-metrics) is optional — the CODE section
# reports `n/a` without it, and the OUTPUT section still runs.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
KAIC2="$ROOT/stage2/kaic2"

# A flagless kaic2 runs the oldest edition; fmt reproduces the surface
# of the edition the repo declares.
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
REPORT="$ROOT/tools/fmt-width-report.sh"
WIDTH="${FMT_WIDTH:-$(sed -n 's/^budget *//p' tools/fmt-width-baseline.txt | head -1)}"

if [ ! -x "$KAIC2" ]; then
  echo "fmt_ledger: $KAIC2 not built; run 'make kaic2' first" >&2
  exit 2
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

echo "fmt_ledger — $(git rev-parse --short HEAD), budget $WIDTH columns"
echo
echo "OUTPUT — what kai fmt emits"
printf '%-22s %8s %8s %10s %8s\n' "CORPUS" "FILES" "SOFT" "SOFT_MAX" "HARD"

# $1 = label, rest = source files. Formats each into a flat scratch dir
# and reports the totals over the results. A file fmt refuses is
# counted and skipped — a refusal is not a width measurement.
measure() {
  label="$1"; shift
  out="$tmp/$label"; rm -rf "$out"; mkdir -p "$out"
  n=0; refused=0
  for f in "$@"; do
    [ -f "$f" ] || continue
    n=$((n + 1))
    if ! "$KAIC2" $EDITION_FLAG --path "$ROOT/stdlib" --fmt "$f" > "$out/$n.kai" 2>/dev/null; then
      refused=$((refused + 1))
      rm -f "$out/$n.kai"
    fi
  done
  totals=$(cd "$out" && sh "$REPORT" --width "$WIDTH" --totals ./*.kai)
  soft=$(echo "$totals" | sed -n 's/.*soft=\([0-9]*\).*/\1/p')
  hard=$(echo "$totals" | sed -n 's/.*hard=\([0-9]*\).*/\1/p')
  smax=$(echo "$totals" | sed -n 's/.*soft_max=\([0-9]*\).*/\1/p')
  printf '%-22s %8d %8d %10d %8d\n' "$label" "$((n - refused))" "$soft" "$smax" "$hard"
}

# stdlib and the compiler are the selfhost corpus: code written by
# hand, read every day, and formatted in place by `kai fmt`. They are
# where a width regression is actually felt, and they are small enough
# (~220 files) to keep the ledger a few seconds rather than the full
# ~2,000-file examples tree.
measure width-corpus examples/fmt/width/*.input.kai
measure stdlib $(find stdlib -name '*.kai' | sort)
measure compiler $(find stage2/compiler -name '*.kai' | sort)

echo
echo "CODE — what the writer costs to read"
printf '%-24s %-8s %8s %8s %8s\n' "MODULE" "SCORE" "LOC" "COG_AVG" "COG_MAX"

score_of() { km score "$1" 2>/dev/null | sed -n 's/.*Project Score: *//p' | head -1; }
loc_of()   { km score "$1" 2>/dev/null | sed -n 's/.*Total LOC: *//p' | head -1; }
# km cogcom prints `<file> <functions> <avg> <max> <total> <level>`;
# the level is two words on some rows, so index from the left.
cog_of()   { km cogcom "$1" 2>/dev/null | awk -v c="$2" '$1 ~ /\.kai$/ { print $(2 + c) }' | head -1; }

if command -v km >/dev/null 2>&1; then
  for f in stage2/compiler/fmt*.kai; do
    printf '%-24s %-8s %8s %8s %8s\n' "$(basename "$f")" \
      "$(score_of "$f")" "$(loc_of "$f")" "$(cog_of "$f" 1)" "$(cog_of "$f" 2)"
  done
else
  echo "  km not on PATH — install github.com/lnds/code-metrics for the CODE axis"
fi
