#!/bin/sh
# merge.sh <lane-root> <commit-or-branch> [...]: merge each ref into the lane branch, in order.
# Stops at the first conflict, leaving the merge in progress and listing the conflicted files.
R=$(cd "$1" && pwd) || exit 2; shift
cd "$R" || exit 2
for b in "$@"; do
  if git merge-base --is-ancestor "$b" HEAD 2>/dev/null; then echo "skip $b (already merged)"; continue; fi
  if git merge --no-edit -q "$b" >/dev/null 2>&1; then
    echo "ok   $b"
  else
    echo "CONFLICT $b:"; git diff --name-only --diff-filter=U | sed 's/^/  /'; exit 1
  fi
done
git log --oneline -1 | cut -c1-80
