#!/bin/bash
# Backend-parity harness — native vs C-direct (the oracle). With
# TARGET_BACKEND=wasm the target is the C backend built for wasm32 and run
# under node with stage2/web/kai_host.mjs (needs WASI_SDK and a node with
# exnref; WASM_NODE_FLAGS carries the flag an older node wants).
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
# A fixture whose directory holds a kai.toml is built as a package
# (`kai build .` from that directory, dependencies resolved into a scratch
# cache); anything else is built as a standalone file. The fixtures that
# cannot be built from the tree alone are listed in the skips file.
#
# The entry-point walk and the skip discipline live in tools/lib/corpus.sh,
# shared with the M:N corpus-determinism gate so the two cannot drift into
# covering different corpora.

set -eu

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAI="$ROOT/bin/kai"

. "$ROOT/tools/lib/corpus.sh"

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
    # never silently degrading a C-only checkout to a pass. This SKIP is
    # only honest when nothing upstream REMOVED native first; the
    # `tier1-backend-parity` target guarantees that via
    # tools/parity-preserve-native.sh.
    probe="$(mktemp)"; mv "$probe" "$probe.kai"; probe="$probe.kai"
    printf 'fn main() : Int = 0\n' > "$probe"
    if ! "$KAI" build --backend=native "$probe" -o "${probe%.kai}.bin" >/dev/null 2>&1; then
      echo "test-backend-parity: SKIP (native backend unavailable; rebuild kaic2 with make KAI_LLVM=1)"
      rm -f "$probe" "${probe%.kai}.bin"
      exit 0
    fi
    rm -f "$probe" "${probe%.kai}.bin"
    ;;
  wasm)
    if [ ! -x "${WASI_SDK:-/nonexistent}/bin/clang" ] || ! command -v node >/dev/null 2>&1; then
      echo "test-backend-parity: SKIP (wasm needs WASI_SDK=<wasi-sdk dir> and node)"
      exit 0
    fi
    WASM_CC="$WASI_SDK/bin/clang"
    WASM_CFLAGS="--target=wasm32-wasip1 -O2 -mexception-handling -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_GETPID -lwasi-emulated-signal -lwasi-emulated-mman -lwasi-emulated-process-clocks -lwasi-emulated-getpid -lsetjmp -Wl,--export=__stack_pointer -Wl,--export-table -Wno-unused-command-line-argument"
    WASM_NODE_FLAGS="${WASM_NODE_FLAGS:-}"
    WASM_HOST="$ROOT/stage2/web/kai_host.mjs"
    export WASM_CC WASM_CFLAGS WASM_NODE_FLAGS WASM_HOST
    ;;
  *)
    if ! command -v "${CLANG:-clang}" >/dev/null 2>&1; then
      echo "test-backend-parity: SKIP (clang not in PATH)"
      exit 0
    fi
    ;;
esac

# The target side. Under wasm it is the C backend for wasm32 on one thread,
# executed by the JavaScript host.
target_build() {
  if [ "$TARGET_BACKEND" = wasm ]; then
    kai_corpus_build "$1" "$2" KAI_BACKEND=c KAI_MODULAR=0 CC="$WASM_CC" CFLAGS="$WASM_CFLAGS"
  else
    local nopt
    nopt="$(kai_corpus_native_opt "$1")" || nopt=""
    kai_corpus_build "$1" "$2" ${nopt:+KAI_NATIVE_OPT="$nopt"} KAI_BACKEND="$TARGET_BACKEND" KAI_NATIVE_MODULAR=0
  fi
}

target_exec() {
  if [ "$TARGET_BACKEND" = wasm ]; then
    # node before 24.15 needs a flag for JSPI, which fibers and kept continuations run on.
    # The first attempt keeps stdout and stderr interleaved in one file, in the order written.
    local out rc
    out="$(mktemp)"
    # shellcheck disable=SC2086
    node $WASM_NODE_FLAGS "$WASM_HOST" "$1" >"$out" 2>&1
    rc=$?
    if [ "$rc" -ne 0 ] && grep -q "Promise Integration" "$out"; then
      # shellcheck disable=SC2086
      node --experimental-wasm-jspi $WASM_NODE_FLAGS "$WASM_HOST" "$1"
      rc=$?
    else
      cat "$out"
    fi
    rm -f "$out"
    return "$rc"
  else
    "$1"
  fi
}

parity_pin() {
  local p
  p="$(kai_corpus_pinned_threads "$1")" || p=""
  if [ "$TARGET_BACKEND" = wasm ]; then p="${p:-1}"; fi
  echo "$p"
}

PARITY_BASELINE="$ROOT/tools/native-parity-baseline.txt"
if [ "$TARGET_BACKEND" = wasm ]; then PARITY_BASELINE="$ROOT/tools/wasm-parity-baseline.txt"; fi

