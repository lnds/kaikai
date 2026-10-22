#!/bin/sh
# A program's C reaches the libLLVM bridge and the bench and check runtime
# only when it calls them: a plain program is gated off both, a program that
# benches or checks, and the compiler's own units, are not.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

emit() { "$ROOT/stage2/kaic2" --path "$ROOT/stdlib" "$@"; }
fail=0
expect() {
  if ! eval "$2"; then
    echo "  FAIL $1"
    fail=$((fail + 1))
  fi
}

printf 'fn main() : Unit / Stdout = Stdout.print("hi")\n' > "$work/plain.kai"
emit "$work/plain.kai" > "$work/plain.c"
expect "plain: bridge gated off" "grep -q '^#define KAI_NO_LLVM_BRIDGE 1' $work/plain.c"
expect "plain: bench and check gated off" "grep -q '^#define KAI_NO_BENCH_CHECK 1' $work/plain.c"
cc -E -P -I "$ROOT/stage2" -I "$ROOT/stage0" "$work/plain.c" > "$work/plain.i"
expect "plain: no bridge stub in the unit" "! grep -q kai_llvm_native_unavailable $work/plain.i"
expect "plain: no bench runtime in the unit" "! grep -q kai_bench_run_one $work/plain.i"
expect "plain: no check runtime in the unit" "! grep -q kai_check_begin $work/plain.i"
expect "plain: assert still reaches the test runtime" "grep -q kai_assert_check $work/plain.i"

emit "$ROOT/examples/stdlib/bench_basic.kai" > "$work/bench.c"
expect "bench: bench runtime kept" "! grep -q '^#define KAI_NO_BENCH_CHECK' $work/bench.c"

emit "$ROOT/examples/stdlib/check_basic.kai" > "$work/check.c"
expect "check: check runtime kept" "! grep -q '^#define KAI_NO_BENCH_CHECK' $work/check.c"

# Every name harness_extra.h defines is covered by a needle of the emitter's gate.
needles="$(grep -o 'any(\[[^]]*\]' "$ROOT/stage2/compiler/emit_cache.kai" | grep -o '"kai_[a-z_]*"' | tr -d '"')"
for n in $(grep -E '^(static|KAI_[A-Z_]+) ' "$ROOT/stage2/runtime/harness_extra.h" | grep -v ';$' | grep -oE 'kai_[a-z_0-9]+\(' | tr -d '(' | sort -u); do
  covered=0
  for p in $needles; do
    case "$n" in "$p"*) covered=1 ;; esac
  done
  expect "gate covers $n" "[ $covered = 1 ]"
done

[ "$fail" -eq 0 ] && echo "runtime gates: ok"
exit "$fail"
