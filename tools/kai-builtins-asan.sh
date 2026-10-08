#!/bin/bash
# Usage: kai-builtins-asan.sh c|native
#
# A declared builtin's args enter owned and its result leaves owned, as a
# `builtin_table` row's do. examples/ffi/extern_kai.kai runs under ASAN
# (and LeakSanitizer on Linux) twice: against the stdlib, and against a
# scratch core declaring each builtin it uses with `extern "kai"`, where
# the declaration shadows the row. Both runs must be clean and print the
# expected lines. On the C backend no call may bind the fn a declaration
# emits instead of the runtime symbol.
set -u
cd "$(dirname "$0")/.."
backend=${1:-c}
ROOT=$(pwd)
FIX=examples/ffi/extern_kai
LEAKS=0
[ "$(uname -s)" = Linux ] && LEAKS=1
CFLAGS_ASAN="-std=c99 -O1 -g -fsanitize=address -fno-omit-frame-pointer -DKAI_NO_CELL_POOL -Wno-unused-function -Wno-unused-variable"

DECLS='extern "kai"("kai_core_string_concat") pub fn string_concat(a: String, b: String) : String
extern "kai"("kai_core_list_reverse") pub fn list_reverse[T](xs: [T]) : [T]
extern "kai"("kai_core_vec_push") pub fn vec_push[T](v: Vec[T], x: T) : Vec[T]
extern "kai"("kai_core_array_set") pub fn array_set[T](a: Array[T], i: Int, x: T) : Array[T] / Mutable
extern "kai"("kai_core_print") pub fn print(s: String) : Unit / Stdout'

tmp=$(mktemp -d)
restore() {
  if [ "$backend" = native ]; then
    mv stage0/runtime_llvm.bc.hidkb stage0/runtime_llvm.bc 2>/dev/null || true
    mv stage0/runtime_inline.bc.hidkb stage0/runtime_inline.bc 2>/dev/null || true
  fi
  rm -rf "$tmp"
}
trap restore EXIT
cp -R stdlib "$tmp/stdlib"
printf '\n%s\n' "$DECLS" >> "$tmp/stdlib/effects/os.kai"

# The runtime bitcodes are uninstrumented, and the inline one would splice
# its hot paths past ASAN.
if [ "$backend" = native ]; then
  mv stage0/runtime_llvm.bc stage0/runtime_llvm.bc.hidkb 2>/dev/null || true
  mv stage0/runtime_inline.bc stage0/runtime_inline.bc.hidkb 2>/dev/null || true
fi

# run <label> <stdlib dir|"">
run() {
  local exe="$tmp/$1" env=()
  [ -n "$2" ] && env=(KAIKAI_STDLIB_PATH="$2" KAI_STDLIB="$2")
  if [ "$backend" = c ]; then
    env ${env[@]+"${env[@]}"} stage2/kaic2 --path "${2:-stdlib}" "$FIX.kai" > "$exe.c" 2> "$exe.err" \
      || { echo "kai-builtins-asan FAIL ($backend, $1): compile"; cat "$exe.err"; return 1; }
    cc $CFLAGS_ASAN -I stage2 -I stage0 "$exe.c" -o "$exe" -lm 2> "$exe.err" \
      || { echo "kai-builtins-asan FAIL ($backend, $1): cc"; cat "$exe.err"; return 1; }
  else
    env ${env[@]+"${env[@]}"} CFLAGS="$CFLAGS_ASAN" KAI_BACKEND=native bin/kai build "$FIX.kai" -o "$exe" 2> "$exe.err" \
      || { echo "kai-builtins-asan FAIL ($backend, $1): build"; cat "$exe.err"; return 1; }
  fi
  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=$LEAKS" KAI_THREADS=1 KAI_TRACE_RC=1 \
    "$exe" > "$exe.out" 2> "$exe.asan"
  local rc=$?
  # The RC ledger catches on macOS what LeakSanitizer catches on Linux.
  local alloc free
  alloc=$(grep -oE 'alloc_total=[0-9]+' "$exe.asan" | head -1 | cut -d= -f2)
  free=$(grep -oE 'free_total=[0-9]+' "$exe.asan" | head -1 | cut -d= -f2)
  [ -n "$alloc" ] && [ "$alloc" = "$free" ] \
    || { echo "kai-builtins-asan FAIL ($backend, $1): the RC ledger leaks (alloc_total=$alloc free_total=$free)"; return 1; }
  if grep -qE 'AddressSanitizer|LeakSanitizer|runtime error:' "$exe.asan"; then
    echo "kai-builtins-asan FAIL ($backend, $1): sanitizer diagnostic"; cat "$exe.asan"; return 1
  fi
  [ $rc -eq 0 ] || { echo "kai-builtins-asan FAIL ($backend, $1): exit $rc"; tail -20 "$exe.asan"; return 1; }
  diff -u "$FIX.out.expected" "$exe.out" || { echo "kai-builtins-asan FAIL ($backend, $1): output"; return 1; }
}

run rows "" || exit 1
run declared "$tmp/stdlib" || exit 1
# Every native entry the declared program calls exists, and each declaration
# emitted its fn: the run did compile against the declarations.
KAIKAI_STDLIB_PATH="$tmp/stdlib" stage2/kaic2 --path "$tmp/stdlib" --emit=kir "$FIX.kai" > "$tmp/declared.kir" \
  || { echo "kai-builtins-asan FAIL ($backend): --emit=kir"; exit 1; }
for name in string_concat list_reverse vec_push array_set print; do
  grep -q "^fn os__$name(" "$tmp/declared.kir" || { echo "kai-builtins-asan FAIL ($backend): no fn for the declaration of $name"; exit 1; }
done
for sym in $(grep -o 'kaix_core_[A-Za-z0-9_]*' "$tmp/declared.kir" | sort -u); do
  grep -q "^KaiValue \*$sym(" stage0/runtime_llvm.c || { echo "kai-builtins-asan FAIL ($backend): a call binds $sym, which the native runtime does not define"; exit 1; }
done

# A declaration's fn is reached only through its thunk: its prototype, its
# definition and the thunk's call name it, no call site does.
if [ "$backend" = c ]; then
  for name in string_concat list_reverse vec_push array_set print; do
    n=$(grep -o "kaiu_os__$name(" "$tmp/declared.c" | wc -l | tr -d ' ')
    [ "$n" -eq 3 ] || { echo "kai-builtins-asan FAIL (c): a call to $name reached its declaration's fn ($n uses), not the runtime symbol"; exit 1; }
  done
fi
echo "kai-builtins-asan OK ($backend, detect_leaks=$LEAKS: rows and core declarations alike)"