if [ ! -x "$KAI" ]; then
  echo "test-backend-parity FAIL — bin/kai not found or not executable"
  exit 1
fi

# Shard the corpus across runners via $BACKEND_PARITY_DIRS (newline- or
# space-separated; docs/ci-time-analysis.md §7). Each shard runs a disjoint
# subset; the union must equal the default in tools/lib/corpus.sh so the
# gate's coverage is unchanged. Sharding is safe ONLY while the ratchet
# baseline (native-parity-baseline.txt) is empty: the new-gap check is
# per-fixture (a gap in a shard's subset fails that shard), but the
# closed-gap "suggest tightening" pass compares against the FULL baseline,
# so a non-empty baseline would mis-report baseline entries outside a
# shard's subset as closed. Keep the baseline empty, or de-shard, if that
# changes.
KAI_CORPUS_DIRS="${BACKEND_PARITY_DIRS:-$KAI_CORPUS_DEFAULT_DIRS}"

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

kai_corpus_render_manifests
KAI_CORPUS_PKG_CACHE="$tmp/pkg-cache"
mkdir -p "$KAI_CORPUS_PKG_CACHE"
export KAI_CORPUS_PKG_CACHE

kai_corpus_entry_points > "$tmp/entry-points"

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
  if kai_corpus_is_skipped "$f"; then
    printf 'S\n' >> "$results"
    return 0
  fi

  slug=$(echo "$f" | tr '/' '_' | tr '.' '_')
  o_bin="$tmp/${slug}.oracle"
  t_bin="$tmp/${slug}.target"
  o_blog="$tmp/${slug}.oracle.build.log"
  t_blog="$tmp/${slug}.target.build.log"

  if ! kai_corpus_build "$f" "$o_bin" KAI_BACKEND="$ORACLE_BACKEND" >"$o_blog" 2>&1; then
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
  if ! target_build "$f" "$t_bin" >"$t_blog" 2>&1; then
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
  pin="$(parity_pin "$f")"
  o_rc=0
  run_with_timeout env ${pin:+KAI_THREADS="$pin"} "$o_bin" >"$o_out_file" 2>&1 </dev/null || o_rc=$?
  t_rc=0
  run_with_timeout env ${pin:+KAI_THREADS="$pin"} bash -c 'target_exec "$1"' _ "$t_bin" >"$t_out_file" 2>&1 </dev/null || t_rc=$?
  kai_corpus_normalize_timestamps <"$o_out_file" >"$o_out_file.norm" && mv "$o_out_file.norm" "$o_out_file"
  kai_corpus_normalize_timestamps <"$t_out_file" >"$t_out_file.norm" && mv "$t_out_file.norm" "$t_out_file"

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
export KAI tmp failures results KAI_CORPUS_SKIPS RUN_TIMEOUT TIMEOUT_CMD
export ORACLE_BACKEND TARGET_BACKEND
export -f target_build target_exec parity_pin process_one kai_corpus_build kai_corpus_is_skipped kai_corpus_pinned_threads kai_corpus_native_opt run_with_timeout kai_corpus_normalize_timestamps

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
  baseline_file="$PARITY_BASELINE"
  [ -f "$baseline_file" ] || { echo "ratchet FAIL — baseline missing: $baseline_file"; exit 1; }
  # Current failing fixtures, one path per line (extracted from the
  # failure log's `FAIL <path> — …` lines).
  grep '^FAIL ' "$failures" 2>/dev/null | awk '{print $2}' | sort -u > "$tmp/failing.now"
  # Allowed gaps: non-comment, non-blank lines of the baseline.
  grep -vE '^[[:space:]]*(#|$)' "$baseline_file" | sort -u > "$tmp/failing.all"
  : > "$tmp/failing.base"
  for d in $KAI_CORPUS_DIRS; do grep "^$d/" "$tmp/failing.all" >> "$tmp/failing.base" || true; done
  sort -u -o "$tmp/failing.base" "$tmp/failing.base"
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
    kai_corpus_build "$rf" "$rob" KAI_BACKEND="$ORACLE_BACKEND" >/dev/null 2>&1 || return 2
    pin="$(parity_pin "$rf")"
    oref=0; orefout="$(run_with_timeout env ${pin:+KAI_THREADS="$pin"} "$rob" 2>&1 </dev/null | kai_corpus_normalize_timestamps)" || oref=$?
    attempt=0
    while [ "$attempt" -lt "$PARITY_RECHECKS" ]; do
      attempt=$((attempt + 1))
      if ! target_build "$rf" "$rtb" >/dev/null 2>&1; then
        return 0   # target build failed => a deterministic, real gap.
      fi
      rt=0; rtout="$(run_with_timeout env ${pin:+KAI_THREADS="$pin"} bash -c 'target_exec "$1"' _ "$rtb" 2>&1 </dev/null | kai_corpus_normalize_timestamps)" || rt=$?
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
