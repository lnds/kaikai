#!/bin/sh
# The unused-import verdict of a file is read from the cache, not rescanned.
#
#   - cold: the scan runs and is published (one miss).
#   - warm: the same text and imports read it back (one hit), and the
#     diagnostics equal those of a build with no cache.
#   - an edit that uses the import changes the text: the scan runs again
#     and the warning is gone, cold and warm.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
PROJ="$(mktemp -d)"
CACHE="$PROJ/cache"
mkdir -p "$CACHE"
trap 'rm -rf "$PROJ"' EXIT INT TERM

fail() { echo "userb_import_scan_cache FAIL — $1"; exit 1; }

cat > "$PROJ/dep.kai" <<'EOF'
pub fn base() : Int = 10
EOF
cat > "$PROJ/other.kai" <<'EOF'
pub fn unrelated() : Int = 1
EOF
cat > "$PROJ/main.kai" <<'EOF'
import dep
import other
fn main() : Unit / Console { print(int_to_string(dep.base() + 5)) }
EOF

cached() {
  "$KAIC2" $EDITION_FLAG --path "$ROOT/stdlib" --path "$PROJ" --user-cache --user-cache-dir "$CACHE" \
    --core-cache-dir "$CACHE" --core-cache-stats "$PROJ/main.kai" 2>&1 >/dev/null | grep -v '^kaic2: core-' || true
}
plain() {
  "$KAIC2" $EDITION_FLAG --path "$ROOT/stdlib" --path "$PROJ" "$PROJ/main.kai" 2>&1 >/dev/null || true
}
verdict() { grep -v '^kaic2: import-scan-cache' || true; }

cold="$(cached)"
warm="$(cached)"
echo "$cold" | grep -q 'import-scan-cache: 0 hit, 1 miss' || fail "cold build did not miss once: $cold"
echo "$warm" | grep -q 'import-scan-cache: 1 hit, 0 miss' || fail "warm build did not hit once: $warm"
echo "$warm" | grep -q 'unused import `other`' || fail "warm build lost the unused-import warning"
[ "$(echo "$warm" | verdict)" = "$(plain)" ] || fail "warm diagnostics differ from a build with no cache"

cat > "$PROJ/main.kai" <<'EOF'
import dep
import other
fn main() : Unit / Console { print(int_to_string(dep.base() + other.unrelated())) }
EOF

edited="$(cached)"
again="$(cached)"
echo "$edited" | grep -q 'import-scan-cache: 0 hit, 1 miss' || fail "an edit that uses the import did not rescan: $edited"
echo "$again" | grep -q 'import-scan-cache: 1 hit, 0 miss' || fail "the edited file did not hit when unchanged: $again"
echo "$edited$again" | grep -q 'unused import' && fail "the used import is still reported unused"

echo "userb_import_scan_cache OK — the unused-import scan is read back and follows the text"
exit 0
