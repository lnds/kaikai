#!/bin/sh
# buildwt.sh <worktree> <host-kaic2> <out-kaic2>: fast-build the worktree's compiler with the host one, holding a build slot.
S=/private/tmp/claude-501/-Users-ediaz--herdr-worktrees-kaikai-symid-names-by-type/8228d0c8-8150-407f-935b-2dc3cdac79d2/scratchpad
W=$(cd "$1" && pwd) || exit 2
cp $2 $W/stage2/kaic2
[ -x $W/bin/kai ] || ( cd $W && make -s -C tools/kai kai > $W/kai-build.log 2>&1 )
( cd $W && make -C stage2 kaic2-fast > $W/fast-build.log 2>&1 )
grep -q "kaic2-fast OK" $W/fast-build.log || { echo "BUILD FAILED"; grep -aE "^error|error:|FAIL" $W/fast-build.log | head -10 | cut -c1-240; exit 1; }
cp $W/stage2/kaic2 $3 && echo "built $3"
