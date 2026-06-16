#!/bin/sh
# native-perf/run.sh — honest native-vs-C codegen benchmark harness.
#
# For each bench under benches/, builds it with the C backend and the
# native (in-process libLLVM) backend, then times the resulting binary
# (best of N runs). Reports binary size and wall time per route so the
# native-vs-C gap is measured on the SAME kaic2.
#
# Usage:  tools/native-perf/run.sh [bench.kai ...]
# With no args, runs every benches/*.kai.
#
# Env:
#   KAI      path to the kai wrapper       (default ./bin/kai)
#   RUNS     timing runs per binary        (default 3, reports best)
#   ROUTES   space-separated route list    (default "c native")
set -eu
_ZO_DOCTOR=0; export _ZO_DOCTOR

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="${KAI:-$ROOT/bin/kai}"
RUNS="${RUNS:-3}"
ROUTES="${ROUTES:-c native}"
BENCHDIR="$ROOT/tools/native-perf/benches"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Support modules (no `main`) live alongside the benches; the harness
# only drives files that define `fn main`. rb_tree.kai is imported by
# rbtree_corpus.kai, not a bench of its own.
if [ "$#" -gt 0 ]; then
  BENCHES="$*"
else
  BENCHES=""
  for f in "$BENCHDIR"/*.kai; do
    grep -q '^fn main' "$f" && BENCHES="$BENCHES $f"
  done
fi

# best-of-N wall time in seconds for a command, via the shell's builtin
# `time` is unreliable; use /usr/bin/time -p and parse `real`.
best_time() {
  best=""
  i=0
  while [ "$i" -lt "$RUNS" ]; do
    t="$( { /usr/bin/time -p "$@" >/dev/null 2>/tmp/.npt; } 2>&1; grep '^real' /tmp/.npt | awk '{print $2}')"
    if [ -z "$best" ] || awk "BEGIN{exit !($t < $best)}"; then best="$t"; fi
    i=$((i + 1))
  done
  printf '%s' "$best"
}

printf '%-28s %-8s %10s %10s\n' "bench" "route" "size(KB)" "best(s)"
printf '%-28s %-8s %10s %10s\n' "-----" "-----" "--------" "-------"
for b in $BENCHES; do
  name="$(basename "$b" .kai)"
  for route in $ROUTES; do
    out="$TMP/${name}_${route}"
    if ! KAI_BACKEND="$route" "$KAI" build "$b" -o "$out" >/tmp/.npbuild 2>&1; then
      printf '%-28s %-8s %10s %10s\n' "$name" "$route" "BUILD-FAIL" "-"
      tail -3 /tmp/.npbuild | sed 's/^/    /'
      continue
    fi
    sz="$(( $(wc -c < "$out") / 1024 ))"
    t="$(best_time "$out")"
    printf '%-28s %-8s %10s %10s\n' "$name" "$route" "$sz" "$t"
  done
done
