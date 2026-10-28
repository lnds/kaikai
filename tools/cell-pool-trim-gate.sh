#!/bin/sh
# Gate: a program that peaks and goes idle, round after round, gives its cell
# pools back each time, on several worker threads and on the lone main thread
# of a single-threaded run. The C backend is built with KAI_CELL_POOL_TRACE,
# which reports each trim; the cells trimmed must cover what the peaks
# filled. Process memory is not asserted: when freed pages leave the process
# is the allocator's call.
# Usage: tools/cell-pool-trim-gate.sh [kai]
set -eu
cd "$(dirname "$0")/.."
kai=${1:-./bin/kai}
src=tools/cell-pool-trim-prog.kai
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT

CFLAGS="${CFLAGS:--O2} -DKAI_CELL_POOL_TRACE" "$kai" build --backend=c "$src" -o "$dir/prog" >/dev/null

check() {
  threads=$1
  min=$2
  KAI_THREADS=$threads "$dir/prog" >/dev/null 2>"$dir/err"
  peaks=$(grep -c '^peak' "$dir/err")
  trimmed=$(awk '/^cell-pool-trim /{ s += $2 } END { print s + 0 }' "$dir/err")
  echo "threads=${threads}: peaks ${peaks}, cells trimmed ${trimmed}"
  [ "$peaks" -eq 6 ] || { echo "FAIL: expected 6 peaks" >&2; exit 1; }
  [ "$trimmed" -ge "$min" ] || { echo "FAIL: only ${trimmed} cells trimmed (need ${min})" >&2; exit 1; }
}

check 4 6000000
check 1 3000000
echo "ok"
