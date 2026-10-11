#!/bin/sh
# ciwatch.sh <pr>: poll until every check of the PR's head commit has finished; print the summary.
PR=$1
while :; do
  out=$(gh pr checks "$PR" 2>/dev/null)
  pend=$(printf '%s\n' "$out" | awk -F'\t' '$2=="pending"||$2=="queued"||$2=="in_progress"' | wc -l | tr -d ' ')
  total=$(printf '%s\n' "$out" | grep -c .)
  if [ "$total" -gt 0 ] && [ "$pend" = 0 ]; then break; fi
  sleep 120
done
TZ=America/Santiago date "+done: %H:%M %Z"
printf '%s\n' "$out" | awk -F'\t' '{print $2"\t"$1"\t"$3}' | sort | cut -c1-90
