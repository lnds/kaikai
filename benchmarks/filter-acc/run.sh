#!/usr/bin/env bash
# filter-acc benchmark: a filter with an accumulator, both backends.
#
#   ./run.sh        CPU time (user+sys), median of 15 runs per backend, then the RC ledger
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$REPO_ROOT/benchmarks/filter-acc"
RUNS=15
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

BACKENDS=()
for be in c native; do
  if "$REPO_ROOT/bin/kai" build --backend="$be" "$HERE/filter_acc.kai" -o "$WORK/filter_acc_$be" >"$WORK/$be.err" 2>&1; then
    BACKENDS+=("$be")
  else
    echo "$be column skipped: $(head -1 "$WORK/$be.err")" >&2
  fi
done

cpu() { /usr/bin/time -p env KAI_THREADS=1 "$1" >/dev/null 2>"$WORK/t" || true; awk '/^(user|sys)/ { s += $2 } END { printf "%.2f\n", s }' "$WORK/t"; }

for be in "${BACKENDS[@]}"; do : >"$WORK/times_$be"; done
for _ in $(seq "$RUNS"); do
  for be in "${BACKENDS[@]}"; do cpu "$WORK/filter_acc_$be" >>"$WORK/times_$be"; done
done
for be in "${BACKENDS[@]}"; do
  echo "$be: $(sort -n "$WORK/times_$be" | sed -n "$(( (RUNS + 1) / 2 ))p") s"
done
for be in "${BACKENDS[@]}"; do
  echo "$be: $(KAI_THREADS=1 KAI_TRACE_RC=1 "$WORK/filter_acc_$be" 2>&1 >/dev/null | grep -oE 'leaked=-?[0-9]+|live_peak=[0-9]+' | tr '\n' ' ')"
done
