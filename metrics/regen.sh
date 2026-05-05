#!/bin/sh
# Regenerate metrics/build-history.tsv from all docs/lane-experience-*.md
# retros. Run from repo root after each lane merge.
#
# Usage: ./metrics/regen.sh

set -e

cd "$(dirname "$0")/.."

OUT=metrics/build-history.tsv

{
  printf 'lane\ttimestamp\tcmd\toutcome\telapsed_s\n'
  for f in docs/lane-experience-*.md; do
    [ -f "$f" ] || continue
    lane=$(basename "$f" .md | sed 's/^lane-experience-//')
    awk -v lane="$lane" '
      /^## Build.*TSV/ { flag = 1; next }
      flag && /^## / { flag = 0 }
      flag && /^[0-9]{4}-[0-9]{2}-[0-9]{2}T/ { print lane "\t" $0 }
    ' "$f"
  done
} > "$OUT"

n=$(wc -l < "$OUT" | tr -d ' ')
echo "wrote $OUT ($n lines including header)"
