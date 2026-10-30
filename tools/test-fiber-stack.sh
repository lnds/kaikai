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

# Guards that do not split the VMA (the runtime reports it with
# KAI_FIBER_STACK_REPORT=1) leave the live-fiber
# count to address space: 100K must run. Elsewhere each stack is two VMAs and
# allocation past vm.max_map_count must fail with the diagnostic, not hand out
# an unguarded stack.
vma_free=0
KAI_FIBER_STACK_REPORT=1 "$work/fiber_stack_many" 2>&1 > /dev/null | grep -q "fiber stack guards: vma-free" && vma_free=1
rss_limit_kb=5000000
max_maps=0
[ -r /proc/sys/vm/max_map_count ] && max_maps="$(cat /proc/sys/vm/max_map_count)"

run_counting_maps() {
  "$@" > "$work/run.out" 2> "$work/run.err" &
  pid=$!
  peak=0
  while kill -0 "$pid" 2> /dev/null; do
    c="$(wc -l < "/proc/$pid/maps" 2> /dev/null || echo 0)"
    [ "$c" -gt "$peak" ] && peak="$c"
    sleep 0.02
  done
  rc=0
  wait "$pid" || rc=$?
  return "$rc"
}

if [ "$vma_free" = 1 ]; then
  for n in 1 ""; do
    if [ "$(uname -s)" = Darwin ]; then
      out="$(KAI_THREADS=$n /usr/bin/time -l "$work/fiber_stack_100k" 2> "$work/time.err")" \
        || { echo "fiber-stack FAIL ($backend, KAI_THREADS=$n: 100K live fibers failed)"; cat "$work/time.err"; exit 1; }
      rss_kb=$(( $(awk '/maximum resident/ {print $1}' "$work/time.err") / 1024 ))
      peak=n/a
    else
      export KAI_THREADS=$n
      run_counting_maps /usr/bin/time -f %M -o "$work/time.out" "$work/fiber_stack_100k" \
        || { echo "fiber-stack FAIL ($backend, KAI_THREADS=$n: 100K live fibers failed)"; cat "$work/run.err"; exit 1; }
      unset KAI_THREADS
      out="$(cat "$work/run.out")"
      rss_kb=$(tail -n 1 "$work/time.out")
    fi
    [ "$out" = "$(cat "$fx/fiber_stack_100k.out.expected")" ] \
      || { echo "fiber-stack DIFF ($backend, 100K fibers, KAI_THREADS=$n)"; echo "$out"; exit 1; }
    [ "$rss_kb" -lt "$rss_limit_kb" ] \
      || { echo "fiber-stack FAIL ($backend, KAI_THREADS=$n: 100K fibers hold ${rss_kb} KB RSS, limit ${rss_limit_kb} KB)"; exit 1; }
    echo "fiber-stack info ($backend, KAI_THREADS=$n: kernel $(uname -r), VMA-free guards, 100K fibers, peak maps lines $peak, max RSS ${rss_kb} KB)"
  done
else
  over=$(( max_maps + 1000 ))
  sed "s/100000/$over/" "$fx/fiber_stack_100k.kai" > "$work/fiber_stack_over.kai"
  "$ROOT/bin/kai" build --backend="$backend" "$work/fiber_stack_over.kai" -o "$work/fiber_stack_over" 2> "$work/build.err" \
    || { echo "fiber-stack FAIL ($backend: build of the over-budget fixture failed)"; cat "$work/build.err"; exit 1; }
  for n in 1 ""; do
    rc=0
    KAI_THREADS=$n "$work/fiber_stack_over" > /dev/null 2> "$work/over.err" || rc=$?
    [ "$rc" -ne 0 ] && grep -q "vm.max_map_count" "$work/over.err" \
      || { echo "fiber-stack FAIL ($backend, KAI_THREADS=$n: $over fibers past the VMA budget not diagnosed, exit=$rc)"; cat "$work/over.err"; exit 1; }
    echo "fiber-stack info ($backend, KAI_THREADS=$n: kernel $(uname -r), PROT_NONE guards, max_map_count $max_maps, past-budget run diagnosed)"
  done
fi

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
echo "fiber-stack OK ($backend: 20K deep completes on a fiber and in a kept-continuation handle, 50M deep diagnosed, at 1 and 8 threads; 20K live fibers at 1 and default; 100K where guards are VMA-free, a diagnosed failure past vm.max_map_count where not)"
