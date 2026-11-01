#!/bin/sh
# tests/fmt_tree.sh — every tracked .kai under the formatted trees is
# already what `kai fmt` prints.
#
# Trees: stdlib, stage2/compiler, tools/kai, examples, demos.
#
# tests/fmt_tree_exempt.txt lists sources left as written: files `fmt`
# refuses (negative and legacy-syntax fixtures) and fixtures whose
# diagnostic quotes the source spelling. An exempt file that is already
# formatted is a stale entry and fails.
#
# `kaic2 --fmt` prints to stdout; sources are never touched.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
KAIC2="$ROOT/stage2/kaic2"
EXEMPT=tests/fmt_tree_exempt.txt

if [ ! -x "$KAIC2" ]; then
  echo "fmt_tree: $KAIC2 not built; run 'make kaic2' first" >&2
  exit 2
fi

is_exempt() {
  grep -qxF -- "$1" "$EXEMPT"
}

# Worker: classify the files given as arguments, one verdict line per problem.
if [ "${1:-}" = "--files" ]; then
  shift
  out="$(mktemp)"
  trap 'rm -f "$out"' EXIT INT TERM
  for rel in "$@"; do
    if "$KAIC2" --edition "$(cat EDITION)" --fmt "$rel" > "$out" 2> /dev/null; then
      if cmp -s "$rel" "$out"; then
        ! is_exempt "$rel" || echo "STALE exempt entry (already formatted): $rel"
      else
        is_exempt "$rel" || echo "UNFORMATTED $rel"
      fi
    else
      is_exempt "$rel" || echo "REFUSED $rel"
    fi
  done
  exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

git ls-files -- stdlib stage2/compiler tools/kai examples demos | grep '\.kai$' > "$tmp/files"
n="$(wc -l < "$tmp/files" | tr -d ' ')"

xargs -n 100 -P "${FMT_TREE_JOBS:-4}" sh "$0" --files < "$tmp/files" > "$tmp/verdicts"

for rel in $(grep -v '^#' "$EXEMPT"); do
  [ -f "$rel" ] || echo "MISSING exempt entry: $rel" >> "$tmp/verdicts"
done

if [ -s "$tmp/verdicts" ]; then
  sort "$tmp/verdicts" | sed 's/^/  /'
  echo "fmt_tree: FAIL ($(wc -l < "$tmp/verdicts" | tr -d ' ') of $n files; run 'kai fmt <file>' on the UNFORMATTED ones)"
  exit 1
fi
echo "fmt_tree: OK ($n files, $(grep -vc '^#' "$EXEMPT") exempt)"
