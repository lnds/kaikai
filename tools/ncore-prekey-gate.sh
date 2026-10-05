#!/bin/sh
# Native core-object pre-key gate. A warm native build finds the cached
# core object before typing through a pre-key that leaves the core's KIR
# out, which is sound only while that KIR does not depend on the program.
#
# 1. Invariance: an effect-heavy and a protocol/generic-heavy program,
#    each built cold in its own cache, produce the same core object.
# 2. A warm build of each prunes the core and still prints its golden,
#    including core generics instantiated at the program's own types.

set -eu
KAI="$1"
WORK="$2"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
A="$ROOT/examples/effects/handler_wide_frames"
B="$ROOT/examples/stdlib/core_hit_mono"

rm -rf "$WORK"; mkdir -p "$WORK/a" "$WORK/b"

# Every run compiles: a memoised binary would skip the warm build the gate checks.
run() { # cache-dir program out
  KAI_BIN_MEMO=0 KAI_CORE_CACHE_DIR="$1" KAI_CORE_CACHE_STATS=1 "$KAI" run "$2.kai" > "$3.out" 2> "$3.err"
}

run "$WORK/a" "$A" "$WORK/a-cold"
run "$WORK/b" "$B" "$WORK/b-cold"
ka=$(cd "$WORK/a" && ls */ncore-*.o 2>/dev/null | sed 's#.*/##')
kb=$(cd "$WORK/b" && ls */ncore-*.o 2>/dev/null | sed 's#.*/##')
if [ -z "$ka" ] || [ -z "$kb" ]; then
  echo "ncore-prekey: SKIP — no native core object was cached"; exit 0
fi
[ "$ka" = "$kb" ] || {
  echo "ncore-prekey: FAIL — the core object depends on the program ($ka vs $kb)"; exit 1; }

for p in A B; do
  eval src=\$$p
  run "$WORK/a" "$src" "$WORK/$p-warm"
  grep -q "native-core-obj: hit (core pruned)" "$WORK/$p-warm.err" || {
    echo "ncore-prekey: FAIL — warm build of $(basename "$src") did not prune"; cat "$WORK/$p-warm.err"; exit 1; }
  cmp -s "$src.out.expected" "$WORK/$p-warm.out" || {
    echo "ncore-prekey: FAIL — pruned warm build of $(basename "$src") diverges from its golden"
    diff "$src.out.expected" "$WORK/$p-warm.out" | head -20; exit 1; }
done
grep -l "native-core-index: conflict" "$WORK"/*.err > /dev/null 2>&1 && {
  echo "ncore-prekey: FAIL — a pre-key recorded two core objects"; exit 1; }
echo "ncore-prekey OK — one core object for both programs; warm builds prune and match their goldens"
