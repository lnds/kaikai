#!/bin/sh
# tier1-backend-parity (issue #575).
#
# Build every entry-point fixture with both backends (C and LLVM),
# run the resulting binaries, and assert that stdout + exit code
# match. Diverge => fail.
#
# This generalizes tools/test-llvm-driver.sh — that script gates
# the *driver* wiring (--backend flag, KAI_BACKEND env, clang
# detection, precedence) on a hard-coded fixture list. This script
# gates *every* fixture across the documented example dirs and
# demos.
#
# Skip discipline (per #575 acceptance):
#   1. tools/backend-parity-skips.txt — one line per skipped
#      fixture: <relative-path>:<issue-number>:<one-line-reason>
#      File a separate issue first; the skip is the bookmark, the
#      issue is the work.
#   2. Inline annotation: a fixture whose first line contains
#      "// skip-backend-parity" is skipped silently. Use only when
#      the fixture is *intentionally* backend-specific (e.g. a
#      feature only one backend implements by design).
#
# Entry-point detection: kaikai fixtures live in two shapes —
#   - flat:    <dir>/<name>.kai is a standalone program with main.
#   - package: <dir>/<pkg>/main.kai is the entry-point; sibling
#              files are libraries loaded by main.
# We walk depth-2 *.kai for flat shape and **/main.kai for the
# package shape. Library files (e.g. lib_greet/greet.kai) are
# never compiled standalone — they would always C-FAIL at link
# time (no kai_main symbol) and that's not a backend-parity issue.

set -eu

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAI="$ROOT/bin/kai"

if ! command -v "${CLANG:-clang}" >/dev/null 2>&1; then
  echo "test-backend-parity: SKIP (clang not in PATH)"
  exit 0
fi

if [ ! -x "$KAI" ]; then
  echo "test-backend-parity FAIL — bin/kai not found or not executable"
  exit 1
fi

# Directories to walk. examples/negative is intentionally absent:
# those fixtures must reject at compile time, so backend-parity
# (which assumes both build cleanly) does not apply.
DIRS="examples/effects
examples/actors
examples/spawn
examples/perceus
examples/refinements
examples/llvm
examples/packages
examples/minimal
examples/quickstart
examples/stdlib
examples/attributes
examples/unstable
demos"

SKIPS_FILE="$ROOT/tools/backend-parity-skips.txt"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

# Per-fixture timeout (seconds) for the binary execution step.
# Build steps inherit no explicit timeout; if the compiler hangs
# the CI job timeout catches it.
RUN_TIMEOUT="${BACKEND_PARITY_RUN_TIMEOUT:-30}"

# timeout(1) is GNU on Linux, ships separately on macOS. Fall back
# to gtimeout if present, otherwise run directly (CI has GNU
# coreutils; local mac may not).
if command -v timeout >/dev/null 2>&1; then
  TIMEOUT_CMD="timeout"
elif command -v gtimeout >/dev/null 2>&1; then
  TIMEOUT_CMD="gtimeout"
else
  TIMEOUT_CMD=""
fi

run_with_timeout() {
  if [ -n "$TIMEOUT_CMD" ]; then
    "$TIMEOUT_CMD" "$RUN_TIMEOUT" "$@"
  else
    "$@"
  fi
}

# Collect entry points into $tmp/entry-points (one path per line).
collect_entry_points() {
  for dir in $DIRS; do
    [ -d "$dir" ] || continue
    # Flat shape: *.kai immediately under $dir (depth 1 from $dir).
    find "$dir" -maxdepth 1 -name "*.kai" -not -name "*.err.kai" 2>/dev/null
    # Package shape: <dir>/<pkg>/main.kai (depth >= 2).
    find "$dir" -mindepth 2 -name "main.kai" 2>/dev/null
  done | sort -u > "$tmp/entry-points"
}

is_skipped() {
  fixture="$1"
  if [ -f "$SKIPS_FILE" ] && grep -q "^${fixture}:" "$SKIPS_FILE" 2>/dev/null; then
    return 0
  fi
  # Inline annotation: first line contains "// skip-backend-parity".
  if [ -r "$fixture" ] && head -1 "$fixture" | grep -q "// skip-backend-parity"; then
    return 0
  fi
  # Negative-by-design: a sibling `.err.expected` file declares this
  # fixture is meant to reject at compile time. Some negative fixtures
  # live inside positive dirs (e.g. examples/stdlib/map_assign_error.kai
  # documents the v1 rejection of indexed-map-write). They are exercised
  # by tools/test-negative.sh against their golden — not by parity.
  case "$fixture" in
    */main.kai)
      dir="${fixture%/main.kai}"
      if [ -f "$dir/main.err.expected" ]; then
        return 0
      fi
      ;;
  esac
  base="${fixture%.kai}"
  if [ -f "$base.err.expected" ] || [ -f "$base.diag.expected" ] || [ -f "$base.run.err.expected" ]; then
    return 0
  fi
  return 1
}

