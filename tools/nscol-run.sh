#!/usr/bin/env bash
# Runs one axis of the namespace-collision corpus over its fixtures in
# parallel and writes the axis log that tools/nscol-ratchet.sh reads.
#
# Usage (from stage2/, as the Makefile targets call it):
#   nscol-run.sh <axis> <log>
#
# The fixture list comes from tests/namespace_matrix_axes.sh; the
# `nohomespell` pseudo-axis scans the c and neg fixtures. Each fixture
# leaves exactly one `<label> OK|FAIL|DIFF <fixture> (<why>)` line.
#
# Verdicts are counted from the workers' stdout, one short printf each
# (atomic on a pipe), never from files the workers leave behind: a burst
# of parallel file creations can lose some on Linux. A fixture's detail
# (stderr head, golden diff) goes to its own file and is appended to the
# log after the verdicts.
#
# The compile commands come from the Makefile through the environment:
# NSCOL_KAIC2, NSCOL_CORE_CACHE, NSCOL_CC, NSCOL_ASAN_CC, NSCOL_LDLIBS.
# NSCOL_JOBS caps the workers (default: online CPUs).

# Flag and environment strings are word-split on purpose.
# shellcheck disable=SC2086,SC2046
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORPUS="$ROOT/examples/namespace-collisions"
KAI="$ROOT/bin/kai"

# Per-axis configuration: which check runs a fixture, plus the bin/kai
# environment and arguments for the axes built through the driver.
configure() {
  AXIS="$1"
  LABEL="namespace-collisions-$AXIS"
  KAI_ENV=""
  KAI_ARGS=""
  case "$AXIS" in
    c)              CHECK=c_build; LABEL="namespace-collisions" ;;
    neg)            CHECK=c_reject ;;
    native)         CHECK=kai_build; KAI_ENV="KAI_BACKEND=native KAI_NATIVE_MODULAR=0" ;;
    native-modular) CHECK=kai_build; KAI_ENV="KAI_BACKEND=native KAI_NATIVE_MODULAR=1" ;;
    neg-native)     CHECK=kai_reject; KAI_ENV="KAI_BACKEND=native" ;;
    modular)        CHECK=kai_build; KAI_ENV="KAI_MODULAR=1"; KAI_ARGS="--backend=c" ;;
    neg-modular)    CHECK=kai_reject; KAI_ENV="KAI_MODULAR=1"; KAI_ARGS="--backend=c" ;;
    asan)           CHECK=asan ;;
    diag)           CHECK=diag ;;
    testsym)        CHECK=kai_output; KAI_ARGS="test --backend=c" ;;
    check)          CHECK=kai_output; KAI_ARGS="check" ;;
    nohomespell)    CHECK=nohomespell ;;
    *) echo "nscol-run: unknown axis '$AXIS'" >&2; exit 2 ;;
  esac
  OUT="build/nscol-$AXIS.d"
}

fixtures() {
  case "$AXIS" in
    nohomespell) { "$ROOT/tests/namespace_matrix_axes.sh" c; "$ROOT/tests/namespace_matrix_axes.sh" neg; } | awk '!seen[$0]++' ;;
    *) "$ROOT/tests/namespace_matrix_axes.sh" "$AXIS" ;;
  esac
}

# The Makefile's compile commands carry shell quoting (an escaped
# -DKAI_STDLIB_PATH=\"...\"), so they are re-parsed rather than split.
kaic2() { eval "$NSCOL_KAIC2" '"$@"'; }
cc_c() { eval "$NSCOL_CC" '"$@"' "$NSCOL_LDLIBS"; }
cc_asan() { eval "$NSCOL_ASAN_CC" '"$@"'; }
kai() { env $KAI_ENV "$KAI" "$@"; }

flags() { cat "$CORPUS/$1/main.flags" 2>/dev/null || true; }
compile() { kaic2 "${@:2}" --path "$ROOT/stdlib" $(flags "$1") "$CORPUS/$1/main.kai"; }
golden() { printf '%s/%s/%s' "$CORPUS" "$1" "$2"; }

# Each check prints `FAIL|DIFF <why>` and returns 1 on a miss; anything
# it writes to stderr is the fixture's detail.

run_and_diff() {
  "$1" > "$1.out" || { echo "FAIL run"; return 1; }
  diff "$(golden "$2" main.out.expected)" "$1.out" >&2 || { echo "DIFF vs golden"; return 1; }
}

needle_in() {
  local needle
  needle="$(head -1 "$(golden "$1" "$2")")"
  grep -qF "$needle" "$3" && return 0
  echo "FAIL substring '$needle' not in output"
  head -5 "$3" >&2
  return 1
}

build_failed() {
  echo "FAIL $1"
  head -3 "$2" >&2
  return 1
}

