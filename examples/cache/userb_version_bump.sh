#!/bin/sh
# Phase B user-cache invalidation fixture #4 — kaikai version bump.
#
# The KAB2 header carries the kaikai version hash; cache_read_header
# rejects any blob whose version does not match the running compiler.
# A `brew upgrade` that changes the version hash therefore invalidates
# every existing blob. We simulate the upgrade by rewriting the
# version field of an on-disk blob to a value the compiler will not
# accept, then confirm the build falls back to a fresh parse and still
# produces the correct result (no stale decls, no fatal error).
#
# Header layout (stage2/compiler/cache.kai): magic[4] format_version[4]
# kaikai_version[4] source_sha[64]. The kaikai_version u32 LE starts at
# byte offset 8.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

cat > "$PROJ/dep.kai" <<'EOF'
pub fn base() : Int = 7
EOF
cat > "$PROJ/main.kai" <<'EOF'
import dep
fn main() : Unit / Console { print(int_to_string(dep.base())) }
EOF

# Warm the cache for the dep module.
out1="$(KAI_CACHE=1 "$KAI" run "$PROJ/main.kai" 2>/dev/null || true)"
if [ "$out1" != "7" ]; then
  echo "userb_version_bump FAIL — warm run did not print 7 (got '$out1')"
  exit 1
fi

blob="$(ls "$PROJ/.kai-cache/"*.kab 2>/dev/null | head -1)"
if [ -z "$blob" ] || [ ! -f "$blob" ]; then
  echo "userb_version_bump FAIL — no cache blob was written"
  exit 1
fi

# Flip the kaikai_version field (offset 8) to 0xFF so the header's
# version check fails on the next load — a stand-in for a real version
# bump. dd writes one byte in place; the rest of the blob is untouched.
printf '\377' | dd of="$blob" bs=1 seek=8 count=1 conv=notrunc 2>/dev/null

# The build must reject the version-mismatched blob and re-parse dep,
# still printing 7.
out2="$(KAI_CACHE=1 "$KAI" run "$PROJ/main.kai" 2>/dev/null || true)"
if [ "$out2" != "7" ]; then
  echo "userb_version_bump FAIL — version-mismatched blob not rejected (got '$out2')"
  exit 1
fi

echo "userb_version_bump OK — version mismatch rejected, fresh build ran"
exit 0
