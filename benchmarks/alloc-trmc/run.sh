#!/usr/bin/env bash
# alloc-trmc benchmark: an allocation-heavy TRMC workload on both backends.
#
#   ./run.sh        CPU time (user+sys), median of 15 runs per backend
#
# Needs stage2/kaic2 built with libLLVM for the native column
# (`make KAI_LLVM=1 kaic2`). Runs single-threaded so the number measures the
# per-cell path, not the scheduler.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$REPO_ROOT/benchmarks/alloc-trmc"
KAIC2="$REPO_ROOT/stage2/kaic2"
CC="${CC:-cc}"
RUNS=15
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

[[ -x "$KAIC2" ]] || { echo "kaic2 not built — run 'make KAI_LLVM=1 kaic2' first" >&2; exit 1; }

cp "$HERE/alloc_trmc.kai" "$WORK/"
"$KAIC2" --path "$REPO_ROOT/stdlib" "$WORK/alloc_trmc.kai" > "$WORK/alloc_c.c"
"$CC" -std=c99 -O2 -I "$REPO_ROOT/stage2" -I "$REPO_ROOT/stage0" "$WORK/alloc_c.c" -o "$WORK/alloc_c" -lm
BINS=("$WORK/alloc_c")
if (cd "$WORK" && "$KAIC2" --emit=native --path "$REPO_ROOT/stdlib" alloc_trmc.kai) >"$WORK/native.err" 2>&1; then
  "$CC" -std=c99 -O2 "$WORK/alloc_trmc.o" "$REPO_ROOT/stage0/runtime_llvm.c" \
    -I "$REPO_ROOT/stage2" -I "$REPO_ROOT/stage0" -o "$WORK/alloc_native" -lm -ldl
  BINS+=("$WORK/alloc_native")
else
  echo "native column skipped (kaic2 without libLLVM)" >&2
fi

python3 - "$RUNS" "${BINS[@]}" <<'PY'
import os, resource, statistics, subprocess, sys
runs, bins = int(sys.argv[1]), sys.argv[2:]
env = dict(os.environ, KAI_THREADS="1")
def cpu_ms(b):
    a = resource.getrusage(resource.RUSAGE_CHILDREN)
    subprocess.run([b], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    z = resource.getrusage(resource.RUSAGE_CHILDREN)
    return 1000 * ((z.ru_utime - a.ru_utime) + (z.ru_stime - a.ru_stime))
for b in bins: cpu_ms(b)
r = {b: [] for b in bins}
for i in range(runs):
    for b in (bins if i % 2 == 0 else bins[::-1]): r[b].append(cpu_ms(b))
for b in bins:
    print(f"{os.path.basename(b):14s} median {statistics.median(r[b]):7.1f} ms CPU")
PY
