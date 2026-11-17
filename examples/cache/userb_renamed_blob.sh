#!/bin/sh
# A cache blob copied onto another module's file name is a miss. The header
# echoes the key the blob was written under; a reader compares it with the key
# it looked up, so decls cached for one module are never compiled as another's.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM
export XDG_CACHE_HOME="$PROJ/xdg" KAI_CACHE_TRIM=0 KAI_BIN_MEMO=0 KAI_BACKEND=c

fail() { echo "userb_renamed_blob FAIL — $1"; exit 1; }

PKG="$PROJ/pkg"
mkdir -p "$PKG"
printf 'name = "rb"\n' > "$PKG/kai.toml"
printf 'pub fn alpha_marker() : Int = 1\n' > "$PKG/alpha.kai"
printf 'pub fn beta_marker() : Int = 2\n' > "$PKG/beta.kai"
cat > "$PKG/main.kai" <<'EOF'
import alpha
import beta
fn main() : Unit / Console { print(int_to_string(alpha.alpha_marker() * 10 + beta.beta_marker())) }
EOF

run() { (cd "$PKG" && "$KAI" run main.kai 2>/dev/null) || true; }

[ "$(run)" = "12" ] || fail "cold build did not print 12"

bucket="$(ls -d "$XDG_CACHE_HOME"/kai/core/*/pkg/* 2>/dev/null | head -1)"
[ -n "$bucket" ] || fail "no package bucket in the shared cache"

copied=0
for kind in '*-0000000000000000.kab' 'tm-*.kab'; do
  a="$(grep -l alpha_marker "$bucket"/$kind 2>/dev/null | head -1 || true)"
  b="$(grep -l beta_marker "$bucket"/$kind 2>/dev/null | head -1 || true)"
  if [ -n "$a" ] && [ -n "$b" ] && [ "$a" != "$b" ]; then
    cp "$a" "$b"
    copied=$((copied + 1))
  fi
done
[ "$copied" -ge 1 ] || fail "found no blob pair to swap"

[ "$(run)" = "12" ] || fail "a blob renamed onto another key was trusted"

echo "userb_renamed_blob OK — $copied renamed blob kind(s) missed, output unchanged"
exit 0
