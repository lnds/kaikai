#!/bin/bash
# Effects golden gate — runtime stdout vs the .out.expected oracle.
#
# Backend parity diffs the native backend against the C oracle, so a
# defect BOTH backends share is invisible to it: they agree, parity is
# green, and nothing compares the program's actual output against the
# fixture's golden. This harness closes that gap for examples/effects:
# every flat fixture with a sibling .out.expected is built under
# TARGET_BACKEND, run, and its stdout must match the golden byte for
# byte (exit status must be 0).
#
# bash (not sh): the xargs worker fan-out uses `export -f`, which dash
# rejects.
#
# Skip discipline mirrors tools/backend-parity-skips.txt:
# tools/effects-goldens-skips.txt, `<relative-path>:<issue>:<reason>`.
# File the issue first; the skip is the bookmark, the issue is the work.

set -eu

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAI="$ROOT/bin/kai"

# kai_corpus_pinned_threads: fixtures whose documented property IS the
# one-thread cooperative rotation pin KAI_THREADS=1 in a sibling .env;
# every corpus harness runs them at the pin.
. "$ROOT/tools/lib/corpus.sh"

TARGET_BACKEND="${TARGET_BACKEND:-c}"
SKIPS="$ROOT/tools/effects-goldens-skips.txt"
FIXDIR="examples/effects"
RUN_TIMEOUT="${EFFECTS_GOLDENS_RUN_TIMEOUT:-60}"

# A C-only kaic2 rejects the native backend; SKIP (success) so the
# native gate runs only where libLLVM is present (tier1-native).
probe_native_or_skip() {
  local probe
  probe="$(mktemp)"; mv "$probe" "$probe.kai"; probe="$probe.kai"
  printf 'fn main() : Int = 0\n' > "$probe"
  if ! KAI_BACKEND=native "$KAI" build "$probe" -o "${probe%.kai}.bin" >/dev/null 2>&1; then
    echo "test-effects-goldens: SKIP (native backend unavailable; rebuild kaic2 with make KAI_LLVM=1)"
    rm -f "$probe" "${probe%.kai}.bin"
    exit 0
  fi
  rm -f "$probe" "${probe%.kai}.bin"
}

detect_jobs() {
  if [ -n "${EFFECTS_GOLDENS_JOBS:-}" ]; then echo "$EFFECTS_GOLDENS_JOBS"
  elif command -v nproc >/dev/null 2>&1; then nproc
  elif command -v sysctl >/dev/null 2>&1; then sysctl -n hw.logicalcpu 2>/dev/null || echo 4
  else echo 4
  fi
}

detect_timeout_cmd() {
  if command -v timeout >/dev/null 2>&1; then echo "timeout"
  elif command -v gtimeout >/dev/null 2>&1; then echo "gtimeout"
  fi
}

run_with_timeout() {
  if [ -n "$TIMEOUT_CMD" ]; then "$TIMEOUT_CMD" "$RUN_TIMEOUT" "$@"; else "$@"; fi
}

# Appends the failure block (header line + detail file excerpt) and
# records the F verdict. Single-line appends up to PIPE_BUF are atomic,
# and the bounded excerpts fit.
report_fail() {
  { echo "FAIL $1"; sed 's/^/    /' < "$2"; echo; } >> "$failures"
  printf 'F\n' >> "$results"
}

process_one() {
  local f="$1" golden="${1%.kai}.out.expected" slug bin out rc
  if grep -q "^${f}:" "$SKIPS" 2>/dev/null; then
    printf 'S\n' >> "$results"
    return 0
  fi

  slug=$(echo "$f" | tr '/.' '__')
  bin="$tmp/${slug}.bin"
  out="$tmp/${slug}.out"

  if ! KAI_BACKEND="$TARGET_BACKEND" KAI_NATIVE_MODULAR=0 "$KAI" build "$f" -o "$bin" >"$tmp/${slug}.build.log" 2>&1; then
    tail -10 "$tmp/${slug}.build.log" > "$tmp/${slug}.detail"
    report_fail "$f — $TARGET_BACKEND build failed:" "$tmp/${slug}.detail"
    return 0
  fi

  local pin
  pin="$(kai_corpus_pinned_threads "$f")" || pin=""
  rc=0
  run_with_timeout env ${pin:+KAI_THREADS="$pin"} "$bin" >"$out" 2>"$tmp/${slug}.err" </dev/null || rc=$?
  if [ "$rc" != "0" ]; then
    tail -5 "$tmp/${slug}.err" > "$tmp/${slug}.detail"
    report_fail "$f — exit status $rc (want 0); stderr tail:" "$tmp/${slug}.detail"
    return 0
  fi

  # cmp, not line-oriented diff: the byte that motivated this gate was
  # a trailing newline, which diff-family tools are prone to forgiving.
  if ! cmp -s "$golden" "$out"; then
    diff "$golden" "$out" | head -10 > "$tmp/${slug}.detail" || true
    report_fail "$f — stdout differs from golden:" "$tmp/${slug}.detail"
    return 0
  fi

  printf 'P\n' >> "$results"
}

[ "$TARGET_BACKEND" = native ] && probe_native_or_skip
if [ ! -x "$KAI" ]; then
  echo "test-effects-goldens FAIL — bin/kai not found or not executable"
  exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM
TIMEOUT_CMD="$(detect_timeout_cmd)"
failures="$tmp/failures.log"
results="$tmp/results"
: > "$failures"
: > "$results"

export -f process_one run_with_timeout report_fail kai_corpus_pinned_threads
export tmp results failures SKIPS KAI TARGET_BACKEND TIMEOUT_CMD RUN_TIMEOUT

for g in "$FIXDIR"/*.out.expected; do
  src="${g%.out.expected}.kai"
  [ -f "$src" ] && echo "$src"
done > "$tmp/fixtures"

if [ ! -s "$tmp/fixtures" ]; then
  echo "test-effects-goldens FAIL — no fixtures found under $FIXDIR"
  exit 1
fi

xargs -P "$(detect_jobs)" -n 1 bash -c 'process_one "$1"' _ < "$tmp/fixtures"

pass=$(grep -c '^P$' "$results" || true)
fail=$(grep -c '^F$' "$results" || true)
skip=$(grep -c '^S$' "$results" || true)

cat "$failures"
echo "test-effects-goldens ($TARGET_BACKEND): $pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
