#!/bin/sh
# A spawned fiber and a handle that keeps its continuation share one stack
# budget: a recursion within it completes and one past it gets the guard-page
# diagnostic, at every thread count. Usage: test-fiber-stack.sh <c|native>

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
backend="${1:?usage: test-fiber-stack.sh <c|native>}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM
fx="$ROOT/examples/runtime"

for name in fiber_stack_many fiber_stack_100k fiber_stack_sized fiber_stack_sized_overflow fiber_stack_sized_handle_overflow; do
  "$ROOT/bin/kai" build --backend="$backend" "$fx/$name.kai" -o "$work/$name" 2> "$work/build.err" \
    || { echo "fiber-stack FAIL ($backend: build of $name failed)"; cat "$work/build.err"; exit 1; }
done

for n in 1 ""; do
  out="$(KAI_THREADS=$n "$work/fiber_stack_many" 2>&1)" \
    || { echo "fiber-stack FAIL ($backend, KAI_THREADS=$n: 20K live fibers failed)"; echo "$out"; exit 1; }
  [ "$out" = "$(cat "$fx/fiber_stack_many.out.expected")" ] \
    || { echo "fiber-stack DIFF ($backend, many fibers, KAI_THREADS=$n)"; echo "$out"; exit 1; }
done

# 100K live fibers: the VMA count per stack must not cap them, and the pages a
# stack never touched must stay uncommitted.
rss_limit_kb=5000000
for n in 1 ""; do
  if [ "$(uname -s)" = Darwin ]; then
    out="$(KAI_THREADS=$n /usr/bin/time -l "$work/fiber_stack_100k" 2> "$work/time.err")" \
      || { echo "fiber-stack FAIL ($backend, KAI_THREADS=$n: 100K live fibers failed)"; cat "$work/time.err"; exit 1; }
    rss_kb=$(( $(awk '/maximum resident/ {print $1}' "$work/time.err") / 1024 ))
  else
    out="$(KAI_THREADS=$n /usr/bin/time -f %M -o "$work/time.out" "$work/fiber_stack_100k" 2> "$work/time.err")" \
      || { echo "fiber-stack FAIL ($backend, KAI_THREADS=$n: 100K live fibers failed)"; cat "$work/time.err"; exit 1; }
    rss_kb=$(tail -n 1 "$work/time.out")
  fi
  [ "$out" = "$(cat "$fx/fiber_stack_100k.out.expected")" ] \
    || { echo "fiber-stack DIFF ($backend, 100K fibers, KAI_THREADS=$n)"; echo "$out"; exit 1; }
  [ "$rss_kb" -lt "$rss_limit_kb" ] \
    || { echo "fiber-stack FAIL ($backend, KAI_THREADS=$n: 100K fibers hold ${rss_kb} KB RSS, limit ${rss_limit_kb} KB)"; exit 1; }
done

for n in 1 8; do
  out="$(KAI_THREADS=$n "$work/fiber_stack_sized" 2>&1)" \
    || { echo "fiber-stack FAIL ($backend, KAI_THREADS=$n: deep recursion overflowed)"; echo "$out"; exit 1; }
  [ "$out" = "$(cat "$fx/fiber_stack_sized.out.expected")" ] \
    || { echo "fiber-stack DIFF ($backend, KAI_THREADS=$n)"; echo "$out"; exit 1; }
  for name in fiber_stack_sized_overflow fiber_stack_sized_handle_overflow; do
    rc=0
    (KAI_THREADS=$n "$work/$name" > /dev/null 2> "$work/overflow.err") 2> /dev/null || rc=$?
    [ "$rc" -ne 0 ] && grep -q "fiber stack overflow" "$work/overflow.err" \
      || { echo "fiber-stack FAIL ($backend, KAI_THREADS=$n: $name not diagnosed, exit=$rc)"; cat "$work/overflow.err"; exit 1; }
  done
done
echo "fiber-stack OK ($backend: 20K deep completes on a fiber and in a kept-continuation handle, 50M deep diagnosed, at 1 and 8 threads; 20K and 100K live fibers at 1 and default)"
