#!/bin/bash
# Backend-parity harness — native vs C-direct (the oracle).
#
# bash (not sh): the xargs worker fan-out uses `export -f` to make
# the worker function visible to forked subshells. dash (Ubuntu's
# /bin/sh) rejects `export -f` as illegal; bash supports it.
#
# Build every entry-point fixture with the TARGET backend and the
# ORACLE backend, run the resulting binaries, and assert that stdout
# + exit code match. Diverge => fail. The default gates the in-process
# libLLVM `native` backend against C-direct (the portable oracle).
#
# SCOPE (design decision, 2026-05-27): this harness is FILE-MODE ONLY.
# It builds each fixture as a standalone file (`kai build <file>`),
# which does NOT read a kai.toml or resolve manifest dependencies. A
# fixture that needs package-mode (deps, git sources) cannot build
# here and is NOT a parity bug — its parity is covered by
# tools/test-packages.sh's `run_parity` (which builds in package mode
# with `kai run .` under both backends, where the git-fixture setup
# lives). The two harnesses split on the package/file axis on purpose;
# do not add package-mode logic here.
#
# Skip discipline:
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

# Parametrised axis. The harness diffs a TARGET backend against an
# ORACLE backend over the same corpus. The default gates the in-process
# libLLVM `native` backend against C-direct (the portable oracle); the
# tier1-native workflow adds NATIVE_PARITY_RATCHET=1 to forbid new gaps.
ORACLE_BACKEND="${ORACLE_BACKEND:-c}"
TARGET_BACKEND="${TARGET_BACKEND:-native}"

case "$TARGET_BACKEND" in
  native)
    # The native backend needs a kaic2 with libLLVM linked in; it does
    # NOT need clang at link time (cc links the emitted object). Probe
    # the capability instead of clang: a C-only kaic2 rejects --emit=native
    # with a known sentinel. SKIP (success) when native is unavailable —
    # the native gate runs only where libLLVM is present (tier1-native),
    # never silently degrading a C-only checkout to a pass.
    probe="$(mktemp)"; mv "$probe" "$probe.kai"; probe="$probe.kai"
    printf 'fn main() : Int = 0\n' > "$probe"
    if ! "$KAI" build --backend=native "$probe" -o "${probe%.kai}.bin" >/dev/null 2>&1; then
      echo "test-backend-parity: SKIP (native backend unavailable; rebuild kaic2 with make KAI_LLVM=1)"
      rm -f "$probe" "${probe%.kai}.bin"
      exit 0
    fi
    rm -f "$probe" "${probe%.kai}.bin"
    ;;
  *)
    if ! command -v "${CLANG:-clang}" >/dev/null 2>&1; then
      echo "test-backend-parity: SKIP (clang not in PATH)"
      exit 0
    fi
    ;;
esac

if [ ! -x "$KAI" ]; then
  echo "test-backend-parity FAIL — bin/kai not found or not executable"
  exit 1
fi

# Directories to walk. examples/negative is intentionally absent:
# those fixtures must reject at compile time, so backend-parity
# (which assumes both build cleanly) does not apply.
#
# Overridable via $BACKEND_PARITY_DIRS (newline- or space-separated) so CI
# can shard the corpus across runners (docs/ci-time-analysis.md §7). Each
# shard runs a disjoint subset; the union must equal this default so the
# gate's coverage is unchanged. Sharding is safe ONLY while the ratchet
# baseline (native-parity-baseline.txt) is empty: the new-gap check is
# per-fixture (a gap in a shard's subset fails that shard), but the
# closed-gap "suggest tightening" pass compares against the FULL baseline,
# so a non-empty baseline would mis-report baseline entries outside a
# shard's subset as closed. Keep the baseline empty, or de-shard, if that
# changes.
DIRS="${BACKEND_PARITY_DIRS:-examples/effects
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
demos}"

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

# Rewrite ISO-8601 UTC timestamps (the stdlib Log default handler's
# `[YYYY-MM-DDTHH:MM:SSZ] LEVEL message` form, docs/log.kai) to a fixed
# placeholder before any oracle/target comparison. Applied identically to
# both sides, so a real content divergence still fails; only the
# wall-clock second — which can differ between two runs straddling a
# second boundary under parallel load — is masked.
normalize_timestamps() {
  sed -E 's/\[[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\]/[TIMESTAMP]/g'
}

