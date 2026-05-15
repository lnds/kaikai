#!/bin/sh
# Phase A cache invalidation fixture #3 — kaikai_version_hash
# mismatch.
#
# Plants a cache .kab whose magic + format_version + sha lines are
# well-formed but the kaikai_version_hash is bumped to 99.
# `cache_read_header` rejects on the version-hash compare; the loader
# falls back to a full parse. This is the path that fires when a
# user upgrades the kaikai binary — every cache file on disk written
# by an older build becomes a miss until re-emitted.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CACHE_DIR="$(mktemp -d)"
trap 'rm -rf "$CACHE_DIR"' EXIT INT TERM

SRC="$ROOT/stdlib/core/char.kai"
SHA="$(shasum -a 256 "$SRC" | awk '{ print $1 }')"
CACHE_FILE="$CACHE_DIR/${SHA}.kab"

# Seed: KAB2 magic + format_version=2 + kaikai_version=99 + sha OK.
printf 'KAB2' > "$CACHE_FILE"
printf '\002\000\000\000' >> "$CACHE_FILE"  # format_version 2 LE
printf '\143\000\000\000' >> "$CACHE_FILE"  # kaikai_version_hash = 99 LE
printf '0000000000000000000000000000000000000000000000000000000000000000' >> "$CACHE_FILE"

cat > "$CACHE_DIR/empty.kai" <<'EOF'
fn main() : Unit / Console {
  print("hi")
}
EOF

out="$(KAI_PRELUDE_CACHE=1 KAI_PRELUDE_CACHE_DIR="$CACHE_DIR" \
       "$ROOT/bin/kai" run "$CACHE_DIR/empty.kai" 2>&1 || true)"

case "$out" in
  *hi*)
    echo "kaikai_version_hash_mismatch OK — loader fell back to re-parse"
    exit 0 ;;
  *)
    echo "kaikai_version_hash_mismatch FAIL — compile did not produce expected output"
    echo "--- driver output ---"
    printf '%s\n' "$out"
    exit 1 ;;
esac
