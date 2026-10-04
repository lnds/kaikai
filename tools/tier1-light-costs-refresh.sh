#!/bin/sh
# tools/tier1-light-costs-refresh.sh — rebuild tools/tier1-light-costs.txt
# from the logs of tier1 CI runs.
#
#   tier1-light-costs-refresh.sh <log>...     rewrite the cost table
#   ... | tier1-light-costs-refresh.sh        same, logs on stdin
#
# A log is the text of one or more tier1-shard-N jobs, in any wrapping
# (GitHub's timestamps and `gh run view --log` prefixes are skipped):
#
#   gh run view <run-id> --log > run.log
#   tools/tier1-light-costs-refresh.sh run.log
#   make test-light-partition
#
# Feed runs of PRs that touch the compiler: a shard that skipped its
# self-host says so in its line and is left out of the bases. Several runs
# are better than one, since runner speed varies; each row is the median
# of its samples.
#
# The lines it reads, printed by the shard recipes:
#   tier1-light-cost <target> <wall s> <user> <sys>   one light target
#   tier1-light-slice <shard> <wall s>                the shard's light slice
#   tier1-shard-wall <shard> <wall s> selfhosts=<0|1> the whole shard
#
# A target's row is its CPU time (user + sys, which adds up across targets
# sharing a runner) and its wall time (which bounds the slice it is in). A
# base is the shard's wall minus its slice's. The comment header and the
# `default` / `parallelism` lines are kept as they are; targets with no
# sample keep their old row, and rows for targets no longer in the pool
# are dropped. The report on stderr gives each slice's utilisation (CPU /
# wall): `parallelism` should sit near the highest one.

set -eu

here=$(dirname "$0")
costs="$here/tier1-light-costs.txt"
pool=$(grep -v '^[[:space:]]*$' "$here/../stage2/test-lists/light.txt" | tr '\n' ' ')
[ -n "$pool" ] || { echo "tier1-light-costs-refresh: no targets in stage2/test-lists/light.txt" >&2; exit 2; }

tmp=$(mktemp)
trap 'rm -f "$tmp" "$tmp.rows"' EXIT

cat "$@" | awk -v pool="$pool" -v rows="$tmp.rows" '
  function secs(x,   a) { split(x, a, "m"); return a[1] * 60 + a[2] }
  function add(list, k, v) { list[k] = (k in list) ? list[k] " " v : v }
  function median(s,   a, n, i, j, x) {
    n = split(s, a, " ")
    for (i = 2; i <= n; i++)
      for (j = i; j > 1 && a[j] + 0 < a[j - 1] + 0; j--) { x = a[j]; a[j] = a[j - 1]; a[j - 1] = x }
    return (n % 2) ? a[(n + 1) / 2] : (a[n / 2] + a[n / 2 + 1]) / 2
  }
  function ceil(x) { return (x == int(x)) ? x : int(x) + 1 }
  FNR == NR {
    if ($0 ~ /^[ \t]*#/ && !body) { print; next }
    body = 1
    if ($1 == "default" || $1 == "parallelism") keep[++nkeep] = $0
    else if ($1 == "base") { oldbase[$2] = $3; if ($2 > nbase) nbase = $2 }
    else if (NF >= 2) { oldc[$1] = $2; oldw[$1] = (NF > 2) ? $3 : $2 }
    next
  }
  {
    for (i = 1; i <= NF; i++) if ($i ~ /^tier1-(light-cost|light-slice|shard-wall)$/) break
    if (i > NF) next
    if ($i == "tier1-light-cost") {
      add(cpu, $(i + 1), secs($(i + 3)) + secs($(i + 4))); add(wall, $(i + 1), $(i + 2))
      runcpu += secs($(i + 3)) + secs($(i + 4))
    } else if ($i == "tier1-light-slice") {
      slice[$(i + 1)] = $(i + 2)
      printf "slice %s: wall %ds, cpu %ds, utilisation %.2f\n", $(i + 1), $(i + 2), runcpu, ($(i + 2) > 0) ? runcpu / $(i + 2) : 0 > "/dev/stderr"
      runcpu = 0
    } else if ($(i + 3) == "selfhosts=1" && ($(i + 1) in slice)) {
      add(base, $(i + 1), $(i + 2) - slice[$(i + 1)]); delete slice[$(i + 1)]
      if ($(i + 1) > nbase) nbase = $(i + 1)
    }
  }
  END {
    print ""
    for (i = 1; i <= nkeep; i++) print keep[i]
    print ""
    for (k = 1; k <= nbase; k++) {
      if (k in base) print "base", k, ceil(median(base[k]))
      else { print "base", k, oldbase[k] + 0; print "base " k ": no sample, kept" > "/dev/stderr" }
    }
    print ""
    n = split(pool, t, " ")
    for (i = 1; i <= n; i++) {
      inpool[t[i]] = 1
      if (t[i] in cpu) { print t[i], ceil(median(cpu[t[i]])), ceil(median(wall[t[i]])) > rows; fresh++ }
      else if (t[i] in oldc) { print t[i], oldc[t[i]], oldw[t[i]] > rows; stale = stale " " t[i] }
      else unlisted = unlisted " " t[i]
    }
    for (x in oldc) if (!(x in inpool)) dropped = dropped " " x
    printf "%d of %d targets measured\n", fresh, n > "/dev/stderr"
    if (stale != "") print "no sample, old row kept:" stale > "/dev/stderr"
    if (unlisted != "") print "no sample and no row (planned at the default):" unlisted > "/dev/stderr"
    if (dropped != "") print "left the pool, row dropped:" dropped > "/dev/stderr"
  }
' "$costs" - > "$tmp"

sort -k3,3nr -k2,2nr -k1,1 "$tmp.rows" >> "$tmp"
cp "$tmp" "$costs"
