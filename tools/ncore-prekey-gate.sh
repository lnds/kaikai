#!/bin/sh
# Native core-object pre-key gate. A warm native build finds the cached
# core object before typing through a pre-key that leaves the core's KIR
# out, which is sound only while that KIR does not depend on the program.
#
# 1. Invariance: an effect-heavy and a protocol/generic-heavy program,
#    each built cold in its own cache, produce the same core object.
# 2. A warm build of each prunes the core and still prints its golden,
#    including core generics instantiated at the program's own types.
# 3. Run as a user runs it, a build of a second program finds the core
#    object the first left: it writes nothing to the cache, so it builds
#    against a read-only one.
# 4. So does a build the read-only cache holds no core object for: its
#    core object goes beside the program and the cache gains nothing.

set -eu
KAI="$1"
WORK="$2"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
A="$ROOT/examples/effects/handler_wide_frames"
B="$ROOT/examples/stdlib/core_hit_mono"

[ -d "$WORK" ] && chmod -R u+w "$WORK"
rm -rf "$WORK"; mkdir -p "$WORK/a" "$WORK/b" "$WORK/c"

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
# 2b. The thread and job counts shape no object, so a count the cache has not seen keeps the prune.
for n in 1 2; do
  KAI_NATIVE_JOBS=$n KAI_JOBS=$n run "$WORK/a" "$A" "$WORK/jobs$n"
  grep -q "native-core-obj: hit (core pruned)" "$WORK/jobs$n.err" || {
    echo "ncore-prekey: FAIL — a build with KAI_NATIVE_JOBS=$n lost the pruned fast path"; cat "$WORK/jobs$n.err"; exit 1; }
done
grep -l "native-core-index: conflict" "$WORK"/*.err > /dev/null 2>&1 && {
  echo "ncore-prekey: FAIL — a pre-key recorded two core objects"; exit 1; }

build() { # program out
  KAI_CORE_CACHE_DIR="$WORK/c" "$KAI" build "$1.kai" -o "$2" > "$2.out" 2> "$2.err"
}
build "$A" "$WORK/c-first" || {
  echo "ncore-prekey: FAIL — the build that fills the cache failed"; cat "$WORK/c-first.err"; exit 1; }
chmod -R a-w "$WORK/c"
rc=0; build "$B" "$WORK/c-second" || rc=$?
chmod -R u+w "$WORK/c"
[ "$rc" = 0 ] && [ ! -s "$WORK/c-second.err" ] || {
  echo "ncore-prekey: FAIL — a second program does not build against a read-only cache (exit $rc)"
  cat "$WORK/c-second.err"; exit 1; }
"$WORK/c-second" > "$WORK/c-second.run" 2>&1
cmp -s "$B.out.expected" "$WORK/c-second.run" || {
  echo "ncore-prekey: FAIL — the build against a read-only cache diverges from its golden"
  diff "$B.out.expected" "$WORK/c-second.run" | head -20; exit 1; }
rm -f "$WORK"/c/*/ncore-*
chmod -R a-w "$WORK/c"
rc=0; build "$B" "$WORK/c-bare" || rc=$?
chmod -R u+w "$WORK/c"
[ "$rc" = 0 ] && [ ! -s "$WORK/c-bare.err" ] || {
  echo "ncore-prekey: FAIL — a build does not get by without writing its core object to the cache (exit $rc)"
  cat "$WORK/c-bare.err"; exit 1; }
"$WORK/c-bare" > "$WORK/c-bare.run" 2>&1
cmp -s "$B.out.expected" "$WORK/c-bare.run" || {
  echo "ncore-prekey: FAIL — the build with its core object beside it diverges from its golden"
  diff "$B.out.expected" "$WORK/c-bare.run" | head -20; exit 1; }
[ -z "$(ls "$WORK"/c/*/ 2>/dev/null | grep '^ncore-')" ] || {
  echo "ncore-prekey: FAIL — a read-only cache gained a core object"; exit 1; }
echo "ncore-prekey OK — one core object for both programs; warm builds prune and match their goldens; a read-only cache builds"