# Collect entry points into $tmp/entry-points (one path per line).
#
# `quarantine/` subdirs hold fixtures that DOCUMENT broken/accidental
# behaviour (no stable golden — silent corruption, segfault) and must
# not enter the parity corpus. The flat walk's -maxdepth 1 already skips
# any subdir, but the package walk is pruned explicitly so a future
# quarantine/main.kai cannot leak in.
collect_entry_points() {
  for dir in $DIRS; do
    [ -d "$dir" ] || continue
    # Flat shape: *.kai immediately under $dir (depth 1 from $dir).
    find "$dir" -maxdepth 1 -name "*.kai" -not -name "*.err.kai" 2>/dev/null
    # Package shape: <dir>/<pkg>/main.kai (depth >= 2).
    find "$dir" -mindepth 2 -name "main.kai" -not -path '*/quarantine/*' 2>/dev/null
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
  o_bin="$tmp/${slug}.oracle"
  t_bin="$tmp/${slug}.target"
  o_blog="$tmp/${slug}.oracle.build.log"
  t_blog="$tmp/${slug}.target.build.log"

  if ! KAI_BACKEND="$ORACLE_BACKEND" "$KAI" build "$f" -o "$o_bin" >"$o_blog" 2>&1; then
    {
      echo "FAIL $f — $ORACLE_BACKEND (oracle) build failed:"
      tail -10 "$o_blog" | sed 's/^/    /'
      echo
    } >> "$failures"
    printf 'F\n' >> "$results"
    return 0
  fi

  # Pin whole-program for the native target: this ratchet proves native-vs-C
  # SEMANTIC parity over hundreds of one-shot fixtures, and the partitioned path
  # would pay partition + per-object codegen + bc-merge on every one with no
  # cache to amortise it (a 30-min shard timeout). The modular path's own
  # equivalence is proved byte-for-byte by `test-native-modular`; this harness
  # need not re-pay for it. (KAI_NATIVE_MODULAR is a no-op on the C oracle.)
  if ! KAI_BACKEND="$TARGET_BACKEND" KAI_NATIVE_MODULAR=0 "$KAI" build "$f" -o "$t_bin" >"$t_blog" 2>&1; then
    {
      echo "FAIL $f — $TARGET_BACKEND (target) build failed:"
      tail -10 "$t_blog" | sed 's/^/    /'
      echo
    } >> "$failures"
    printf 'F\n' >> "$results"
    return 0
  fi

  o_out_file="$tmp/${slug}.oracle.out"
  t_out_file="$tmp/${slug}.target.out"
  o_rc=0
  run_with_timeout "$o_bin" >"$o_out_file" 2>&1 </dev/null || o_rc=$?
  t_rc=0
  run_with_timeout "$t_bin" >"$t_out_file" 2>&1 </dev/null || t_rc=$?
  normalize_timestamps <"$o_out_file" >"$o_out_file.norm" && mv "$o_out_file.norm" "$o_out_file"
  normalize_timestamps <"$t_out_file" >"$t_out_file.norm" && mv "$t_out_file.norm" "$t_out_file"

  if [ "$o_rc" != "$t_rc" ]; then
    {
      echo "FAIL $f — exit code mismatch ($ORACLE_BACKEND=$o_rc, $TARGET_BACKEND=$t_rc)"
      echo "  $ORACLE_BACKEND output (first 5 lines):"
      head -5 "$o_out_file" | sed 's/^/    /'
      echo "  $TARGET_BACKEND output (first 5 lines):"
      head -5 "$t_out_file" | sed 's/^/    /'
      echo
    } >> "$failures"
    printf 'F\n' >> "$results"
    return 0
  fi

  if ! diff -q "$o_out_file" "$t_out_file" >/dev/null 2>&1; then
    {
      echo "FAIL $f — output mismatch (exit code matched: $o_rc):"
      diff -u "$o_out_file" "$t_out_file" | head -20 | sed 's/^/    /'
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
export ORACLE_BACKEND TARGET_BACKEND
export -f process_one is_skipped run_with_timeout normalize_timestamps

# Total fixtures (pre-skip).
total=$(wc -l < "$tmp/entry-points" | tr -d ' ')

echo "test-backend-parity: $TARGET_BACKEND vs $ORACLE_BACKEND (oracle) — walking $total fixtures with $JOBS workers..."

# xargs spawns N parallel `bash -c 'process_one <fixture>'` calls,
# each fixture goes to whichever worker is free. Output ordering is
# non-deterministic but the failure log and counter file capture
# everything; the final summary is deterministic.
xargs -P "$JOBS" -n 1 -I{} bash -c 'process_one "$@"' _ {} < "$tmp/entry-points"

pass=$(grep -c '^P$' "$results" 2>/dev/null || true)
fail=$(grep -c '^F$' "$results" 2>/dev/null || true)
skip=$(grep -c '^S$' "$results" 2>/dev/null || true)

echo ""
echo "test-backend-parity: $TARGET_BACKEND vs $ORACLE_BACKEND — pass=$pass fail=$fail skip=$skip total=$total"

# Ratchet mode (Lane 1.5). With NATIVE_PARITY_RATCHET=1 the gate does NOT
# require zero failures — the native backend is at ~60% corpus parity and
# the flip is blocked on closing the rest. Instead it compares the set of
# failing fixtures against tools/native-parity-baseline.txt (the allowed
# gaps) and enforces the anti-regression contract:
#   - a fixture failing NOW that is NOT in the baseline => FAIL (a new gap).
#   - a baseline fixture that now PASSES => OK, prompt to remove it.
#   - failing set == baseline => OK at baseline.
# This locks the burn-down: lanes can only move the count DOWN.
if [ "${NATIVE_PARITY_RATCHET:-0}" = "1" ]; then
  baseline_file="$ROOT/tools/native-parity-baseline.txt"
  [ -f "$baseline_file" ] || { echo "ratchet FAIL — baseline missing: $baseline_file"; exit 1; }
  # Current failing fixtures, one path per line (extracted from the
  # failure log's `FAIL <path> — …` lines).
  grep '^FAIL ' "$failures" 2>/dev/null | awk '{print $2}' | sort -u > "$tmp/failing.now"
  # Allowed gaps: non-comment, non-blank lines of the baseline.
  grep -vE '^[[:space:]]*(#|$)' "$baseline_file" | sort -u > "$tmp/failing.base"
  # New gaps = failing now but not allowed. Closed gaps = allowed but
  # now passing.
  new_gaps="$(comm -23 "$tmp/failing.now" "$tmp/failing.base")"
  closed_gaps="$(comm -13 "$tmp/failing.now" "$tmp/failing.base")"
  # Flakiness guard. A handful of fixtures are NON-DETERMINISTIC under
  # --backend=native (output carrying raw pointers / addresses, RC or
  # hash-order dependence) — they pass on one run and fail the next. A
  # strict set-equality ratchet would fail CI at random on those, so a
  # candidate "new gap" is RE-VERIFIED before it is allowed to be called
  # flaky. The bar is asymmetric on purpose: a single converging run is
  # NOT evidence of flakiness — the parallel walk's evaluation order can
  # make a 100%-reproducible failure happen to converge once. A flaky
  # verdict therefore requires N CONSECUTIVE clean rechecks (a divergence
  # that reproduces even ONCE across the rechecks is a REAL gap, exactly
  # as it would be without any recheck). N=2 means a divergence is only
  # downgraded after THREE total confirmations the failure is gone (the
  # original walk plus N rechecks); a deterministic failure reproduces
  # well within two extra runs, while a genuine 1-in-many timing flake is
  # rare and the loud `flaky_warn` annotation covers it.
  PARITY_RECHECKS="${PARITY_RECHECKS:-2}"
  # Verdict codes: 0 = real gap (diverged at least once), 1 = flaky
  # (converged on all N rechecks), 2 = unjudgeable (oracle won't build
  # here). 2 keeps the caller's pre-recheck observation without emitting a
  # misleading flaky warning.
  recheck_diverges() {
    rf="$1"
    rob="$tmp/recheck.oracle"; rtb="$tmp/recheck.target"
    KAI_BACKEND="$ORACLE_BACKEND" "$KAI" build "$rf" -o "$rob" >/dev/null 2>&1 || return 2
    oref=0; orefout="$(run_with_timeout "$rob" 2>&1 </dev/null | normalize_timestamps)" || oref=$?
    attempt=0
    while [ "$attempt" -lt "$PARITY_RECHECKS" ]; do
      attempt=$((attempt + 1))
      if ! KAI_BACKEND="$TARGET_BACKEND" "$KAI" build "$rf" -o "$rtb" >/dev/null 2>&1; then
        return 0   # target build failed => a deterministic, real gap.
      fi
      rt=0; rtout="$(run_with_timeout "$rtb" 2>&1 </dev/null | normalize_timestamps)" || rt=$?
      if [ "$rt" != "$oref" ] || [ "$rtout" != "$orefout" ]; then
        return 0   # diverged on a recheck => a real (new) gap.
      fi
    done
    return 1       # converged on all N rechecks => flaky, not a regression.
  }
  # Surface a flaky verdict LOUDLY: a GitHub annotation under CI (so it
  # lands on the run summary, not buried in the log) plus a log line that
  # is unmistakable. A divergence that clears the recheck is still noise
  # we want visible, never swallowed silently into the green.
  flaky_warn() {
    fw="$1"
    msg="native-parity: $fw diverged on the initial walk but converged on $PARITY_RECHECKS consecutive rechecks — held as flaky, not counted as a regression. Investigate: a divergence this harness deems flaky is still a parity gap."
    if [ -n "${GITHUB_ACTIONS:-}" ]; then
      echo "::warning title=native-parity flaky::$msg"
    fi
    echo "ratchet WARNING (flaky): $msg"
  }
  real_new_gaps=""
  for g in $new_gaps; do
    verdict=0; recheck_diverges "$g" || verdict=$?
    case "$verdict" in
      0) real_new_gaps="$real_new_gaps$g
" ;;
      1) flaky_warn "$g" ;;
      *) echo "ratchet: $g — oracle ($ORACLE_BACKEND) failed to build on recheck; cannot judge, not counted as a new gap." ;;
    esac
  done
  if [ -n "$real_new_gaps" ]; then
    echo ""
    echo "ratchet FAIL — NEW native-parity gap(s) not in the baseline (confirmed on recheck):"
    printf '%s' "$real_new_gaps" | sed 's/^/    + /'
    echo ""
    echo "  These fixtures regressed under --backend=native. Fix the backend"
    echo "  (do NOT add them to tools/native-parity-baseline.txt to silence)."
    echo ""
    echo "--- failures ---"
    cat "$failures"
    exit 1
  fi
  # Symmetric flakiness guard for closed gaps: a baseline fixture that
  # passed THIS run might be a flaky gap that happened to converge, not a
  # genuinely-fixed one. Only suggest tightening the baseline for fixtures
  # that pass CONSISTENTLY — re-verify each candidate and keep it only if
  # it does NOT diverge on ANY of the N consecutive rechecks. A real fix
  # survives; a flaky gap (e.g. hex_basic at ~50%) is dropped from the
  # suggestion so a lane is not nudged to remove a gap that will reappear.
  stable_closed=""
  for g in $closed_gaps; do
    verdict=0; recheck_diverges "$g" || verdict=$?
    case "$verdict" in
      1) stable_closed="$stable_closed$g
" ;;                # passed all N rechecks => a genuine, stable fix.
      *) : ;;          # still diverges, or unjudgeable — do not suggest removal.
    esac
  done
  if [ -n "$stable_closed" ]; then
    echo ""
    echo "ratchet OK (improved) — these baseline gaps now PASS consistently; tighten the ratchet:"
    printf '%s' "$stable_closed" | sed 's/^/    - /'
    echo "  Remove the lines above from tools/native-parity-baseline.txt."
  else
    echo "ratchet OK — native-parity failures match the baseline ($(wc -l < "$tmp/failing.base" | tr -d ' ') gaps; flaky gaps held)."
  fi
  exit 0
fi

if [ "$fail" -gt 0 ]; then
  echo ""
  echo "--- failures ---"
  cat "$failures"
  exit 1
fi
