#!/usr/bin/env bash
# vec-layout benchmark: build and sum a Vec[Point] and a Vec[Int], both backends.
#
#   ./run.sh [N]    CPU time (user+sys), median of 15 runs per backend, then the RC ledger
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$REPO_ROOT/benchmarks/vec-layout"
N="${1:-10000000}"
RUNS=15
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

BACKENDS=()
for be in c native; do
  if "$REPO_ROOT/bin/kai" build --backend="$be" "$HERE/vec_layout.kai" -o "$WORK/vec_layout_$be" >"$WORK/$be.err" 2>&1; then
    BACKENDS+=("$be")
  else
    echo "$be column skipped: $(head -1 "$WORK/$be.err")" >&2
  fi
done

cpu() { /usr/bin/time -p env KAI_THREADS=1 "$1" "$N" >/dev/null 2>"$WORK/t" || true; awk '/^(user|sys)/ { s += $2 } END { printf "%.2f\n", s }' "$WORK/t"; }

for be in "${BACKENDS[@]}"; do : >"$WORK/times_$be"; done
for _ in $(seq "$RUNS"); do
  for be in "${BACKENDS[@]}"; do cpu "$WORK/vec_layout_$be" >>"$WORK/times_$be"; done
done
for be in "${BACKENDS[@]}"; do
  echo "$be: $(sort -n "$WORK/times_$be" | sed -n "$(( (RUNS + 1) / 2 ))p") s"
done
for be in "${BACKENDS[@]}"; do
  echo "$be: $(KAI_THREADS=1 KAI_TRACE_RC=1 "$WORK/vec_layout_$be" "$N" 2>&1 >/dev/null | grep -oE 'leaked=-?[0-9]+|vec_inplace=[0-9]+|vec_cow=[0-9]+|incref_total=[0-9]+' | tr '\n' ' ')"
done
