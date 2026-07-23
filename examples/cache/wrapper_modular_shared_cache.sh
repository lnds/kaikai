#!/bin/sh
# c-modular shared-cache soundness — program-independent core TUs plus
# race-free publishes.
#
# Part 1 (sequential): two DIFFERENT programs with the same name-level
# surface (one `fn main`, no imports) but different bodies share a fresh
# core cache. Their emit-cache keys collide by design (the key hashes
# the surface, not bodies), so the second build splices the first
# build's core TUs. It must still LINK and RUN: monomorphised instances
# are program-dependent, so they must live in the root TU, never in a
# spliced module TU. A cached core TU carrying program A's instances
# leaves program B's instances undefined (or colliding) at link time.
#
# Part 2 (parallel): N concurrent `KAI_MODULAR=1` builds share fresh
# core + .o caches. Every publish on the path must be atomic
# (temp+rename) so no build ever reads a torn entry; all N binaries
# must build and run correctly.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

# The defect and its fix live on the c-modular (C backend) path; pin the
# backend so the fixture exercises it regardless of the kaic2 default
# (native on the tier1-native runners, C elsewhere).
BE="--backend=c"

CORE_CACHE="$PROJ/core-cache"
OBJ_CACHE="$PROJ/obj-cache"
mkdir -p "$CORE_CACHE" "$OBJ_CACHE"

# ---- part 1: sequential, shared core cache, divergent mono sets ------

mkdir -p "$PROJ/a" "$PROJ/b"
cat > "$PROJ/a/main.kai" <<'EOF'
fn main() : Unit / Console {
  print("a=#{int_to_string(list.sum([1, 2, 3]))}")
}
EOF
# Same surface (one fn main/0), different body: instantiates a core
# generic (list.product) the first program never touches.
cat > "$PROJ/b/main.kai" <<'EOF'
fn main() : Unit / Console {
  print("b=#{int_to_string(list.product([2, 3, 4]))}")
}
EOF

KAI_MODULAR=1 KAI_CORE_CACHE_DIR="$CORE_CACHE" KAI_CORE_CACHE_STATS=1 \
  KAI_MODULAR_CACHE_DIR="$OBJ_CACHE" \
  "$KAI" build $BE "$PROJ/a/main.kai" -o "$PROJ/a/out" 2>"$PROJ/a.err" || {
  echo "wrapper_modular_shared_cache FAIL — cold build of program A failed"
  cat "$PROJ/a.err"; exit 1; }
grep -q "core-emit-cache: miss" "$PROJ/a.err" || {
  echo "wrapper_modular_shared_cache FAIL — program A did not report an emit-cache miss"
  cat "$PROJ/a.err"; exit 1; }

KAI_MODULAR=1 KAI_CORE_CACHE_DIR="$CORE_CACHE" KAI_CORE_CACHE_STATS=1 \
  KAI_MODULAR_CACHE_DIR="$OBJ_CACHE" \
  "$KAI" build $BE "$PROJ/b/main.kai" -o "$PROJ/b/out" 2>"$PROJ/b.err" || {
  echo "wrapper_modular_shared_cache FAIL — program B did not build/link on program A's warm core cache"
  cat "$PROJ/b.err"; exit 1; }
grep -q "core-emit-cache: hit" "$PROJ/b.err" || {
  echo "wrapper_modular_shared_cache FAIL — program B missed the emit cache (keys diverged; splice untested)"
  cat "$PROJ/b.err"; exit 1; }

[ "$("$PROJ/a/out")" = "a=6" ] || {
  echo "wrapper_modular_shared_cache FAIL — program A output wrong"; exit 1; }
[ "$("$PROJ/b/out")" = "b=24" ] || {
  echo "wrapper_modular_shared_cache FAIL — program B output wrong"; exit 1; }
echo "wrapper_modular_shared_cache OK (sequential: B links its own monos on A's warm core cache)"

# ---- part 2: parallel builds, fresh shared caches --------------------

rm -rf "$CORE_CACHE" "$OBJ_CACHE"
mkdir -p "$CORE_CACHE" "$OBJ_CACHE"

N=6
i=1
while [ "$i" -le "$N" ]; do
  mkdir -p "$PROJ/p$i"
  if [ $((i % 2)) = 0 ]; then
    body="list.sum([1, 2, $i])"
  else
    body="list.product([1, 2, $i])"
  fi
  cat > "$PROJ/p$i/main.kai" <<EOF
fn main() : Unit / Console {
  print("p$i=#{int_to_string($body)}")
}
EOF
  i=$((i + 1))
done

pids=""
i=1
while [ "$i" -le "$N" ]; do
  KAI_MODULAR=1 KAI_CORE_CACHE_DIR="$CORE_CACHE" \
    KAI_MODULAR_CACHE_DIR="$OBJ_CACHE" \
    "$KAI" build $BE "$PROJ/p$i/main.kai" -o "$PROJ/p$i/out" \
    2>"$PROJ/p$i.err" &
  pids="$pids $!"
  i=$((i + 1))
done

i=1
fail=0
for pid in $pids; do
  wait "$pid" || { echo "wrapper_modular_shared_cache FAIL — parallel build p$i failed"; cat "$PROJ/p$i.err"; fail=1; }
  i=$((i + 1))
done
[ "$fail" = "0" ] || exit 1

i=1
while [ "$i" -le "$N" ]; do
  if [ $((i % 2)) = 0 ]; then want="p$i=$((3 + i))"; else want="p$i=$((2 * i))"; fi
  got="$("$PROJ/p$i/out")"
  [ "$got" = "$want" ] || {
    echo "wrapper_modular_shared_cache FAIL — p$i output '$got', want '$want'"; exit 1; }
  i=$((i + 1))
done
echo "wrapper_modular_shared_cache OK (parallel: $N concurrent builds on shared fresh caches)"
