#!/bin/sh
# A `.kai-cache/` that arrives with a package is never read. The package
# cache lives in the shared user cache, bucketed by the package's absolute
# path, so blobs planted in the checkout cannot stand in for the sources.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM
export KAI_CACHE_TRIM=0 KAI_BIN_MEMO=0 KAI_BACKEND=c

fail() { echo "userb_planted_kai_cache FAIL — $1"; exit 1; }

PKG="$PROJ/pkg"
mkdir -p "$PKG"
printf 'name = "pk"\n' > "$PKG/kai.toml"
cat > "$PKG/main.kai" <<'EOF'
import alpha
fn main() : Unit / Console { print(int_to_string(alpha.alpha_marker())) }
EOF
tid="$("$KAI" env KAI_TOOLCHAIN_ID)"

write_alpha() { printf 'pub fn alpha_marker() : Int = %s\n' "$1" > "$PKG/alpha.kai"; }
run() { (cd "$PKG" && XDG_CACHE_HOME="$PROJ/xdg$1" "$KAI" run main.kai 2>/dev/null) || true; }
bucket() { ls -d "$PROJ/xdg$1"/kai/core/*/pkg/* 2>/dev/null | head -1; }

write_alpha 1
[ "$(run 1)" = "1" ] || fail "first version did not print 1"
write_alpha 5
[ "$(run 2)" = "5" ] || fail "second version did not print 5"

b1="$(bucket 1)"
b2="$(bucket 2)"
[ -n "$b1" ] && [ -n "$b2" ] || fail "no package bucket in the shared cache"

planted="$PKG/.kai-cache/$tid"
mkdir -p "$planted"
n=0
for kind in '*-0000000000000000.kab' 'tm-*.kab'; do
  old="$(grep -l alpha_marker "$b1"/$kind 2>/dev/null | head -1 || true)"
  new="$(grep -l alpha_marker "$b2"/$kind 2>/dev/null | head -1 || true)"
  if [ -n "$old" ] && [ -n "$new" ]; then
    cp "$old" "$planted/$(basename "$new")"
    n=$((n + 1))
  fi
done
[ "$n" -ge 1 ] || fail "found no blob to plant"
before="$(cksum "$planted"/* | sort)"

[ "$(run 3)" = "5" ] || fail "blobs planted in the package's .kai-cache were used"
[ "$(cksum "$planted"/* | sort)" = "$before" ] || fail "the package's .kai-cache was modified"
[ "$(ls "$planted" | wc -l | tr -d ' ')" = "$n" ] || fail "the package's .kai-cache gained entries"

echo "userb_planted_kai_cache OK — $n planted blob(s) ignored, output follows the sources"
exit 0
