#!/bin/sh
# A blob whose payload changed after it was written is a miss. The header
# carries a checksum of the payload; a byte flipped inside a declaration
# still decodes as a declaration list, so only the checksum can reject it.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM
export XDG_CACHE_HOME="$PROJ/xdg" KAI_CACHE_TRIM=0 KAI_BIN_MEMO=0 KAI_BACKEND=c

fail() { echo "userb_flipped_payload FAIL — $1"; exit 1; }

PKG="$PROJ/pkg"
mkdir -p "$PKG"
printf 'name = "fp"\n' > "$PKG/kai.toml"
printf 'pub fn alpha_marker() : Int = 7\n' > "$PKG/alpha.kai"
cat > "$PKG/main.kai" <<'EOF'
import alpha
fn main() : Unit / Console { print(int_to_string(alpha.alpha_marker())) }
EOF

run() { (cd "$PKG" && "$KAI" run main.kai 2>/dev/null) || true; }

[ "$(run)" = "7" ] || fail "cold build did not print 7"

bucket="$(ls -d "$XDG_CACHE_HOME"/kai/core/*/pkg/* 2>/dev/null | head -1)"
[ -n "$bucket" ] || fail "no package bucket in the shared cache"

flipped=0
for blob in "$bucket"/*.kab; do
  if grep -q alpha_marker "$blob"; then
    perl -pi -e 's/alpha_marker/alphb_marker/g' "$blob"
    flipped=$((flipped + 1))
  fi
done
[ "$flipped" -ge 1 ] || fail "no blob held the declaration"

[ "$(run)" = "7" ] || fail "a blob with a flipped payload byte was trusted"

echo "userb_flipped_payload OK — $flipped corrupted blob(s) missed, output unchanged"
exit 0
