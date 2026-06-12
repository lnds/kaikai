#!/bin/sh
# Phase B user-cache invalidation fixture #3 — edit a transitive import.
#
# The dependency hash is computed over the WHOLE transitive import
# closure, so a change to a module two hops away (main -> mid -> leaf)
# still changes the dependency hash of every consumer on the path. Both
# mid and main must rebuild even though neither's own bytes changed.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

cat > "$PROJ/leaf.kai" <<'EOF'
pub fn base() : Int = 10
EOF
cat > "$PROJ/mid.kai" <<'EOF'
import leaf
pub fn mid() : Int = leaf.base() + 5
EOF
cat > "$PROJ/main.kai" <<'EOF'
import mid
fn main() : Unit / Console { print(int_to_string(mid.mid())) }
EOF

out1="$(KAI_CACHE=1 "$KAI" run "$PROJ/main.kai" 2>/dev/null || true)"
if [ "$out1" != "15" ]; then
  echo "userb_edit_transitive FAIL — warm run did not print 15 (got '$out1')"
  exit 1
fi

# Edit the leaf, two hops below main. Neither mid.kai nor main.kai
# changed, but both have leaf in their transitive closure, so their
# dependency hashes change and both miss the cache. Expect 100 + 5 = 105.
cat > "$PROJ/leaf.kai" <<'EOF'
pub fn base() : Int = 100
EOF
out2="$(KAI_CACHE=1 "$KAI" run "$PROJ/main.kai" 2>/dev/null || true)"
if [ "$out2" != "105" ]; then
  echo "userb_edit_transitive FAIL — stale cache served '$out2', expected 105"
  exit 1
fi

echo "userb_edit_transitive OK — transitive-import edit cascaded the invalidation"
exit 0
