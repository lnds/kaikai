#!/bin/sh
# Negative-space test runner (issue #511).
#
# For every negative fixture under examples/negative/**, run kaic2
# (the stage 2 compiler) and assert two things:
#   1. exit code is non-zero (compile-time rejection)
#   2. stderr contains the substring stored in the sibling
#      .err.expected golden (first line, like the existing
#      test-modules-qualified-neg pattern in stage2/Makefile).
#
# Layout conventions:
#
#   Single-file fixture:
#     examples/negative/<category>/<name>.kai
#     examples/negative/<category>/<name>.err.expected
#
#   Single-file fixture loaded as prelude:
#     examples/negative/<category>/<name>.kai
#     examples/negative/<category>/<name>.err.expected
#     examples/negative/<category>/<name>.prelude.kai   (optional sibling)
#
#   Multi-file fixture:
#     examples/negative/<category>/<name>/main.kai
#     examples/negative/<category>/<name>/lib.kai        (or any siblings)
#     examples/negative/<category>/<name>/main.err.expected
#
# Single-file fixtures whose `.err.expected` ends in `.kaic1.err.expected`
# are routed to stage1/kaic1 instead — used to assert clean stage 1
# rejection of stage-2-only features (e.g. protocol impls).
#
# Each fixture run also produces one line of audit output:
#   PASS <fixture>  — language rejects with the expected diagnostic.
#   FAIL <fixture>  — language accepts (silent contract; needs follow-up).
#   MISS <fixture>  — language rejects but with a different diagnostic.
# The script exits non-zero if any fixture FAILs or MISSes.
#
# Parallelism (lane tier1-perf): the per-fixture kaic2 invocations are
# independent — each writes only to its own temp errfile keyed by the
# fixture's relative path. The orchestrator therefore fans the goldens
# out over `xargs -P$JOBS`, re-invoking this script in a worker mode
# (`__worker <exp>`) that processes exactly one golden and prints its
# single PASS/FAIL/MISS line. The summary is recomputed from the
# collected lines, so the exit-code contract is byte-identical to the
# old serial loop. JOBS defaults to the core count and is overridable
# via KAI_TEST_JOBS; KAI_TEST_JOBS=1 restores fully serial behaviour.

set -eu

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

KAIC2="$ROOT/stage2/kaic2"
KAIC1="$ROOT/stage1/kaic1"

if [ ! -x "$KAIC2" ]; then
  echo "test-negative FAIL — stage2/kaic2 not built (run 'make kaic2' first)"
  exit 1
fi

# run_one: classify a single fixture and print one PASS/FAIL/MISS line.
# Self-contained — reads only its arguments + $ROOT/$KAIC1/$KAIC2 and a
# private $tmp. Safe to run concurrently with other run_one calls
# because every output path it touches is keyed by the fixture's
# relative path.
run_one() {
  exp="$1"
  tmp="$2"

  base=$(basename "$exp")
  dir=$(dirname "$exp")

  # ---- compile-time goldens (.err.expected / .diag.expected) ----
  case "$base" in
    *.run.err.expected)
      run_one_runtime "$exp" "$tmp"
      return ;;
  esac

  multi_line=0
  case "$base" in
    main.err.expected)
      stem="main"; src="$dir/main.kai"; compiler="$KAIC2" ;;
    *.kaic1.err.expected)
      stem="${base%.kaic1.err.expected}"; src="$dir/$stem.kai"; compiler="$KAIC1" ;;
    *.diag.expected)
      stem="${base%.diag.expected}"; src="$dir/$stem.kai"; compiler="$KAIC2"; multi_line=1 ;;
    *.err.expected)
      stem="${base%.err.expected}"; src="$dir/$stem.kai"; compiler="$KAIC2" ;;
    *)
      return ;;
  esac

  if [ ! -f "$src" ]; then
    echo "MISS $exp — source file not found ($src)"
    return
  fi

  extra_flags=""
  if [ -f "$dir/$stem.flags" ]; then
    extra_flags=$(cat "$dir/$stem.flags")
  fi

  rel="${src#$ROOT/}"
  errfile="$tmp/$(echo "$rel" | tr '/' '_').err"
  rc=0
  # shellcheck disable=SC2086 — extra_flags is intentionally word-split.
  "$compiler" $extra_flags "$src" > /dev/null 2> "$errfile" || rc=$?

  if [ "$rc" -eq 0 ]; then
    echo "FAIL $rel — exit 0 (silent contract; expected non-zero)"
    return
  fi

  if [ "$multi_line" = "1" ]; then
    missing=""
    while IFS= read -r line; do
      case "$line" in
        ""|"#"*) continue ;;
      esac
      if ! grep -qF -- "$line" "$errfile"; then
        missing="$missing\n  want: $line"
      fi
    done < "$exp"
    if [ -z "$missing" ]; then
      echo "PASS $rel"
    else
      printf 'MISS %s — diagnostic body mismatch%b\n' "$rel" "$missing"
    fi
    return
  fi

  needle=$(head -1 "$exp")
  if [ -z "$needle" ]; then
    echo "MISS $rel — .err.expected first line is empty"
    return
  fi

  if grep -qF "$needle" "$errfile"; then
    echo "PASS $rel"
  else
    echo "MISS $rel — diagnostic mismatch"
    echo "  want: $needle"
    echo "  got : $(head -1 "$errfile")"
  fi
}

