#!/bin/sh
# tools/fmt-width-report.sh — line-width metrics over kaikai sources.
#
# The formatter decides line breaks from syntactic shape alone, so a
# construct it considers "inline" is emitted at whatever length it
# happens to reach. This script is the yardstick for that: it measures
# how far a file's lines run past a budget, and — the part that makes
# the number actionable — separates the lines a writer COULD have
# broken from the ones no writer can.
#
#   soft  an over-budget line the writer could have broken: its longest
#         unbreakable atom still leaves room inside the budget.
#   hard  an over-budget line no writer can shorten: indentation plus a
#         single atom (a string literal, an identifier, a dotted path,
#         a comment tail) already exceeds the budget on its own.
#
# `soft` is the number a width model drives to zero. `hard` is a
# property of the source, not of the writer, so it is reported and
# never gated — otherwise a user's long string literal would read as a
# formatter regression.
#
# An ATOM is a run of characters the writer cannot split:
#   * a quoted string, including its interior spaces (a `"a b"` is one
#     atom of width 5, not two of width 2);
#   * a char literal;
#   * a comment, from `#` to end of line (the writer does not reflow
#     comment text);
#   * otherwise, a run of non-space characters.
#
# `soft_max` is the widest soft line — how bad the worst break is, not
# just how many there are. Two runs with the same soft count are not
# equally bad if one peaks at 104 and the other at 240.
#
# Usage:
#   tools/fmt-width-report.sh [--width N] [--totals] FILE...
#
#   --width N   budget in columns (default 100; see docs/fmt-width.md)
#   --totals    emit one machine-readable line instead of the table:
#               `lines=N soft=N hard=N soft_max=N over=N`
#
# Widths are counted in characters, not display cells: a source with
# non-ASCII text in a string literal reads slightly wide here. Every
# consumer in-tree compares two runs of this same script, so the bias
# cancels.

set -eu

width=100
totals=0
files=""

while [ $# -gt 0 ]; do
  case "$1" in
    --width) width="$2"; shift 2 ;;
    --width=*) width="${1#--width=}"; shift ;;
    --totals) totals=1; shift ;;
    --) shift; break ;;
    -*) echo "fmt-width-report: unknown option $1" >&2; exit 2 ;;
    *) files="$files $1"; shift ;;
  esac
done
files="$files $*"

if [ -z "$(echo "$files" | tr -d ' ')" ]; then
  echo "fmt-width-report: no input files" >&2
  exit 2
fi

# shellcheck disable=SC2086
awk -v width="$width" -v totals="$totals" '
# Longest unbreakable atom on the line, and the leading indent.
function scan_atoms(line,   i, n, c, cur, best, mode, q) {
  n = length(line)
  best = 0; cur = 0; mode = "code"
  for (i = 1; i <= n; i++) {
    c = substr(line, i, 1)
    if (mode == "code") {
      if (c == "#") {                       # comment: the rest is one atom
        cur = n - i + 1
        if (cur > best) best = cur
        return best
      }
      if (c == "\"" || c == "'"'"'") { mode = "str"; q = c; cur++; continue }
      if (c == " " || c == "\t") { if (cur > best) best = cur; cur = 0; continue }
      cur++
    } else {
      cur++
      if (c == "\\") { i++; cur++; continue }   # escape: consume the pair
      if (c == q) mode = "code"
    }
  }
  if (cur > best) best = cur
  return best
}
function indent_of(line,   i, n, c) {
  n = length(line)
  for (i = 1; i <= n; i++) {
    c = substr(line, i, 1)
    if (c != " " && c != "\t") return i - 1
  }
  return n
}
FNR == 1 {
  if (FILENAME != prev && prev != "") emit(prev)
  prev = FILENAME
  f_lines = 0; f_soft = 0; f_hard = 0; f_max = 0; in_triple = 0
}
{
  f_lines++; t_lines++
  len = length($0)
  # A line inside a """ block is verbatim user text: unbreakable whole.
  triples = gsub(/"""/, "\"\"\"")
  here_triple = in_triple
  if (triples % 2 == 1) in_triple = !in_triple
  if (len > width) {
    f_over++; t_over++
    atom = here_triple ? len : scan_atoms($0)
    if (indent_of($0) + atom > width) { f_hard++; t_hard++ }
    else {
      f_soft++; t_soft++
      if (len > f_max) f_max = len
      if (len > t_max) t_max = len
    }
  }
}
function emit(name) {
  if (totals) return
  printf "%-52s %6d %6d %6d %8d\n", name, f_lines, f_soft, f_hard, f_max
}
BEGIN {
  if (!totals)
    printf "%-52s %6s %6s %6s %8s\n", "FILE", "LINES", "SOFT", "HARD", "SOFT_MAX"
}
END {
  if (prev != "") emit(prev)
  if (totals) {
    printf "lines=%d soft=%d hard=%d soft_max=%d over=%d\n", t_lines, t_soft, t_hard, t_max, t_over
  } else {
    printf "%-52s %6d %6d %6d %8d\n", "TOTAL (budget " width ")", t_lines, t_soft, t_hard, t_max
  }
}
' $files
