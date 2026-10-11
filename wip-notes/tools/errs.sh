#!/bin/sh
# errs.sh <repo-root> [file.kai ...]: type-check the stage-2 compiler of the checkout at <repo-root> and print
# the errors located in the named files (basenames), at most ERRS_MAX (default 150), then "<n> errors" for
# those files. With no file: the per-file error counts of the whole compiler and the total.
# With ONE file and ERRS_FROM=<fn> and/or ERRS_TO=<fn>: only the errors located from the definition of fn
# ERRS_FROM (inclusive) up to the definition of fn ERRS_TO (exclusive) in that file, as the file is now.
# ERRS_RAW=<path> also saves the checker's full output there. One run takes about a minute; at most 8 run
# at once across all agents (the others queue), so a call may wait a few minutes before it starts.
S=/private/tmp/claude-501/-Users-ediaz--herdr-worktrees-kaikai-symid-names-by-type/8228d0c8-8150-407f-935b-2dc3cdac79d2/scratchpad
K=$S/kaic2-dev
R=$(cd "$1" && pwd) || exit 2; shift
while [ -f $S/locks/HOLD ]; do sleep 15; done
slot=
while [ -z "$slot" ]; do
  for i in 1 2 3 4 5 6 7 8; do
    d=$S/locks/errs.$i
    if mkdir "$d" 2>/dev/null; then echo $$ > "$d/pid"; slot=$d; break; fi
    p=$(cat "$d/pid" 2>/dev/null)
    if [ -n "$p" ] && ! kill -0 "$p" 2>/dev/null; then rm -rf "$d"; fi
  done
  [ -z "$slot" ] && sleep 5
done
trap 'rm -rf "$slot"' EXIT INT TERM
out=$(cd "$R/stage2" && KAIKAI_STDLIB_PATH=../stdlib "$K" --check --edition "$(head -1 ../EDITION)" main.kai 2>&1)
rm -rf "$slot"
[ -n "$ERRS_RAW" ] && printf '%s\n' "$out" > "$ERRS_RAW"
printf '%s\n' "$out" | python3 $S/errs.py "$R" "${ERRS_MAX:-150}" "$@"
