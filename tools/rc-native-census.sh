#!/bin/bash
# RC census over the FULL golden corpus (examples/perceus +
# examples/effects), NATIVE backend, under ASAN + -DKAI_NO_CELL_POOL.
#
# Complements tools/rc-selfhost-detector.sh: the detector is a gate over a
# small directed corpus on both backends; this is a census — it classifies
# every fixture and prints a taxonomy instead of stopping at a verdict.
# The cell pool recycles freed cells into live-looking ones, so it must be
# OFF for ASAN's redzones to see a double-free / stale read (the pool has
# masked real RC bugs before).
#
# The native ASAN build must instrument the runtime, so the run hides both
# runtime bitcodes to force the legacy link path (cc compiles runtime_llvm.c
# under $CFLAGS). Coverage note: the LLVM-emitted program object itself is
# NOT instrumented — detection rides the instrumented runtime ops plus
# ASAN's malloc/free interceptors (double-free, invalid-free, redzone
# writes), not shadow checks on emitted loads.
#
# Classification per fixture:
#   SANITIZER   ASAN/UBSan diagnostic — RC-corruption candidate
#   DIFF        output differs from the .out.expected golden
#   TIMEOUT     run exceeded $RC_CENSUS_TIMEOUT seconds (default 60)
#   BUILD-FAIL  did not build under the census CFLAGS
#   OK          clean (parity-exempt nondeterministic fixtures skip the diff)
#
# tools/rc-native-census-skips.txt excludes fixtures the census cannot
# execute standalone (compiler-dump goldens, external-signal harness
# shapes) and parks open findings under their issue number.
#
# Fixtures build and run in parallel ($RC_CENSUS_JOBS workers, default the
# core count); the summary aggregates per-fixture verdict files afterwards.
# Exit 1 when any fixture classifies as SANITIZER/DIFF/TIMEOUT/BUILD-FAIL.
# Requires a KAI_LLVM=1 kaic2; exits 2 when the native backend is absent.

set -u

cd "$(dirname "$0")/.."
export ROOT="$(pwd)"

export KAI="$ROOT/bin/kai"
export WORK="$ROOT/stage2/build/rc-native-census"
export SKIPS="$ROOT/tools/backend-parity-skips.txt"
CENSUS_SKIPS="$ROOT/tools/rc-native-census-skips.txt"
rm -rf "$WORK"
mkdir -p "$WORK"

export ASAN_CFLAGS="-std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -DKAI_NO_CELL_POOL -Wno-unused-function -Wno-unused-variable"
export ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
export RUN_TIMEOUT="${RC_CENSUS_TIMEOUT:-60}"

# The cached core object is emitted for the separate-compilation model and
# link-fails against the legacy (bitcode-less) runtime this census forces.
export KAI_NATIVE_CORE_OBJ=0

# GNU timeout on Linux, gtimeout via coreutils on macOS; empty runs unbounded.
export TIMEOUT_CMD="$(command -v timeout || command -v gtimeout || true)"

JOBS="${RC_CENSUS_JOBS:-$( { nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null; } | head -n1 )}"
case "$JOBS" in ''|*[!0-9]*) JOBS=4 ;; esac

# An explicit native probe: a kaic2 without libLLVM errors on
# --backend=native, so every fixture build below is also the probe. This
# up-front check just fails fast with a clear message.
probe="$WORK/native-probe"
printf 'fn main() { println("ok") }\n' > "$probe.kai"
if ! "$KAI" build --backend=native "$probe.kai" -o "$probe" 2>"$probe.err"; then
  echo "rc-native-census: SKIP — kaic2 cannot run the native backend:" >&2
  sed 's/^/  /' "$probe.err" >&2
  exit 2
fi

# Force the instrumented-runtime link path for the whole run; restore the
# bitcodes on any exit so a failure never leaves the tree stripped.
BC="$ROOT/stage0/runtime_llvm.bc"
BC_INLINE="$ROOT/stage0/runtime_inline.bc"
restore_bc() {
  [ -f "$WORK/runtime_llvm.bc.hidden" ] && mv "$WORK/runtime_llvm.bc.hidden" "$BC"
  [ -f "$WORK/runtime_inline.bc.hidden" ] && mv "$WORK/runtime_inline.bc.hidden" "$BC_INLINE"
}
trap restore_bc EXIT INT TERM
[ -f "$BC" ] && mv "$BC" "$WORK/runtime_llvm.bc.hidden"
[ -f "$BC_INLINE" ] && mv "$BC_INLINE" "$WORK/runtime_inline.bc.hidden"

# Fixtures whose stdout is nondeterministic by design (scheduler / RNG /
# clock) per the parity skip list: still built and run under ASAN, but the
# golden diff is not enforced.
nondet_exempt() {
  grep -q "^examples/[^:]*/$1\.kai:exempt-nondet:" "$SKIPS" 2>/dev/null
}

