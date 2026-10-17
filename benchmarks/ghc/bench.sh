#!/bin/bash
# bench.sh <kai-binary>: compile and run CPU time (user+sys, children included), kaikai vs GHC, best of N.
set -u
KAI="$1"; cd "$(dirname "$0")"
export KAI_BIN_MEMO=0
N=${N:-5}
best() { # best user+sys seconds of N runs of "$@"
  local b=999999 t
  for _ in $(seq 1 $N); do
    t=$(python3 -I "$PWD/cpu.py" "$@")
    b=$(awk -v a="$b" -v c="$t" 'BEGIN{print (c<a)?c:a}')
  done; echo "$b"
}
ghcc() { echo "rm -rf o-$1-$3 && ghc $2 -outputdir o-$1-$3 -o bin/$1-ghc$3 $1.hs"; }
mkdir -p bin
printf '| program | ghc -O2 compile | kai build compile | ghc -O0 compile | kai O1 compile | ghc -O2 run | kai build run |\n|---|---:|---:|---:|---:|---:|---:|\n'
for p in hello fib lists strings rbtree; do
  c_g2=$(best sh -c "$(ghcc $p -O2 2)"); c_g0=$(best sh -c "$(ghcc $p -O0 0)")
  "$KAI" build $p.kai -o bin/$p-kai >/dev/null 2>&1
  c_k2=$(best "$KAI" build $p.kai -o bin/$p-kai)
  c_k1=$(KAI_NATIVE_OPT=1 KAI_C_OPT=1 best "$KAI" build $p.kai -o bin/$p-kai1)
  r_g=$(best bin/$p-ghc2); r_k=$(best bin/$p-kai)
  [ "$(bin/$p-ghc2)" = "$(bin/$p-kai)" ] || echo "OUTPUT DIFFERS: $p ($(bin/$p-ghc2) vs $(bin/$p-kai))" >&2
  echo "| $p | $c_g2 | $c_k2 | $c_g0 | $c_k1 | $r_g | $r_k |"
done
