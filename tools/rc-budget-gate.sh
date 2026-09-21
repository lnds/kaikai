#!/bin/sh
# Reference-count budget: how many incref/decref a shape actually executes.
#
# The leak gate answers "did every allocation come back". This one answers
# "at what cost", and they fail on different defects. A pass that plants a
# redundant dup/drop pair keeps the ledger balanced, so the leak gate, the
# selfhost gate and the corpus all stay green while every run of the shape
# pays for traffic it does not need.
#
# The counters come from the runtime under KAI_TRACE_RC, so they are the
# operations that ran, not the ones the emitter wrote down. KAI_THREADS=1
# keeps the per-thread ledgers from needing a cross-thread reduction.
#
# Each shape below exercises a different owner: a tree that shares nothing,
# a pipe chain over a list, and strings through interpolation.
#
# The backend is pinned: the two emit different counts for the same shape
# (native folds a drop the C path pays), so a floating default would read a
# tree built with KAI_LLVM=1 against numbers taken from the other backend.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
SRC="$ROOT/tools/rc-budget"
BASELINE="$ROOT/tools/rc-budget-baseline.txt"
WORK="${TMPDIR:-/tmp}/rc-budget.$$"
UPDATE="${RC_BUDGET_UPDATE:-0}"

trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK"

measure() {
  KAI_BACKEND="${RC_BUDGET_BACKEND:-c}" "$KAI" build "$SRC/$1.kai" -o "$WORK/$1" >/dev/null 2>&1 \
    || { echo "rc-budget: $1 failed to build" >&2; return 1; }
  KAI_TRACE_RC=1 KAI_THREADS=1 "$WORK/$1" 2>&1 >/dev/null \
    | sed -n 's/.*incref_total=\([0-9]*\) decref_total=\([0-9]*\).*/\1 \2/p' \
    | head -1
}

for f in "$SRC"/*.kai; do
  name="$(basename "$f" .kai)"
  got="$(measure "$name")"
  [ -n "$got" ] || { echo "rc-budget: $name produced no counters" >&2; exit 1; }
  echo "$name $got"
done > "$WORK/measured.txt"

if [ "$UPDATE" = "1" ]; then
  cp "$WORK/measured.txt" "$BASELINE"
  echo "rc-budget: baseline updated"
  cat "$BASELINE"
  exit 0
fi

[ -f "$BASELINE" ] || {
  echo "rc-budget: no baseline; run RC_BUDGET_UPDATE=1 $0" >&2
  exit 1
}

# A shape may get cheaper without ceremony; only growth is a regression.
status=0
while read -r name inc dec; do
  want="$(grep "^$name " "$BASELINE" || true)"
  [ -n "$want" ] || { echo "rc-budget: $name is not in the baseline" >&2; status=1; continue; }
  winc="$(echo "$want" | cut -d' ' -f2)"
  wdec="$(echo "$want" | cut -d' ' -f3)"
  if [ "$inc" -gt "$winc" ] || [ "$dec" -gt "$wdec" ]; then
    echo "rc-budget: $name grew: incref $winc -> $inc, decref $wdec -> $dec" >&2
    status=1
  elif [ "$inc" -lt "$winc" ] || [ "$dec" -lt "$wdec" ]; then
    echo "rc-budget: $name improved: incref $winc -> $inc, decref $wdec -> $dec (lower the baseline)" >&2
    status=1
  fi
done < "$WORK/measured.txt"

[ "$status" -eq 0 ] && echo "rc-budget: OK ($(wc -l < "$WORK/measured.txt" | tr -d ' ') shapes)"
exit "$status"
