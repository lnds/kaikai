#!/bin/sh
# bslot.sh <worktree> <out-kaic2>: build the worktree's compiler with the lane's current self-hosted compiler
# as host, at most 2 such builds at once across agents (others queue). Prints "built <out>" or "BUILD FAILED"
# with the first errors. A build takes 6-10 minutes.
S=/private/tmp/claude-501/-Users-ediaz--herdr-worktrees-kaikai-symid-names-by-type/8228d0c8-8150-407f-935b-2dc3cdac79d2/scratchpad
while [ -f $S/locks/HOLD ]; do sleep 15; done
slot=
while [ -z "$slot" ]; do
  for i in 1 2; do
    d=$S/locks/build.$i
    if mkdir "$d" 2>/dev/null; then echo $$ > "$d/pid"; slot=$d; break; fi
    p=$(cat "$d/pid" 2>/dev/null)
    if [ -n "$p" ] && ! kill -0 "$p" 2>/dev/null; then rm -rf "$d"; fi
  done
  [ -z "$slot" ] && sleep 10
done
trap 'rm -rf "$slot"' EXIT INT TERM
sh $S/buildwt.sh "$1" $S/kaic2-g2 "$2"
