#!/bin/sh
# Gate: a multi-threaded program that peaked and went idle gives its cell
# pools back. The C backend is built with KAI_CELL_POOL_TRACE, which reports
# each trim; the pooled cells trimmed between the peak and the idle marker
# must cover the pools the peak filled. RSS is printed for reference only
# (the allocator decides when freed pages leave the process).
# Usage: tools/cell-pool-trim-gate.sh [kai]
set -eu
cd "$(dirname "$0")/.."
kai=${1:-./bin/kai}
src=tools/cell-pool-trim-prog.kai
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT

CFLAGS="${CFLAGS:-} -DKAI_CELL_POOL_TRACE" "$kai" build --backend=c "$src" -o "$dir/prog" >/dev/null
KAI_THREADS=4 "$dir/prog" >"$dir/out" 2>"$dir/err" &
pid=$!

wait_for() {
  n=0
  until grep -q "$1" "$dir/out"; do
    n=$((n + 1))
    [ $n -le 300 ] || { echo "FAIL: marker '$1' never printed" >&2; kill $pid 2>/dev/null; exit 1; }
    sleep 0.1
  done
}
rss() { ps -o rss= -p $pid | tr -d ' '; }

wait_for peak
before=$(rss)
wait_for idle
after=$(rss)
wait $pid

trimmed=$(awk '/^cell-pool-trim /{ s += $2 } END { print s + 0 }' "$dir/err")
echo "rss after peak: ${before} KiB, once idle: ${after} KiB; cells trimmed: ${trimmed}"
# 8 fibers over 4 workers leave far more than this pooled.
[ "$trimmed" -ge 1000000 ] || { echo "FAIL: only ${trimmed} cells trimmed" >&2; exit 1; }
echo "ok"
