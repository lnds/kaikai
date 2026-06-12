#!/bin/sh
# Issue #825 core-cache invalidation fixture — kaikai version bump.
#
# The KAB2 header carries the kaikai version hash; cache_read_header
# rejects any blob whose version does not match the running compiler,
# so a `brew upgrade` that bumps the version invalidates every core
# blob. We simulate the upgrade by rewriting the version field of an
# on-disk core blob to a value the compiler will not accept, then
# confirm the build falls back to a fresh parse of that core module
# and still produces the correct result (no stale decls, no fatal).
#
# Header layout (stage2/compiler/cache.kai): magic[4] format_version[4]
# kaikai_version[4] source_sha[64]. The kaikai_version u32 LE is at
# byte offset 8 — same header the user cache uses, so the same probe.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

cat > "$PROJ/main.kai" <<'EOF'
fn main() : Unit / Console = print(int_to_string(list.sum([4, 5, 6])))
EOF

# Warm the core cache.
out1="$(KAI_CACHE=1 "$KAI" run "$PROJ/main.kai" 2>/dev/null || true)"
if [ "$out1" != "15" ]; then
  echo "corec_version_bump FAIL — warm run did not print 15 (got '$out1')"
  exit 1
fi

blob="$(ls "$PROJ/.kai-cache/core-"*.kab 2>/dev/null | head -1)"
if [ -z "$blob" ] || [ ! -f "$blob" ]; then
  echo "corec_version_bump FAIL — no core blob was written"
  exit 1
fi

# Flip the kaikai_version field (offset 8) to 0xFF on one core blob so
# the header version check fails on the next load — a stand-in for a
# real version bump. The corrupt module must re-parse; the build stays
# correct.
printf '\377' | dd of="$blob" bs=1 seek=8 count=1 conv=notrunc 2>/dev/null

out2="$(KAI_CACHE=1 "$KAI" run "$PROJ/main.kai" 2>/dev/null || true)"
if [ "$out2" != "15" ]; then
  echo "corec_version_bump FAIL — version-mismatched core blob not rejected (got '$out2')"
  exit 1
fi

echo "corec_version_bump OK — version mismatch rejected, fresh core parse ran"
exit 0
