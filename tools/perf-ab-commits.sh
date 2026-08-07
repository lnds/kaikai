#!/bin/bash
# A/B a benchmark across two commits, alternating arms serially so machine
# drift hits both equally.
#
# The kaikai/C ratio from a single run is NOT enough to judge a regression:
# it absorbs machine load only partly, because kaikai and the reference C do
# not degrade at the same rate. The same commit measured under load and on an
# idle machine yields materially different ratios, so a ratio from one run
# must never be compared against a historical number taken under unknown
# load. Only two commits measured in the same session are comparable.
#
# Each arm builds with its OWN bin/kai: kaic2 bakes its stdlib path at build
# time, so a binary copied between checkouts resolves the wrong stdlib.
#
# Usage: tools/perf-ab-commits.sh <baseline-ref> [iters]
#
#   tools/perf-ab-commits.sh fce451f4 7
#
# Run it on an idle machine — check `uptime` first.
set -u

HEAD_WT="$(cd "$(dirname "$0")/.." && pwd)"
BASE_REF="${1:?usage: $0 <baseline-ref> [iters]}"
ITERS="${2:-7}"
BASE_WT="${BASE_WT:-/tmp/perf-ab-baseline-wt}"
SRC="${SRC:-examples/perceus/rb_tree_bench.kai}"
C_SRC="${C_SRC:-examples/perceus/rb_tree_bench_c.c}"

if [ ! -x "$BASE_WT/bin/kai" ]; then
  echo "building baseline $BASE_REF in $BASE_WT (this takes a while)..."
  rm -rf "$BASE_WT"
  git -C "$HEAD_WT" worktree add -q "$BASE_WT" "$BASE_REF" || exit 1
  (cd "$BASE_WT" && make KAI_LLVM=1 kaic2 >/dev/null 2>&1) || {
    echo "baseline build failed"; exit 1; }
fi

build() {  # $1=worktree $2=out
  (cd "$1" && ./bin/kai build --backend=native "$SRC" -o "$2" >/dev/null 2>&1)
}
build "$HEAD_WT" /tmp/ab_head || { echo "head build failed"; exit 1; }
build "$BASE_WT" /tmp/ab_base || { echo "base build failed"; exit 1; }
cc -std=c99 -O2 -o /tmp/ab_c "$HEAD_WT/$C_SRC" -lm 2>/dev/null

ms() { local s=$(date +%s%N); "$1" >/dev/null 2>&1; echo $((($(date +%s%N)-s)/1000000)); }

echo "iter  head_ms  base_ms  c_ms"
h_all=(); b_all=(); c_all=()
for i in $(seq 1 "$ITERS"); do
  # alternate order each iteration so neither arm systematically runs on a
  # warmer machine
  if [ $((i % 2)) -eq 1 ]; then
    h=$(ms /tmp/ab_head); b=$(ms /tmp/ab_base)
  else
    b=$(ms /tmp/ab_base); h=$(ms /tmp/ab_head)
  fi
  c=$(ms /tmp/ab_c)
  h_all+=("$h"); b_all+=("$b"); c_all+=("$c")
  printf "%4d  %7d  %7d  %5d\n" "$i" "$h" "$b" "$c"
done

med() { printf '%s\n' "$@" | sort -n | awk '{a[NR]=$1} END{print (NR%2)?a[(NR+1)/2]:int((a[NR/2]+a[NR/2+1])/2)}'; }
H=$(med "${h_all[@]}"); B=$(med "${b_all[@]}"); C=$(med "${c_all[@]}")
echo
echo "medians: head=${H}ms base=${B}ms c=${C}ms"
awk -v h="$H" -v b="$B" -v c="$C" 'BEGIN{
  printf "ratio head/C = %.2fx\n", h/c
  printf "ratio base/C = %.2fx\n", b/c
  printf "head vs base = %+.1f%% (negative = head faster)\n", (h-b)*100.0/b
}'
