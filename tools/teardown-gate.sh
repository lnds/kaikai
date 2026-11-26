#!/bin/sh
# Gate: a run of main that returns releases what the runtime allocated for it,
# at one thread and at four. Each fixture is built twice: with the teardown
# audit alone, and (where LeakSanitizer exists) with ASan and leak detection.
# Two builds against a runtime with one release removed must fail, so a gate
# that stopped looking is red too.
# Usage: tools/teardown-gate.sh <kaic2>
set -eu
cd "$(dirname "$0")/.."
. tools/lib/timeout.sh
kaic2=$1
cc=${CC:-cc}
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT

fixtures="examples/teardown/actors examples/teardown/reactor
  examples/actors/receive_timeout_race_message examples/ffi_userdata/round_trip
  examples/segments/generator examples/runtime/root_reaps_finished_children"

leaks=0
[ "$(uname -s)" = Linux ] && leaks=1
leaks=${KAI_TEARDOWN_LEAKS:-$leaks}
asan="-fsanitize=address -fno-omit-frame-pointer"

# build <fixture> <binary> [cc flags]
build() {
  src=$1
  bin=$2
  shift 2
  c="$dir/$(basename "$src").c"
  [ -f "$c" ] || "$kaic2" --path stdlib "$src.kai" > "$c"
  "$cc" -O1 -g -w -DKAI_TEARDOWN_AUDIT -DKAI_STDLIB_PATH="\"$PWD/stdlib\"" "$@" \
    -I stage2 -I stage0 "$c" -o "$bin" -lm -lpthread
}

# run <binary> <threads>
run() {
  KAI_THREADS=$2 ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=$leaks:symbolize=0" \
    kai_timeout 300 "$1" > "$dir/out" 2> "$dir/err"
}

check() {
  for src in $fixtures; do
    build "$src" "$dir/prog" "$@"
    for n in 1 4; do
      run "$dir/prog" "$n" || { echo "runtime-teardown FAIL $src (KAI_THREADS=$n $*)"; cat "$dir/err"; exit 1; }
      diff "$src.out.expected" "$dir/out" || { echo "runtime-teardown DIFF $src (KAI_THREADS=$n)"; exit 1; }
    done
  done
}

# must_fail <line of teardown.h to drop> <report expected> [cc flags]
must_fail() {
  line=$1
  report=$2
  shift 2
  mkdir -p "$dir/cut"
  cp -R stage2/runtime.h stage2/runtime "$dir/cut/"
  grep -vxF "$line" stage2/runtime/teardown.h > "$dir/cut/runtime/teardown.h"
  ! cmp -s stage2/runtime/teardown.h "$dir/cut/runtime/teardown.h" \
    || { echo "runtime-teardown FAIL: no line to drop: $line"; exit 1; }
  build examples/teardown/reactor "$dir/cut/prog" -I "$dir/cut" "$@"
  if run "$dir/cut/prog" 4 || ! grep -q "$report" "$dir/err"; then
    echo "runtime-teardown FAIL: dropping '$line' went unreported"
    cat "$dir/err"
    exit 1
  fi
}

check
must_fail '    kai_reactor_release();' 'a reactor buffer still held'
if [ "$leaks" -eq 1 ]; then
  # shellcheck disable=SC2086
  check $asan
  # shellcheck disable=SC2086
  must_fail '    free(kai_reactor_ready_batch);' 'LeakSanitizer' $asan
fi
echo "runtime-teardown OK (leak detection: $leaks)"
