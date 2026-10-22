#!/bin/sh
# An unfilled hole or todo! panics with the same stderr text on both backends.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
DIR="$ROOT/examples/holes_panic"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT INT TERM

for f in "$DIR"/*.kai; do
  name="$(basename "$f" .kai)"
  for be in c native; do
    KAI_BACKEND=$be KAI_CORE_CACHE_DIR="$OUT/core-$be" "$KAI" build "$f" -o "$OUT/$name-$be" >/dev/null 2>"$OUT/build.err" \
      || { echo "holes_panic FAIL $name ($be) — build failed"; cat "$OUT/build.err"; exit 1; }
    "$OUT/$name-$be" >/dev/null 2>"$OUT/$name-$be.err" || true
    diff "$DIR/$name.err.expected" "$OUT/$name-$be.err" >/dev/null \
      || { echo "holes_panic FAIL $name ($be)"; diff "$DIR/$name.err.expected" "$OUT/$name-$be.err" || true; exit 1; }
  done
done
echo "holes_panic OK — hole and todo! panics read the same on both backends"
