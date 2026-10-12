#!/bin/bash
# Per-program cost split: kaic2 (no cache / cold / warm), cc compile, link, run. Wall x3 + callgrind Ir x3.
set -u
P=$1; cd /home/user/kaikai/stage2
K=(./kaic2 --edition hanga-roa)
F=(-std=c99 -Wall -Wno-unused-function -Wno-unused-variable -g -O2 -I . '-DKAI_STDLIB_PATH="/home/user/kaikai/stdlib"' -I ../stage0)
OUT=$P/raw.tsv; : > $OUT
wall(){ local s e; s=$(date +%s%N); "$@" > /dev/null; e=$(date +%s%N); echo $(( (e-s)/1000 )); }
ir(){ local tag=$1; shift; valgrind --tool=callgrind --trace-children=yes --callgrind-out-file=$P/cg/$tag.%p.out "$@" > /dev/null 2> $P/cg/$tag.log; grep -h "Collected" $P/cg/$tag.log | awk '{s+=$4} END{print s}'; }
mkdir -p $P/cg $P/ccw
for prog in hello effects_fx perceus_fx; do
  src=$P/$prog.kai
  "${K[@]}" --core-cache-dir $P/ccw $src > $P/$prog.c
  for i in 1 2 3; do
    rm -rf $P/ccc; mkdir $P/ccc
    echo -e "$prog\tkaic2_nocache\twall_us\t$i\t$(wall "${K[@]}" $src)" >> $OUT
    echo -e "$prog\tkaic2_cold\twall_us\t$i\t$(wall "${K[@]}" --core-cache-dir $P/ccc $src)" >> $OUT
    echo -e "$prog\tkaic2_warm\twall_us\t$i\t$(wall "${K[@]}" --core-cache-dir $P/ccw $src)" >> $OUT
    echo -e "$prog\tcc_compile_link\twall_us\t$i\t$(wall clang "${F[@]}" $P/$prog.c -o $P/$prog.bin -lm)" >> $OUT
    echo -e "$prog\tcc_compile\twall_us\t$i\t$(wall clang "${F[@]}" -c $P/$prog.c -o $P/$prog.o)" >> $OUT
    echo -e "$prog\tlink\twall_us\t$i\t$(wall clang $P/$prog.o -o $P/$prog.bin2 -lm)" >> $OUT
    echo -e "$prog\trun\twall_us\t$i\t$(wall $P/$prog.bin)" >> $OUT
  done
  for i in 1 2 3; do
    rm -rf $P/ccc; mkdir $P/ccc
    echo -e "$prog\tkaic2_nocache\tIr\t$i\t$(ir $prog-nocache-$i "${K[@]}" $src)" >> $OUT
    echo -e "$prog\tkaic2_cold\tIr\t$i\t$(ir $prog-cold-$i "${K[@]}" --core-cache-dir $P/ccc $src)" >> $OUT
    echo -e "$prog\tkaic2_warm\tIr\t$i\t$(ir $prog-warm-$i "${K[@]}" --core-cache-dir $P/ccw $src)" >> $OUT
    echo -e "$prog\tcc_compile\tIr\t$i\t$(ir $prog-cc-$i clang "${F[@]}" -c $P/$prog.c -o $P/$prog.o)" >> $OUT
    echo -e "$prog\tlink\tIr\t$i\t$(ir $prog-link-$i clang $P/$prog.o -o $P/$prog.bin2 -lm)" >> $OUT
    echo -e "$prog\trun\tIr\t$i\t$(ir $prog-run-$i $P/$prog.bin)" >> $OUT
  done
done
echo DONE
