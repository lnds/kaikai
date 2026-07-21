#!/usr/bin/env bash
# Turn tools/mn-corpus-shards.txt into the tier1-mn-corpus job matrix, after
# asserting the shards PARTITION the corpus: union == KAI_CORPUS_DEFAULT_DIRS
# and no directory in two shards.
#
# The assertion is the point. A matrix written out longhand in the workflow
# drifts the moment someone adds a corpus directory, and it drifts SILENTLY —
# the gate stays green over a smaller corpus, which is exactly the failure a
# coverage gate must not have. Deriving the matrix from the same file the
# check reads makes the drift impossible rather than merely unlikely.
#
# Prints `matrix=<json>` to $GITHUB_OUTPUT under CI, and the JSON to stdout
# always, so `tools/mn-corpus-plan.sh` is also the local way to see the split.

set -euo pipefail
cd "$(dirname "$0")/.."

. tools/lib/corpus.sh

SHARDS="${MN_CORPUS_SHARDS:-tools/mn-corpus-shards.txt}"
[ -f "$SHARDS" ] || { echo "mn-corpus-plan: missing $SHARDS" >&2; exit 1; }

declared=""
entries=""
while read -r name dirs; do
  case "$name" in ''|\#*) continue ;; esac
  [ -n "$dirs" ] || { echo "mn-corpus-plan: shard '$name' names no directory" >&2; exit 1; }
  declared="$declared $dirs"
  entries="$entries{\"name\":\"$name\",\"dirs\":\"$dirs\"},"
done < "$SHARDS"

dup="$(printf '%s\n' $declared | sort | uniq -d)"
if [ -n "$dup" ]; then
  echo "mn-corpus-plan: directory in more than one shard — a fixture would be measured twice:" >&2
  printf '    %s\n' $dup >&2
  exit 1
fi

union="$(printf '%s\n' $declared | sort -u)"
full="$(printf '%s\n' $KAI_CORPUS_DEFAULT_DIRS | sort -u)"
if [ "$union" != "$full" ]; then
  echo "mn-corpus-plan: shards do not cover the corpus in tools/lib/corpus.sh" >&2
  echo "  '<' only in the corpus (UNCOVERED), '>' only in the shards (stale):" >&2
  diff <(printf '%s\n' "$full") <(printf '%s\n' "$union") | sed 's/^/    /' >&2 || true
  exit 1
fi

matrix="{\"include\":[${entries%,}]}"
echo "$matrix"
[ -z "${GITHUB_OUTPUT:-}" ] || echo "matrix=$matrix" >> "$GITHUB_OUTPUT"