collect_entry_points

# Jobs: number of parallel workers. Defaults to the host's logical CPU count;
# override via BACKEND_PARITY_JOBS. macOS uses sysctl, Linux uses nproc.
if [ -n "${BACKEND_PARITY_JOBS:-}" ]; then
  JOBS="$BACKEND_PARITY_JOBS"
elif command -v nproc >/dev/null 2>&1; then
  JOBS="$(nproc)"
elif command -v sysctl >/dev/null 2>&1; then
  JOBS="$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"
else
  JOBS=4
fi

failures="$tmp/failures.log"
results="$tmp/results"   # one line per fixture: P|F|S
: > "$failures"
: > "$results"

# Worker — invoked once per fixture via `xargs -P $JOBS -n 1`.
# Side-effects: appends one of "P", "F", "S" to $results; appends a
# multi-line failure block to $failures on F. Read-only otherwise.
# Single-line appends to the counter file are atomic on POSIX up to
# PIPE_BUF (>= 512 bytes); the 1-char results and the diff-bounded
# failure blocks both fit.
process_one() {
  f="$1"
  if is_skipped "$f"; then
    printf 'S\n' >> "$results"
    return 0
  fi

  slug=$(echo "$f" | tr '/' '_' | tr '.' '_')
  c_bin="$tmp/${slug}.c"
  l_bin="$tmp/${slug}.llvm"
  c_blog="$tmp/${slug}.c.build.log"
  l_blog="$tmp/${slug}.llvm.build.log"

  if ! KAI_BACKEND=c "$KAI" build "$f" -o "$c_bin" >"$c_blog" 2>&1; then
    {
      echo "FAIL $f — C build failed:"
      tail -10 "$c_blog" | sed 's/^/    /'
      echo
    } >> "$failures"
    printf 'F\n' >> "$results"
    return 0
  fi

  if ! KAI_BACKEND=llvm "$KAI" build "$f" -o "$l_bin" >"$l_blog" 2>&1; then
    {
      echo "FAIL $f — LLVM build failed:"
      tail -10 "$l_blog" | sed 's/^/    /'
      echo
    } >> "$failures"
    printf 'F\n' >> "$results"
    return 0
  fi

  c_out_file="$tmp/${slug}.c.out"
  l_out_file="$tmp/${slug}.llvm.out"
  c_rc=0
  run_with_timeout "$c_bin" >"$c_out_file" 2>&1 </dev/null || c_rc=$?
  l_rc=0
  run_with_timeout "$l_bin" >"$l_out_file" 2>&1 </dev/null || l_rc=$?

  if [ "$c_rc" != "$l_rc" ]; then
    {
      echo "FAIL $f — exit code mismatch (C=$c_rc, LLVM=$l_rc)"
      echo "  C output (first 5 lines):"
      head -5 "$c_out_file" | sed 's/^/    /'
      echo "  LLVM output (first 5 lines):"
      head -5 "$l_out_file" | sed 's/^/    /'
      echo
    } >> "$failures"
    printf 'F\n' >> "$results"
    return 0
  fi

  if ! diff -q "$c_out_file" "$l_out_file" >/dev/null 2>&1; then
    {
      echo "FAIL $f — output mismatch (exit code matched: $c_rc):"
      diff -u "$c_out_file" "$l_out_file" | head -20 | sed 's/^/    /'
      echo
    } >> "$failures"
    printf 'F\n' >> "$results"
    return 0
  fi

  printf 'P\n' >> "$results"
}

# Export the worker + the helpers and state it touches so xargs can
# reach them from each forked subshell.
export KAI tmp failures results SKIPS_FILE RUN_TIMEOUT TIMEOUT_CMD
export -f process_one is_skipped run_with_timeout

# Total fixtures (pre-skip).
total=$(wc -l < "$tmp/entry-points" | tr -d ' ')

echo "test-backend-parity: walking $total fixtures with $JOBS workers..."

# xargs spawns N parallel `sh -c 'process_one <fixture>'` calls,
# each fixture goes to whichever worker is free. Output ordering is
# non-deterministic but the failure log and counter file capture
# everything; the final summary is deterministic.
xargs -P "$JOBS" -n 1 -I{} sh -c 'process_one "$@"' _ {} < "$tmp/entry-points"

pass=$(grep -c '^P$' "$results" 2>/dev/null || echo 0)
fail=$(grep -c '^F$' "$results" 2>/dev/null || echo 0)
skip=$(grep -c '^S$' "$results" 2>/dev/null || echo 0)

echo ""
echo "test-backend-parity: pass=$pass fail=$fail skip=$skip total=$total"

if [ "$fail" -gt 0 ]; then
  echo ""
  echo "--- failures ---"
  cat "$failures"
  exit 1
fi
