#!/bin/sh
# Phase A.0 cache invalidation fixture #2 — format_version mismatch.
#
# Plants a cache .kab whose magic + sha lines are well-formed but the
# format_version field is 99 (current compiler expects 1).
# `cache_read_header` rejects on the format_version comparison;
# the loader falls back to a full parse.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CACHE_DIR="$(mktemp -d)"
trap 'rm -rf "$CACHE_DIR"' EXIT INT TERM

SRC="$ROOT/stdlib/core/char.kai"
SHA="$(shasum -a 256 "$SRC" | awk '{ print $1 }')"
CACHE_FILE="$CACHE_DIR/${SHA}.kab"

# Seed: magic OK, format_version = 99 (0x63), version_hash OK, sha OK.
# Payload empty — the header check rejects before any payload work.
{
  printf 'KAB1\n'
  printf '63000000\n'        # format_version = 99 → mismatch
  printf '01000000\n'
  printf '%s\n' "0000000000000000000000000000000000000000000000000000000000000000"
} > "$CACHE_FILE"

cat > "$CACHE_DIR/empty.kai" <<'EOF'
fn main() : Unit / Console {
  print("hi")
}
EOF

out="$(KAI_PRELUDE_CACHE=1 KAI_PRELUDE_CACHE_DIR="$CACHE_DIR" \
       "$ROOT/bin/kai" run "$CACHE_DIR/empty.kai" 2>&1 || true)"

case "$out" in
  *hi*)
    echo "format_version_mismatch OK — loader fell back to re-parse"
    exit 0 ;;
  *)
    echo "format_version_mismatch FAIL — compile did not produce expected output"
    echo "--- driver output ---"
    printf '%s\n' "$out"
    exit 1 ;;
esac
