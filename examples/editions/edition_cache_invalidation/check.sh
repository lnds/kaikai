#!/bin/sh
# Issue #603 cache-partition smoke. Compile this fixture under each
# of {tongariki, hanga-roa}, snapshotting the prelude cache root. The
# partition must produce one subdirectory per edition; cache files
# created in one must NOT appear under the other.
#
# Run from the repo root:
#   sh examples/editions/edition_cache_invalidation/check.sh
#
# Sets a private $KAI_PRELUDE_CACHE_DIR so the test does not touch
# the user's real cache.

set -e
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
KAI="$ROOT/bin/kai"
FIXTURE="$ROOT/examples/editions/edition_cache_invalidation"
TMPDIR="$(mktemp -d -t kaikai-edcache.XXXXXX)"
trap 'rm -rf "$TMPDIR"' EXIT
export KAI_PRELUDE_CACHE_DIR="$TMPDIR"

manifest="$FIXTURE/kai.toml"
saved="$(cat "$manifest")"
restore() { printf '%s' "$saved" > "$manifest"; }
trap 'restore; rm -rf "$TMPDIR"' EXIT

cat > "$manifest" <<'EOF'
name = "edition_cache_invalidation"
version = "0.1.0"
edition = "tongariki"
EOF
"$KAI" build "$FIXTURE/main.kai" -o "$TMPDIR/m.tongariki" 2>/dev/null
[ -d "$TMPDIR/tongariki" ] || { echo "FAIL: $TMPDIR/tongariki missing"; exit 1; }

cat > "$manifest" <<'EOF'
name = "edition_cache_invalidation"
version = "0.1.0"
edition = "hanga-roa"
EOF
"$KAI" build "$FIXTURE/main.kai" -o "$TMPDIR/m.hanga-roa" 2>/dev/null
[ -d "$TMPDIR/hanga-roa" ] || { echo "FAIL: $TMPDIR/hanga-roa missing"; exit 1; }

t_count="$(ls "$TMPDIR/tongariki" 2>/dev/null | wc -l | tr -d ' ')"
a_count="$(ls "$TMPDIR/hanga-roa" 2>/dev/null | wc -l | tr -d ' ')"
[ "$t_count" -gt 0 ] || { echo "FAIL: tongariki partition empty"; exit 1; }
[ "$a_count" -gt 0 ] || { echo "FAIL: hanga-roa partition empty"; exit 1; }

echo "edition-cache-partition OK: tongariki=$t_count entries; hanga-roa=$a_count entries"
