#!/bin/bash
# Run one tier1 light slice under the logging shims; log every kaic2 / cc / binary run.
S=$(dirname "$(readlink -f "$0")")
shard=$1; shift
export KPERF_REAL_KAIC1=$S/kaic1.real KPERF_REAL_KAIC2=$S/kaic2.real KPERF_REAL_CC=clang CC=$S/cc
unset KPERF_INFO; export KPERF_LOG=$S/shard-$shard.log
: > "$KPERF_LOG"
cd /home/user/kaikai; rm -rf ~/.cache/kai stage2/build/core-cache
s=$(date +%s%N)
make -C stage2 $shard TIER1_SELFHOSTS=0 KAI_LLVM=0 CC=$S/cc "$@" > $S/shard-$shard.out 2>&1
rc=$?
e=$(date +%s%N)
echo "shard $shard rc=$rc elapsed_ns=$((e-s))" >> $S/shard-$shard.out
times >> $S/shard-$shard.out
echo "shard $shard rc=$rc elapsed_s=$(( (e-s)/1000000000 ))"
