#!/bin/sh
# Phase A.0 cache invalidation fixture #1 — magic mismatch.
#
# Plants a cache .kab whose 4-byte magic is "KAB0" instead of the
# current "KAB1". `cache_read_header` rejects the blob, the loader
# falls back to `load_prelude`, the compile succeeds.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CACHE_DIR="$(mktemp -d)"
trap 'rm -rf "$CACHE_DIR"' EXIT INT TERM

# Use a tiny stdlib file as the "victim" prelude. char.kai is the
# smallest core prelude and the alphabetical-first one bin/kai loads,
# so a single corrupted cache entry is enough to exercise the path.
SRC="$ROOT/stdlib/core/char.kai"
SHA="$(shasum -a 256 "$SRC" | awk '{ print $1 }')"
CACHE_FILE="$CACHE_DIR/${SHA}.kab"

# Seed: valid header layout, BUT magic = KAB0 (one digit off).
{
  printf 'KAB0\n'
  printf '01000000\n'        # format_version (matches current)
  printf '01000000\n'        # kaikai_version_hash (matches current)
  printf '%s\n' "0000000000000000000000000000000000000000000000000000000000000000"
} > "$CACHE_FILE"

cat > "$CACHE_DIR/empty.kai" <<'EOF'
fn main() : Unit / Console {
  print("hi")
}
EOF

# Compile with the seeded cache directory. Expect: success + "hi"
# on stdout. Stderr will contain the cache-miss diagnostic; we don't
# require its exact wording, only that the build does not fail.
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
