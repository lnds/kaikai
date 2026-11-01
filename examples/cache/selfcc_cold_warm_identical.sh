#!/bin/sh
# The compiler's own source, built with the per-module typed cache, is
# byte-identical to the plain build in both outputs the cache could
# perturb: the emitted C and the KIR dump. The cold build (every module
# misses and publishes) and the warm one (every module hits) must each
# equal the oracle.
#
# A hit that serves stale content is a correctness bug, and a program of
# 200+ modules is the shape that exercises every cross-module path.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT INT TERM

fail() { echo "selfcc_cold_warm_identical FAIL — $1"; exit 1; }

build() {
  out="$1"; shift
  if ! "$KAIC2" $EDITION_FLAG --path "$ROOT/stdlib" "$@" "$ROOT/stage2/main.kai" > "$out" 2>"$WORK/err"; then
    cat "$WORK/err"
    fail "build exited non-zero ($*)"
  fi
}

same() {
  cmp -s "$1" "$2" || { diff "$1" "$2" | head -20; fail "$3"; }
}

cache_flags="--user-cache --user-cache-dir $WORK/cache"
mkdir -p "$WORK/cache"

build "$WORK/plain.c"
build "$WORK/cold.c" $cache_flags
blobs="$(ls "$WORK/cache" | wc -l | tr -d ' ')"
[ "$blobs" -gt 0 ] || fail "the cold build published no blob"
build "$WORK/warm.c" $cache_flags
[ "$(ls "$WORK/cache" | wc -l | tr -d ' ')" = "$blobs" ] || fail "the warm build published new blobs"
same "$WORK/plain.c" "$WORK/cold.c" "C: cold build differs from plain"
same "$WORK/plain.c" "$WORK/warm.c" "C: warm build differs from plain"

build "$WORK/plain.kir" --emit=kir
build "$WORK/warm.kir" --emit=kir $cache_flags
same "$WORK/plain.kir" "$WORK/warm.kir" "KIR: warm build differs from plain"

echo "selfcc_cold_warm_identical OK — cold and warm self-compile match plain in C and KIR"
