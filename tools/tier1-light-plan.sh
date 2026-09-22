#!/bin/sh
# tools/tier1-light-plan.sh — split the tier1 light pool across CI shards
# by measured cost.
#
#   tier1-light-plan.sh <shard> <shards> <target>...   print slice <shard>
#   tier1-light-plan.sh --check <shards> <target>...   assert the slices
#                                                      partition the pool
#
# Costs come from tools/tier1-light-costs.txt. The assignment is greedy
# longest-processing-time: targets in descending cost each go to the slice
# whose projected wall-clock is lowest after taking it. A slice's
# wall-clock is modelled as its fixed base plus the larger of
#   total cost / parallelism                   (throughput-bound)
#   longest + contention * (total - longest)   (bound by its serial pole)
# because a slice runs under `make -j` yet no target finishes faster than
# its own serial loop. Each slice prints longest first, so `make -j` starts
# its long poles before the short targets.
#
# Every shard computes the whole plan and keeps its own slice, so the
# slices partition the pool by construction; --check re-derives all of
# them and fails if a target is lost, duplicated, or listed twice.

set -eu

COSTS="$(dirname "$0")/tier1-light-costs.txt"

usage() {
  echo "usage: $0 <shard> <shards> <target>... | --check <shards> <target>..." >&2
  exit 2
}

[ $# -ge 2 ] || usage
mode=slice
if [ "$1" = "--check" ]; then
  mode=check
  shift
  shard=0
else
  shard=$1
  shift
fi
shards=$1
shift

echo "$*" | awk -v mode="$mode" -v want="$shard" -v n="$shards" '
  function cost(t) { return (t in c) ? c[t] : dflt }
  function wall(k, add,   big, tot, pole) {
    big = (add > mx[k]) ? add : mx[k]
    tot = sum[k] + add
    pole = big + (tot - big) * slow
    return base[k] + ((tot / par > pole) ? tot / par : pole)
  }
  function place(t,   k, best, bw, w) {
    best = 1; bw = wall(1, cost(t))
    for (k = 2; k <= n; k++) {
      w = wall(k, cost(t))
      if (w < bw || (w == bw && sum[k] < sum[best])) { best = k; bw = w }
    }
    sum[best] += cost(t); if (cost(t) > mx[best]) mx[best] = cost(t)
    slice[best] = slice[best] " " t
  }
  function before(a, b) { return cost(a) > cost(b) || (cost(a) == cost(b) && a < b) }
  FNR == NR {
    if ($0 ~ /^[ \t]*(#|$)/) next
    if ($1 == "base") base[$2] = $3
    else if ($1 == "default") dflt = $2
    else if ($1 == "parallelism") par = $2
    else if ($1 == "contention") slow = $2
    else c[$1] = $2
    next
  }
  { m = split($0, t, " ") }
  END {
    if (n < 1 || par <= 0) { print "tier1-light-plan: bad shard count or parallelism" > "/dev/stderr"; exit 2 }
    for (i = 1; i <= m; i++) {
      if (seen[t[i]]++) { print "tier1-light-plan FAIL: " t[i] " listed twice" > "/dev/stderr"; exit 1 }
      for (j = i; j > 1 && before(t[j], t[j - 1]); j--) { x = t[j]; t[j] = t[j - 1]; t[j - 1] = x }
    }
    for (i = 1; i <= m; i++) place(t[i])
    if (mode == "slice") { sub(/^ /, "", slice[want]); print slice[want]; exit 0 }
    for (k = 1; k <= n; k++) {
      cnt = split(slice[k], s, " ")
      for (i = 1; i <= cnt; i++) got[s[i]]++
      printf "tier1-light-plan: slice %d/%d: %d targets, ~%ds\n", k, n, cnt, wall(k, 0)
    }
    for (x in seen) if (got[x] != 1) { print "tier1-light-plan FAIL: " x " assigned " got[x] + 0 " times" > "/dev/stderr"; bad = 1 }
    for (x in got) if (!(x in seen)) { print "tier1-light-plan FAIL: " x " assigned but not in the pool" > "/dev/stderr"; bad = 1 }
    if (bad) exit 1
    print "tier1-light-plan OK: " m " targets partitioned across " n " slices"
  }
' "$COSTS" -
