#!/bin/sh
# Usage: test-seq.sh c|native|asan
#
# The lazy sequence fixtures (examples/perceus/seq_*.kai) on one backend:
# each prints its golden, and the ones that read past the end, define a
# constant through its own value or read a cell from its own thunk panic
# with their message after printing what came before. `c` also drives
# the cell's states from C (stage2/tests/seq_cell_test.c); `asan` runs
# both under ASAN+UBSan.
set -u
cd "$(dirname "$0")/.."
mode=${1:-c}
FIX=examples/perceus
OUT=stage2/build
CC=${CC:-cc}
ASAN="-std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -DKAI_NO_CELL_POOL -Wno-unused-function -Wno-unused-variable"
mkdir -p "$OUT"

fail() { echo "seq FAIL ($mode): $1"; exit 1; }

# build <fixture> : the executable's path
build() {
  exe="$OUT/seq-$mode-$1"
  case "$mode" in
    native) KAI_BACKEND=native bin/kai build "$FIX/$1.kai" -o "$exe" 2> "$exe.err" ;;
    asan)   CFLAGS="$ASAN" KAI_BACKEND=c bin/kai build "$FIX/$1.kai" -o "$exe" 2> "$exe.err" ;;
    *)      KAI_BACKEND=c bin/kai build "$FIX/$1.kai" -o "$exe" 2> "$exe.err" ;;
  esac || { cat "$exe.err"; fail "$1 did not build"; }
}

run() {
  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" UBSAN_OPTIONS="halt_on_error=1" \
    KAI_THREADS=1 "$@"
}

clean() {
  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' "$1"; then
    cat "$1"; fail "sanitizer diagnostic in $2"
  fi
}

for f in seq_sieve seq_shared_once seq_pipes seq_defer_root_lambda seq_region_border seq_trap_in_fiber \
         seq_const_fibs seq_const_shared seq_const_per_fiber seq_filter_stages; do
  build "$f"
  run "$exe" > "$exe.out" 2> "$exe.err" || { cat "$exe.err"; fail "$f exited non-zero"; }
  clean "$exe.err" "$f"
  diff -u "$FIX/$f.out.expected" "$exe.out" || fail "$f output"
done

# panics <fixture> <message>
panics() {
  build "$1"
  if run "$exe" > "$exe.out" 2> "$exe.err"; then fail "$1 did not panic"; fi
  clean "$exe.err" "$1"
  grep -q "$2" "$exe.err" || { cat "$exe.err"; fail "$1 panic message"; }
  diff -u "$FIX/$1.out.expected" "$exe.out" || fail "$1 output"
}
panics seq_nth_out_of_range "seq.nth: index out of range"
panics seq_const_reads_itself "constant depends on itself"
panics seq_reads_itself "sequence depends on itself"
panics seq_filter_reads_itself "sequence depends on itself"

if [ "$mode" != native ]; then
  t="$OUT/seq-$mode-cell-test"
  flags="-std=c99 -O2 -g -Wall -Wno-unused-function -Wno-unused-variable"
  [ "$mode" = asan ] && flags="$ASAN"
  $CC $flags -I stage2 -I stage0 stage2/tests/seq_cell_test.c -o "$t" -lm 2> "$t.err" || { cat "$t.err"; fail "cell test did not build"; }
  run "$t" 2> "$t.err" || { cat "$t.err"; fail "cell states"; }
  clean "$t.err" "the cell test"
  if run "$t" self 2> "$t.err"; then fail "a cell read by its own thunk did not panic"; fi
  grep -q "sequence depends on itself" "$t.err" || { cat "$t.err"; fail "self-dependency panic message"; }
fi
echo "seq OK ($mode)"
