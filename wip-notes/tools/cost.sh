#!/bin/sh
# cost.sh <base-kaic2> <new-kaic2>: self-compile instructions and compile-alloc, alternated x3
R=/Users/ediaz/.herdr/worktrees/kaikai/symid-names-by-type
B=$1; N=$2
sh $R/tools/compile-alloc-gate.sh >/dev/null 2>&1
W=$R/stage2/build/compile-alloc
ed=$(head -1 $R/EDITION)
for i in 1 2 3; do
  for arm in base new; do
    if [ $arm = base ]; then K=$B; else K=$N; fi
    ins=$( (cd $R/stage2 && /usr/bin/time -l $K --edition $ed main.kai > /dev/null) 2>&1 | awk '/instructions retired/ {print $1}')
    cells=$( (cd $W && KAI_TRACE_RC=1 KAI_THREADS=1 $K main.kai >/dev/null) 2>&1 | grep -oE 'alloc_total=[0-9]+' | head -1 | cut -d= -f2)
    echo "$i $arm instructions=$ins cells=$cells"
  done
done
