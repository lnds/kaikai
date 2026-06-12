#!/bin/sh
# Phase B user-cache invalidation fixture #1 — edit the file's own source.
#
# A user module's content hash is half of its composite cache key. When
# the source bytes change, the content hash changes, so the blob's
# content-addressed filename changes and the old entry is never read.
# The build must reflect the new source, not serve stale decls.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

cat > "$PROJ/lib.kai" <<'EOF'
pub fn value() : Int = 1
EOF
cat > "$PROJ/main.kai" <<'EOF'
import lib
fn main() : Unit / Console { print(int_to_string(lib.value())) }
EOF

# Warm the cache, then run again — expect 1 both times.
out1="$(KAI_CACHE=1 "$KAI" run "$PROJ/main.kai" 2>/dev/null || true)"
out2="$(KAI_CACHE=1 "$KAI" run "$PROJ/main.kai" 2>/dev/null || true)"
if [ "$out1" != "1" ] || [ "$out2" != "1" ]; then
  echo "userb_edit_source FAIL — warm runs did not print 1 (got '$out1','$out2')"
  exit 1
fi

# Edit lib.kai's body. Its content hash changes; the cached blob under
# the old key is now invisible. The next build must print 2, not 1.
cat > "$PROJ/lib.kai" <<'EOF'
pub fn value() : Int = 2
EOF
out3="$(KAI_CACHE=1 "$KAI" run "$PROJ/main.kai" 2>/dev/null || true)"
if [ "$out3" != "2" ]; then
  echo "userb_edit_source FAIL — stale cache served '$out3', expected 2"
  exit 1
fi

echo "userb_edit_source OK — content-hash change invalidated the cache"
exit 0
