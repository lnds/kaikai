#!/bin/sh
# Phase B user-cache invalidation fixture #2 — edit a direct import.
#
# A module's composite key folds in the content hashes of its imports.
# Editing a directly-imported module changes the importer's dependency
# hash even though the importer's own bytes are untouched, so the
# importer misses the cache and rebuilds against the new dependency.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

cat > "$PROJ/dep.kai" <<'EOF'
pub fn base() : Int = 10
EOF
cat > "$PROJ/main.kai" <<'EOF'
import dep
fn main() : Unit / Console { print(int_to_string(dep.base() + 5)) }
EOF

out1="$(KAI_CACHE=1 "$KAI" run "$PROJ/main.kai" 2>/dev/null || true)"
if [ "$out1" != "15" ]; then
  echo "userb_edit_import FAIL — warm run did not print 15 (got '$out1')"
  exit 1
fi

# Edit the directly-imported dep. main.kai's own bytes are unchanged,
# but its dependency hash now differs, so its cached blob is invisible
# and the build must reflect the new dep (100 + 5 = 105).
cat > "$PROJ/dep.kai" <<'EOF'
pub fn base() : Int = 100
EOF
out2="$(KAI_CACHE=1 "$KAI" run "$PROJ/main.kai" 2>/dev/null || true)"
if [ "$out2" != "105" ]; then
  echo "userb_edit_import FAIL — stale cache served '$out2', expected 105"
  exit 1
fi

echo "userb_edit_import OK — direct-import edit invalidated the consumer"
exit 0
