#!/bin/sh
# `main` runs on one fixed stack budget: a recursion within it completes and
# one past it gets the guard-page diagnostic, at every thread count and
# whatever the host stack rlimit says. Usage: test-main-stack.sh <c|native>

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
backend="${1:?usage: test-main-stack.sh <c|native>}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM
fx="$ROOT/examples/runtime"

for name in main_stack_sized main_stack_sized_overflow; do
  "$ROOT/bin/kai" build --backend="$backend" "$fx/$name.kai" -o "$work/$name" 2> "$work/build.err" \
    || { echo "main-stack FAIL ($backend: build of $name failed)"; cat "$work/build.err"; exit 1; }
done

for n in 1 8; do
  for limit in 1024 "$(ulimit -H -s)"; do
    out="$(ulimit -s "$limit"; KAI_THREADS=$n "$work/main_stack_sized" 2>&1)" \
      || { echo "main-stack FAIL ($backend, KAI_THREADS=$n, ulimit -s $limit: deep recursion overflowed)"; echo "$out"; exit 1; }
    [ "$out" = "$(cat "$fx/main_stack_sized.out.expected")" ] \
      || { echo "main-stack DIFF ($backend, KAI_THREADS=$n, ulimit -s $limit)"; echo "$out"; exit 1; }
    rc=0
    (ulimit -s "$limit"; KAI_THREADS=$n "$work/main_stack_sized_overflow" > /dev/null 2> "$work/overflow.err") 2> /dev/null || rc=$?
    [ "$rc" -ne 0 ] && grep -q "fiber stack overflow" "$work/overflow.err" \
      || { echo "main-stack FAIL ($backend, KAI_THREADS=$n, ulimit -s $limit: overflow not diagnosed, exit=$rc)"; cat "$work/overflow.err"; exit 1; }
  done
done
echo "main-stack OK ($backend: 50K deep completes, 3M deep diagnosed, at 1 and 8 threads)"
