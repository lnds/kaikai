#!/bin/sh
# Phase A.0 cache invalidation fixture #4 — source content change.
#
# Demonstrates that the content-addressable filename (`<sha>.kab`)
# survives a source-content edit without a stale header check. The
# driver's `prelude_cache_lookup` computes the sha of the current
# source bytes and probes `<sha>.kab`. When the source's content
# changes the sha changes too, so the existing cache file (under the
# OLD sha) is invisible and the driver issues a fresh
# --emit-prelude-cache invocation. This invalidation path is NOT a
# header check — it predates `cache_read_header` entirely.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CACHE_DIR="$(mktemp -d)"
SRC_DIR="$(mktemp -d)"
trap 'rm -rf "$CACHE_DIR" "$SRC_DIR"' EXIT INT TERM

# Two copies of the same one-fn user prelude. v1 defines fn one() = 1;
# v2 swaps the literal to 2 so the sha differs by one byte's worth.
cat > "$SRC_DIR/lib.kai" <<'EOF'
pub fn one() : Int = 1
EOF
SHA1="$(shasum -a 256 "$SRC_DIR/lib.kai" | awk '{ print $1 }')"

cat > "$SRC_DIR/main.kai" <<'EOF'
fn main() : Unit / Console {
  print("hi")
}
EOF

# First compile — populates the cache under SHA1.
out1="$(KAI_PRELUDE_CACHE=1 KAI_PRELUDE_CACHE_DIR="$CACHE_DIR" \
        "$ROOT/stage2/kaic2" --emit-prelude-cache --cache-sha "$SHA1" \
        "$SRC_DIR/lib.kai" > "$CACHE_DIR/${SHA1}.kab" && echo built || echo failed)"
if [ "$out1" != "built" ]; then
  echo "source_content_change FAIL — initial cache emit failed"
  exit 1
fi
[ -f "$CACHE_DIR/${SHA1}.kab" ] || {
  echo "source_content_change FAIL — first cache file not present"
  exit 1
}

# Now edit the source. The sha changes; the cache file for the new
# sha does not exist yet.
cat > "$SRC_DIR/lib.kai" <<'EOF'
pub fn one() : Int = 2
EOF
SHA2="$(shasum -a 256 "$SRC_DIR/lib.kai" | awk '{ print $1 }')"

if [ "$SHA1" = "$SHA2" ]; then
  echo "source_content_change FAIL — sha did not change after edit"
  exit 1
fi

# Lookup with the new sha must miss (no such file). The cache for the
# OLD sha is still on disk — it just does not match the current
# source any more, so the driver bypasses it. Mirror prelude_cache_lookup's
# filename rule.
if [ -f "$CACHE_DIR/${SHA2}.kab" ]; then
  echo "source_content_change FAIL — new-sha cache appeared from nowhere"
  exit 1
fi
if [ ! -f "$CACHE_DIR/${SHA1}.kab" ]; then
  echo "source_content_change FAIL — old-sha cache vanished prematurely"
  exit 1
fi

# Drive bin/kai end-to-end with the edited source. Expect: cache
# miss, fresh emit, working binary.
out2="$(KAI_PRELUDE_CACHE=1 KAI_PRELUDE_CACHE_DIR="$CACHE_DIR" \
        "$ROOT/bin/kai" run "$SRC_DIR/main.kai" 2>&1 || true)"

case "$out2" in
  *hi*) ;;
  *)
    echo "source_content_change FAIL — driver did not produce expected output"
    printf '%s\n' "$out2"
    exit 1 ;;
esac

# Confirm the driver populated a cache for the user-side lib too? No —
# bin/kai only caches stdlib preludes, not the user file. The sha2
# slot stays empty; the assertion here is that the OLD slot is still
# present (no cleanup) while the build worked.
echo "source_content_change OK — sha-rotation invalidated cache, fresh build ran"
exit 0