# run_one_runtime: typer-accepts-but-runtime-rejects fixtures. Compile
# via kaic2 → C → cc, run, expect non-zero exit + golden substring.
run_one_runtime() {
  exp="$1"
  tmp="$2"

  base=$(basename "$exp")
  dir=$(dirname "$exp")
  stem="${base%.run.err.expected}"
  src="$dir/$stem.kai"

  if [ ! -f "$src" ]; then
    echo "MISS $exp — source file not found ($src)"
    return
  fi

  rel="${src#$ROOT/}"
  key="$(echo "$rel" | tr '/' '_')"
  cfile="$tmp/$key.c"
  bin="$tmp/$key.bin"
  errfile="$tmp/$key.runerr"

  if ! "$KAIC2" "$src" > "$cfile" 2> "$errfile.compile"; then
    echo "FAIL $rel — kaic2 rejected at compile (this is a runtime-negative fixture)"
    return
  fi
  # `-I "$ROOT/stage2"` comes BEFORE `-I "$ROOT/stage0"` so the emitted
  # `#include "runtime.h"` resolves to stage 2's Koka-style runtime
  # (tagged Int, kai_intf, reuse-token). Compiling kaic2-emitted C
  # against stage0/runtime.h alone leaves `kai_intf` undeclared.
  if ! cc -std=c99 -Wall -Wno-incompatible-function-pointer-types -I "$ROOT/stage2" -I "$ROOT/stage0" "$cfile" -o "$bin" -lm 2> "$errfile.cc"; then
    echo "FAIL $rel — cc rejected the generated C (likely silent type-level contract)"
    head -3 "$errfile.cc"
    return
  fi

  rc=0
  "$bin" > /dev/null 2> "$errfile" || rc=$?
  if [ "$rc" -eq 0 ]; then
    echo "FAIL $rel — binary exit 0 (expected runtime panic)"
    return
  fi

  needle=$(head -1 "$exp")
  if grep -qF "$needle" "$errfile"; then
    echo "PASS $rel (runtime)"
  else
    echo "MISS $rel (runtime) — diagnostic mismatch"
    echo "  want: $needle"
    echo "  got : $(head -1 "$errfile")"
  fi
}

# ---- worker mode -----------------------------------------------------
# `$0 __worker <tmpdir> <exp>` processes exactly one golden. Invoked by
# xargs from the orchestrator below; never called by a human.
if [ "${1:-}" = "__worker" ]; then
  run_one "$3" "$2"
  exit 0
fi

# ---- orchestrator ----------------------------------------------------
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

# Job count: core-count by default, KAI_TEST_JOBS overrides (=1 → serial).
JOBS="${KAI_TEST_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"

self="$ROOT/tools/test-negative.sh"
results="$tmp/results"

# Skip the `silent_contract/` subtree — those fixtures DOCUMENT a gap
# the language has not yet closed. Find every compile-time + runtime
# golden, fan out over xargs -P. Each worker prints its own lines; the
# null-delimited find + xargs -0 keeps paths with spaces intact.
find "$ROOT/examples/negative" \
  \( -name '*.err.expected' -o -name '*.diag.expected' -o -name '*.run.err.expected' \) \
  -not -path '*/silent_contract/*' -print0 2>/dev/null \
  | sort -z \
  | xargs -0 -P "$JOBS" -n1 "$self" __worker "$tmp" \
  > "$results"

cat "$results"

# Counters recomputed from the results file — identical contract to the
# old serial loop (subshell/parallel counter loss is a non-issue).
pass=$(grep -c '^PASS ' "$results" 2>/dev/null || true)
fail=$(grep -c '^FAIL ' "$results" 2>/dev/null || true)
miss=$(grep -c '^MISS ' "$results" 2>/dev/null || true)
total=$((pass + fail + miss))

echo
echo "test-negative summary: $pass PASS, $fail FAIL, $miss MISS (total $total)"

if [ "$fail" -ne 0 ] || [ "$miss" -ne 0 ]; then
  exit 1
fi
