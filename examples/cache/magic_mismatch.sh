#!/bin/sh
# Phase A cache invalidation fixture #1 — magic mismatch.
#
# Plants a cache .kab whose 4-byte magic is "KAB0" instead of the
# current "KAB2". `cache_read_header` rejects the blob, the loader
# falls back to `load_prelude`, the compile succeeds.
#
# KAB2 header layout (76 bytes, fixed):
#   magic           4 bytes  "KAB2"
#   format_version  4 bytes  u32 LE  (current = 2)
#   kaikai_version  4 bytes  u32 LE  (current = 1)
#   source_sha     64 bytes  ASCII lowercase hex

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CACHE_DIR="$(mktemp -d)"
trap 'rm -rf "$CACHE_DIR"' EXIT INT TERM

SRC="$ROOT/stdlib/core/char.kai"
SHA="$(shasum -a 256 "$SRC" | awk '{ print $1 }')"
CACHE_FILE="$CACHE_DIR/${SHA}.kab"

# Seed: valid header layout but magic = "KAB0" (one byte off).
# format_version + kaikai_version match the current binary so the
# rejection is specifically on the magic.
printf 'KAB0' > "$CACHE_FILE"
printf '\002\000\000\000' >> "$CACHE_FILE"  # format_version 2 LE
printf '\001\000\000\000' >> "$CACHE_FILE"  # kaikai_version_hash 1 LE
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
    echo "magic_mismatch OK — loader fell back to re-parse"
    exit 0 ;;
  *)
    echo "magic_mismatch FAIL — compile did not produce expected output"
    echo "--- driver output ---"
    printf '%s\n' "$out"
    exit 1 ;;
esac
