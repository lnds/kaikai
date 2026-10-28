#!/bin/sh
# Builds racing one shared cache while an evictor keeps ageing and trimming
# its core objects (a trim takes only entries unused for a while) never fail
# and always run correctly: a cache that loses an entry mid-build costs a
# recompile, not a build error.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
"$ROOT/tools/kaic2-native-capable.sh" "$ROOT/stage2/kaic2" > /dev/null 2>&1 || {
  echo "native-cache-eviction SKIP (kaic2 has no native backend)"
  exit 0
}
WORK="$(mktemp -d)"
trap 'kill "${EVICTOR:-0}" 2> /dev/null || true; rm -rf "$WORK"' EXIT INT TERM
JOBS=4
ROUNDS=3
XDG="$WORK/xdg"
for i in $(seq 1 "$JOBS"); do
  mkdir -p "$WORK/p$i"
  printf 'fn main() : Unit / Console = println("evict-ok %s")\n' "$i" > "$WORK/p$i/main.kai"
done

build() {
  (cd "$WORK/p$1" && XDG_CACHE_HOME="$XDG" KAI_BACKEND=native "$KAI" build main.kai -o "$WORK/out-$1-$2" > "$WORK/log-$1-$2" 2>&1)
}

# Warm the cache so every later build prunes against an index hit.
build 1 0 || { echo "native-cache-eviction FAIL (warm build)"; cat "$WORK/log-1-0"; exit 1; }

(while :; do
  find "$XDG/kai/core" -name 'ncore-*.o' -exec touch -t 200001010000 {} + 2> /dev/null || true
  sleep 0.2
  find "$XDG/kai/core" -name 'ncore-*.o' -mmin +60 -delete 2> /dev/null || true
done) &
EVICTOR=$!

fail=0
for r in $(seq 1 "$ROUNDS"); do
  pids=""
  for i in $(seq 1 "$JOBS"); do
    build "$i" "$r" &
    pids="$pids $!"
  done
  for pid in $pids; do wait "$pid" || true; done
  for i in $(seq 1 "$JOBS"); do
    if [ "$("$WORK/out-$i-$r" 2> /dev/null)" != "evict-ok $i" ]; then
      echo "native-cache-eviction FAIL (job $i round $r)"; cat "$WORK/log-$i-$r"; fail=1
    fi
  done
done
[ "$fail" = 0 ] && echo "native-cache-eviction OK"
exit "$fail"
