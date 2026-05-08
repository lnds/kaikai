#!/usr/bin/env bash
# examples/perceus/rb_tree_bench.sh — runs the canonical Perceus
# red-black tree benchmark in three implementations and prints a
# comparison table.
#
# Usage:
#
#   ./examples/perceus/rb_tree_bench.sh [iterations]
#
# Default iterations = 3 (each implementation runs N times; we
# report the median wall-clock).
#
# Output is the table format docs/benchmarks/rb_tree.md cites.
# The benchmark is **not** wired into any tier — too slow for
# tier0/1 and the absolute numbers are machine-dependent. Run
# locally when investigating Perceus performance.

set -euo pipefail

cd "$(dirname "$0")/../.."
KAI_BIN="${KAI_BIN:-bin/kai}"
ITERS="${1:-3}"

KAIKAI_SRC="examples/perceus/rb_tree_bench.kai"
C_SRC="examples/perceus/rb_tree_bench_c.c"
KAI_BUILD="/tmp/rb_tree_bench_kaikai"
C_BUILD="/tmp/rb_tree_bench_c"
CXX_BUILD="/tmp/rb_tree_bench_cxx"

echo "[1/3] building kaikai..."
"$KAI_BIN" build "$KAIKAI_SRC" -o "$KAI_BUILD" >/dev/null

echo "[2/3] building C (hand-written rb-tree, clang -O2)..."
clang -std=c11 -O2 "$C_SRC" -o "$C_BUILD"

echo "[3/3] building C++ (std::set, clang++ -O2)..."
clang++ -std=c++17 -O2 -DBENCH_STDSET "$C_SRC" -o "$CXX_BUILD"

# Run each implementation ITERS times and report the median elapsed
# time (in milliseconds). Median rather than mean since one cold
# warm-up run can skew the average; median is robust to outliers.
median_elapsed_ms() {
  local bin="$1"
  local samples=()
  local i
  for i in $(seq 1 "$ITERS"); do
    local out
    out=$("$bin" 2>/dev/null | grep '^elapsed:' | head -1)
    # Format: "elapsed: 2.873s" → 2873
    local secs ms
    secs=$(echo "$out" | sed -E 's/elapsed: ([0-9]+)\..*/\1/')
    ms=$(echo "$out"   | sed -E 's/elapsed: [0-9]+\.([0-9]+)s/\1/')
    samples+=("$((10#$secs * 1000 + 10#$ms))")
  done
  printf '%s\n' "${samples[@]}" | sort -n | awk -v n="$ITERS" 'NR==int((n+1)/2)'
}

# RSS in MB via /usr/bin/time -l on macOS / -v on Linux. We grep
# both formats and divide by 1024^2 (macOS reports bytes; Linux
# reports kilobytes — handle both).
peak_rss_mb() {
  local bin="$1"
  local out
  out=$(/usr/bin/time -l "$bin" 2>&1 1>/dev/null || /usr/bin/time -v "$bin" 2>&1 1>/dev/null)
  local raw
  raw=$(echo "$out" | grep -E 'maximum resident' | sed -E 's/[^0-9]*([0-9]+).*/\1/')
  if [ -z "$raw" ]; then echo "?"; return; fi
  local platform
  platform=$(uname)
  if [ "$platform" = "Darwin" ]; then
    echo $((raw / 1048576))
  else
    echo $((raw / 1024))
  fi
}

echo
echo "running each implementation ${ITERS}× and reporting median..."
KAI_MS=$(median_elapsed_ms "$KAI_BUILD")
C_MS=$(median_elapsed_ms   "$C_BUILD")
CXX_MS=$(median_elapsed_ms "$CXX_BUILD")
KAI_RSS=$(peak_rss_mb "$KAI_BUILD")
C_RSS=$(peak_rss_mb   "$C_BUILD")
CXX_RSS=$(peak_rss_mb "$CXX_BUILD")

# Ratios with two decimals via awk; bash arithmetic only does ints.
ratio() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%.2fx", a / b }'; }

cat <<EOF

Workload: 1,000,000 random insert into red-black tree (LCG-seeded keys)
Hardware: $(uname -smr)

Implementation         Median elapsed    RSS         Notes
---------------------- ----------------- ----------- ----------------------------------
kaikai (this)          $(printf '%6d ms' "$KAI_MS")        $(printf '%4d MB' "$KAI_RSS")     Functional Okasaki RB; reuse-in-place fires (issue #210)
C (hand-written RB)    $(printf '%6d ms' "$C_MS")        $(printf '%4d MB' "$C_RSS")     Intrusive RB with parent pointers, clang -O2
C++ std::set<int64_t>  $(printf '%6d ms' "$CXX_MS")        $(printf '%4d MB' "$CXX_RSS")     libc++ RB tree, clang++ -O2

Ratios:
  kaikai / C    = $(ratio "$KAI_MS" "$C_MS")  time, $(ratio "$KAI_RSS" "$C_RSS") RSS
  kaikai / C++  = $(ratio "$KAI_MS" "$CXX_MS")  time, $(ratio "$KAI_RSS" "$CXX_RSS") RSS

Anga Roa DoD #4 target: kaikai/C ≤ 1.7× (stretch) or 2.0× (relaxed).
EOF
