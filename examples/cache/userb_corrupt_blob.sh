#!/bin/sh
# Phase B user-cache invalidation fixture #5 — corrupt cache blob.
#
# A blob truncated or scribbled on mid-write (disk error, SIGKILL,
# parallel writer) must never deserialise into wrong decls. The KAB2
# header validates magic/format/version, and the AST decoder rejects a
# payload that does not round-trip, so any corruption is a cache miss
# that triggers a clean re-parse — a torn write is self-correcting, not
# silently-wrong.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

cat > "$PROJ/dep.kai" <<'EOF'
pub fn base() : Int = 9
EOF
cat > "$PROJ/main.kai" <<'EOF'
import dep
fn main() : Unit / Console { print(int_to_string(dep.base())) }
EOF

out1="$(KAI_CACHE=1 "$KAI" run "$PROJ/main.kai" 2>/dev/null || true)"
if [ "$out1" != "9" ]; then
  echo "userb_corrupt_blob FAIL — warm run did not print 9 (got '$out1')"
  exit 1
fi

blob="$(ls "$PROJ/.kai-cache/"*.kab 2>/dev/null | head -1)"
if [ -z "$blob" ] || [ ! -f "$blob" ]; then
  echo "userb_corrupt_blob FAIL — no cache blob was written"
  exit 1
fi

# Truncate the blob to just past the 76-byte header. The header still
# validates (magic/format/version intact) but the AST payload is gone,
# so the decoder returns None and the loader falls back to a re-parse.
# This is the worst case: a corruption the header alone cannot catch.
head -c 80 "$blob" > "$blob.trunc"
mv "$blob.trunc" "$blob"

out2="$(KAI_CACHE=1 "$KAI" run "$PROJ/main.kai" 2>/dev/null || true)"
if [ "$out2" != "9" ]; then
  echo "userb_corrupt_blob FAIL — corrupt blob not rejected (got '$out2')"
  exit 1
fi

echo "userb_corrupt_blob OK — truncated payload rejected, fresh build ran"
exit 0
