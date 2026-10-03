#!/bin/sh
# Native module objects do not depend on how many threads emit them.
#
# With KAI_NATIVE_JOBS > 1, kaic2 optimises and emits each native-modular
# partition on a worker thread while it builds the next one. Pinned here:
#   1. the content-addressed objects are byte-identical for 1 and 4 threads,
#      and both binaries print the fixture's expected output;
#   2. a partition that fails to emit on a worker still fails the build.
#
# This needs a kaic2 with libLLVM; a C-only kaic2 reports SKIP.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
FX="$ROOT/examples/multi-module/issue-963-cross-module-mono"

work="$(mktemp -d)"
trap 'chmod -R u+w "$work" 2>/dev/null; rm -rf "$work"' EXIT INT TERM

build() {
  KAI_NATIVE_JOBS="$1" KAI_NATIVE_MODULAR_CACHE_DIR="$work/nm-$1" \
    KAI_CORE_CACHE_DIR="$work/core-$1" \
    "$KAI" build --backend=native "$FX/main.kai" -o "$work/bin-$1" 2>"$work/err-$1"
}

if ! build 1; then
  if grep -q "not built into this compiler\|native backend is not built" "$work/err-1"; then
    echo "native-emit-jobs SKIP (native backend not built into this kaic2)"
    exit 0
  fi
  echo "native-emit-jobs FAIL: single-thread build failed"; cat "$work/err-1"; exit 1
fi
build 4 || { echo "native-emit-jobs FAIL: 4-thread build failed"; cat "$work/err-4"; exit 1; }

for n in 1 4; do
  "$work/bin-$n" > "$work/out-$n"
  if ! cmp -s "$work/out-$n" "$FX/main.out.expected"; then
    echo "native-emit-jobs FAIL: $n-thread binary output differs"
    diff "$FX/main.out.expected" "$work/out-$n" | head -6
    exit 1
  fi
done

if ! diff -r "$work/nm-1" "$work/nm-4" > "$work/objs.diff"; then
  echo "native-emit-jobs FAIL: objects differ between 1 and 4 threads"
  head -6 "$work/objs.diff"
  exit 1
fi

# The fixture's partitions, made unwritable, cannot be emitted again.
for d in "$work"/nm-4/*/; do
  case "$d" in */runtime/) continue ;; esac
  rm -f "$d"*.o
  chmod a-w "$d"
done
if build 4; then
  echo "native-emit-jobs FAIL: a partition that could not be written built anyway"
  exit 1
fi
if ! grep -q "native object emit failed" "$work/err-4"; then
  echo "native-emit-jobs FAIL: the emit failure was not reported"; cat "$work/err-4"; exit 1
fi

echo "native-emit-jobs: PASS (objects identical for 1 and 4 threads; a failed emit fails the build)"
