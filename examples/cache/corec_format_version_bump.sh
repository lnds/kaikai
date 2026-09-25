#!/bin/sh
# A blob written under an older payload layout is rejected by the KAB2
# header's format_version, never decoded. Every warm core blob is stamped
# back to format_version 11 — the hand-written codec's layout, which the
# derived codec does not read — and the next build must miss on each,
# re-parse, emit the same KIR as a cache-off build, and republish the
# blobs under the current version.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
STDLIB="$ROOT/stdlib"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

cat > "$PROJ/main.kai" <<'EOF'
fn main() : Unit / Console {
  let xs = [1, 2, 3]
  print("sum=#{int_to_string(list.sum(xs))}")
}
EOF

CCDIR="$PROJ/core-cache"
mkdir -p "$CCDIR"

build() {
  "$KAIC2" $EDITION_FLAG --emit=kir --path "$STDLIB" --path "$PROJ" \
    --core-cache-dir "$CCDIR" --toolchain-id fixture --core-cache-stats \
    "$PROJ/main.kai" > "$PROJ/$1.kir" 2> "$PROJ/$1.err"
}

"$KAIC2" $EDITION_FLAG --emit=kir --path "$STDLIB" --path "$PROJ" "$PROJ/main.kai" \
  > "$PROJ/oracle.kir" 2>/dev/null

build cold

version_at() { od -An -tu1 -j4 -N1 "$1" | tr -d ' '; }

n=0
for blob in "$CCDIR"/core-*.kab; do
  [ -f "$blob" ] || continue
  printf '\013\000\000\000' | dd of="$blob" bs=1 seek=4 count=4 conv=notrunc 2>/dev/null
  n=$((n + 1))
done
if [ "$n" = "0" ]; then
  echo "corec_format_version_bump FAIL — the cold run wrote no core blobs"
  exit 1
fi

build stale

if grep -q "core-parse-cache: hit" "$PROJ/stale.err"; then
  echo "corec_format_version_bump FAIL — a format_version 11 blob was served"
  cat "$PROJ/stale.err"
  exit 1
fi
if ! grep -q "core-parse-cache: miss" "$PROJ/stale.err"; then
  echo "corec_format_version_bump FAIL — the stale run reported no parse-layer miss"
  cat "$PROJ/stale.err"
  exit 1
fi
if ! cmp -s "$PROJ/stale.kir" "$PROJ/oracle.kir"; then
  echo "corec_format_version_bump FAIL — KIR after the stale blobs differs from the cache-off oracle"
  diff "$PROJ/oracle.kir" "$PROJ/stale.kir" | head -20
  exit 1
fi
for blob in "$CCDIR"/core-*.kab; do
  if [ "$(version_at "$blob")" = "11" ]; then
    echo "corec_format_version_bump FAIL — $blob was not republished under the current format_version"
    exit 1
  fi
done

echo "corec_format_version_bump OK — $n format_version 11 blobs rejected and republished"
