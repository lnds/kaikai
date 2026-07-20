#!/bin/sh
# Issue #825 core-cache invalidation fixture — corrupt core blob.
#
# A core blob truncated or scribbled on mid-write (disk error, SIGKILL,
# parallel writer) must never deserialise into wrong core decls — that
# would typecheck every program against a mangled stdlib. The KAB2
# header validates magic/format/version, and the AST decoder rejects a
# payload that does not round-trip, so any corruption is a cache miss
# that triggers a clean re-parse of that core module. A torn write is
# self-correcting, not silently-wrong.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

cat > "$PROJ/main.kai" <<'EOF'
fn main() : Unit / Console = print(int_to_string(list.sum([7, 8])))
EOF

out1="$(KAI_CACHE=1 KAI_CORE_CACHE=0 "$KAI" run "$PROJ/main.kai" 2>/dev/null || true)"
if [ "$out1" != "15" ]; then
  echo "corec_corrupt_blob FAIL — warm run did not print 15 (got '$out1')"
  exit 1
fi

# Truncate the largest core blob to just past the 76-byte header. The
# header still validates but the AST payload is gone, so the decoder
# returns None and the loader re-parses that module. The worst case: a
# corruption the header alone cannot catch.
blob="$(ls -S "$PROJ/.kai-cache/core-"*.kab 2>/dev/null | head -1)"
if [ -z "$blob" ] || [ ! -f "$blob" ]; then
  echo "corec_corrupt_blob FAIL — no core blob was written"
  exit 1
fi
head -c 80 "$blob" > "$blob.trunc"
mv "$blob.trunc" "$blob"

out2="$(KAI_CACHE=1 KAI_CORE_CACHE=0 "$KAI" run "$PROJ/main.kai" 2>/dev/null || true)"
if [ "$out2" != "15" ]; then
  echo "corec_corrupt_blob FAIL — truncated core blob not rejected (got '$out2')"
  exit 1
fi

echo "corec_corrupt_blob OK — truncated payload rejected, fresh core parse ran"
exit 0