sanitizer_hit() { grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' "$1"; }

run_bounded() {
  if [ -n "$TIMEOUT_CMD" ]; then "$TIMEOUT_CMD" "$RUN_TIMEOUT" "$@"; else "$@"; fi
}

# Goldens follow two capture conventions: stdout only, or stdout+stderr
# (runners that capture 2>&1 to see runtime diagnostics). Accept either,
# with ASan's benign fiber-stack warning filtered from stderr and trailing
# newlines normalised (the $(cat) substitutions strip them — several
# goldens lack the final one).
golden_matches() {
  local exp="$1" bin="$2"
  [ "$(cat "$exp")" = "$(cat "$bin.out")" ] && return 0
  [ "$(cat "$exp")" = "$(cat "$bin.out" "$bin.err" | grep -vE '^==[0-9]+==WARNING: ASan is ignoring|^False positive error reports may follow|^For details see https://github.com/google/sanitizers')" ]
}

# One fixture: build + run + classify. Writes the verdict to
# $WORK/<dir>-<name>.verdict for the serial aggregation pass.
classify_one() {
  local dir="${1%%/*}" name="${1##*/}"
  local src="$ROOT/examples/$dir/$name.kai" exp="$ROOT/examples/$dir/$name.out.expected"
  local bin="$WORK/$dir-$name" verdict=OK
  if ! CFLAGS="$ASAN_CFLAGS" "$KAI" build --backend=native "$src" -o "$bin" 2>"$bin.build"; then
    verdict=BUILD-FAIL
  else
    run_bounded env KAI_THREADS=1 "$bin" >"$bin.out" 2>"$bin.err" </dev/null
    local rc=$?
    if sanitizer_hit "$bin.err"; then
      verdict=SANITIZER
    elif [ "$rc" -eq 124 ]; then
      verdict=TIMEOUT
    elif ! nondet_exempt "$name" && ! golden_matches "$exp" "$bin"; then
      verdict=DIFF
    fi
  fi
  echo "$verdict" > "$bin.verdict"
  echo "  $verdict $dir/$name"
}
export -f classify_one nondet_exempt sanitizer_hit run_bounded golden_matches

collect_fixtures() {
  for dir in perceus effects; do
    for src in "$ROOT/examples/$dir"/*.kai; do
      name="$(basename "$src" .kai)"
      [ -f "$ROOT/examples/$dir/$name.out.expected" ] || continue
      grep -q "^$dir/$name:" "$CENSUS_SKIPS" 2>/dev/null && continue
      echo "$dir/$name"
    done
  done
}

print_failure_detail() {
  local verdict="$1" dir="$2" name="$3" bin="$WORK/$2-$3"
  echo ""
  echo "FAIL $dir/$name — $verdict"
  case "$verdict" in
    SANITIZER)  grep -m4 -E 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:|SUMMARY' "$bin.err" | sed 's/^/    /' ;;
    DIFF)       diff "$ROOT/examples/$dir/$name.out.expected" "$bin.out" 2>/dev/null | head -6 | sed 's/^/    /' ;;
    BUILD-FAIL) tail -4 "$bin.build" 2>/dev/null | sed 's/^/    /' ;;
  esac
}

# One `<dir>/<name>:<verdict>` line per fixture, in corpus order.
collect_results() {
  while IFS= read -r fx; do
    echo "$fx:$(cat "$WORK/${fx%%/*}-${fx##*/}.verdict" 2>/dev/null || echo BUILD-FAIL)"
  done < "$1"
}

count_verdict() { grep -c ":$1\$" "$results"; }

fixtures="$WORK/fixtures.txt"
collect_fixtures > "$fixtures"
total="$(wc -l < "$fixtures" | tr -d ' ')"

echo "rc-native-census: $total fixtures, native backend, ASAN + no-cell-pool, instrumented runtime, $JOBS workers"
xargs -P "$JOBS" -n 1 -I{} bash -c 'classify_one "$@"' _ {} < "$fixtures"
restore_bc

results="$WORK/results.txt"
collect_results "$fixtures" > "$results"
failures="$(grep -v ':OK$' "$results")"
for line in $failures; do
  fx="${line%:*}"
  print_failure_detail "${line##*:}" "${fx%%/*}" "${fx##*/}"
done

echo ""
echo "rc-native-census: $total fixtures — OK $(count_verdict OK), SANITIZER $(count_verdict SANITIZER), DIFF $(count_verdict DIFF), TIMEOUT $(count_verdict TIMEOUT), BUILD-FAIL $(count_verdict BUILD-FAIL)"
if [ -n "$failures" ]; then
  echo "rc-native-census: FAIL —"
  for line in $failures; do echo "  $line"; done
  exit 1
fi
echo "rc-native-census: PASS — full corpus clean on the native backend."
exit 0
