#!/bin/sh
# tools/check-attribution-trailers.sh — refuse a pull request whose body or
# commits carry a Claude attribution trailer.
#
#   PR_BODY=<body> check-attribution-trailers.sh <base-sha> <head-sha>
#
# A squash merge builds its commit message from the PR title and body, so
# the body is the text that reaches main; the commits in base..head are
# checked too. Only a trailer at the start of a line counts, so prose that
# names one mid-sentence passes.

set -eu

[ $# -eq 2 ] || { echo "usage: PR_BODY=<body> $0 <base-sha> <head-sha>" >&2; exit 2; }
base="$1"
head="$2"
pattern='^[[:space:]]*(co-authored-by:[[:space:]]*claude|claude-session:)'
bad=0

hits="$(printf '%s\n' "${PR_BODY:-}" | tr -d '\r' | grep -inE "$pattern" || true)"
if [ -n "$hits" ]; then
  printf '%s\n' "$hits" | sed 's/^/attribution trailer in the PR body, line /'
  bad=1
fi

for c in $(git rev-list "$base..$head"); do
  hits="$(git log -1 --format=%B "$c" | grep -inE "$pattern" || true)"
  if [ -n "$hits" ]; then
    printf '%s\n' "$hits" | sed "s/^/attribution trailer in commit $(git rev-parse --short "$c"), line /"
    bad=1
  fi
done

if [ "$bad" -ne 0 ]; then
  echo "check-attribution-trailers FAIL — remove the line(s) above; a squash merge copies the PR body into main's history"
  exit 1
fi
echo "check-attribution-trailers OK — no Claude attribution trailer in the PR body or in $base..$head"