check_c_build() {
  local b="$OUT/$1"
  compile "$1" $NSCOL_CORE_CACHE > "$b.c" 2> "$b.err" || build_failed kaic2 "$b.err" || return 1
  cc_c "$b.c" -o "$b.bin" || { echo "FAIL cc"; return 1; }
  run_and_diff "$b.bin" "$1"
}

check_c_reject() {
  local b="$OUT/$1"
  compile "$1" $NSCOL_CORE_CACHE > /dev/null 2> "$b.err" && { echo "FAIL expected error, got success"; return 1; }
  needle_in "$1" main.err.expected "$b.err"
}

check_kai_build() {
  local b="$OUT/$1"
  kai build $KAI_ARGS "$CORPUS/$1/main.kai" -o "$b.bin" 2> "$b.err" || build_failed build "$b.err" || return 1
  run_and_diff "$b.bin" "$1"
}

check_kai_reject() {
  local b="$OUT/$1"
  kai build $KAI_ARGS "$CORPUS/$1/main.kai" -o "$b.bin" > /dev/null 2> "$b.err" && { echo "FAIL expected error, got success"; return 1; }
  needle_in "$1" main.err.expected "$b.err"
}

check_kai_output() {
  local b="$OUT/$1" exp="main.$AXIS.expected"
  [ "$AXIS" = testsym ] && exp=main.test.expected
  kai $KAI_ARGS "$CORPUS/$1/main.kai" > "$b.out" 2>&1 || { echo "FAIL kai $KAI_ARGS errored"; cat "$b.out" >&2; return 1; }
  needle_in "$1" "$exp" "$b.out"
}

check_asan() {
  local b="$OUT/$1"
  compile "$1" > "$b.c" 2> "$b.cerr" || build_failed kaic2 "$b.cerr" || return 1
  cc_asan "$b.c" -o "$b.bin" || { echo "FAIL cc"; return 1; }
  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
    "$b.bin" > "$b.out" 2> "$b.err" || true
  grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' "$b.err" || return 0
  echo "FAIL sanitizer diagnostic"
  cat "$b.err" >&2
  return 1
}

# Every line of DIAG.expected must be a substring of the diagnostic; the
# compile's own exit status is not asserted.
check_diag() {
  local b="$OUT/$1" missing
  compile "$1" $NSCOL_CORE_CACHE > /dev/null 2> "$b.err" || true
  missing="$(grep -v '^$' "$(golden "$1" DIAG.expected)" | while IFS= read -r n; do
    grep -qF -- "$n" "$b.err" || echo "  lacks: $n"
  done)"
  [ -z "$missing" ] && return 0
  echo "FAIL diagnostic lacks an expected line"
  { echo "$missing"; head -5 "$b.err"; } >&2
  return 1
}

# A home-spelled name (`Item__da`) is internal and must never reach a
# diagnostic. Synthesised heads (`__proto_`) open with the separator, so
# an identifier character is required before it.
check_nohomespell() {
  local b="$OUT/$1"
  [ -f "$CORPUS/$1/main.kai" ] || return 0
  compile "$1" $NSCOL_CORE_CACHE > /dev/null 2> "$b.err" || true
  grep -qE '[A-Za-z0-9_]__[a-z][A-Za-z0-9_]*' "$b.err" || return 0
  echo "FAIL home-spelled name in a diagnostic"
  grep -oE '[A-Za-z0-9_]*__[a-z][A-Za-z0-9_]*' "$b.err" | sort -u | sed 's/^/    /' >&2
  return 1
}

worker() {
  local d="$1" why st=OK
  if ! why="$("check_$CHECK" "$d" 2> "$OUT/$d.detail")"; then
    [ -n "$why" ] || why="FAIL check aborted"
    st="${why%% *}"
    why=" (${why#* })"
  else
    why=""
  fi
  printf '%s %s %s%s\n' "$LABEL" "$st" "$d" "$why"
}

if [ "${1:-}" = "--worker" ]; then
  configure "$2"
  worker "$3"
  exit 0
fi

[ $# -eq 2 ] || { echo "usage: $0 <axis> <log>" >&2; exit 2; }
configure "$1"
log="$2"
rm -rf "$OUT"
mkdir -p "$OUT"
fixtures > "$OUT/list"

jobs="${NSCOL_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
# A worker lost to a signal leaves no verdict; the ratchet reports it as
# a declared fixture missing from the log.
verdicts="$(xargs -P "$jobs" -n 1 "$0" --worker "$AXIS" < "$OUT/list" || true)"

# Verdicts in matrix order, then the detail of every fixture that missed.
printf '%s\n' "$verdicts" \
  | awk 'NR == FNR { rank[$0] = NR; next } NF { print rank[$3] "\t" $0 }' "$OUT/list" - \
  | sort -n | cut -f2- > "$OUT/verdicts"
{
  cat "$OUT/verdicts"
  awk '$2 != "OK" { print $3 }' "$OUT/verdicts" | while read -r d; do
    echo "--- $d ---"
    cat "$OUT/$d.detail"
  done
} > "$log"
