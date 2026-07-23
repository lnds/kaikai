#!/usr/bin/env bash
# M:N corpus-determinism gate — the whole fixture corpus at KAI_THREADS>1.
#
# The other M:N gates run ~9 hand-authored `mn_*` stress fixtures. None of
# them asks whether an ORDINARY program prints the same thing at N=4 that it
# prints at N=1, so nothing exercised the corpus above one thread while the
# default was one thread. This gate closes that: for every entry-point
# fixture, on every backend, the N=1 run is the reference and every N>1 run
# must reproduce its stdout AND its exit code.
#
# It is the intersection of two harnesses that already existed — the corpus
# walk of tools/test-backend-parity.sh and the N=1-vs-N>1 diff loop of
# tools/run-mn-determinism.sh — and it keeps both of the latter's
# load-bearing observation conditions: stdout to a FILE rather than a pipe,
# and every run bounded so a wedge counts as a hang.
#
# This file is the driver. The parts it orchestrates:
#
#   tools/lib/corpus.sh      the entry-point walk and skip rules
#   tools/lib/mn-observe.sh  running and classifying one run
#   tools/lib/mn-measure.sh  judging one (fixture, backend) pair
#   tools/lib/mn-ratchet.sh  the verdict against the known-findings baseline
#
# BOUNDED COVERAGE, stated rather than sampled silently. The gate walks the
# {c, native} backends through the shipped `bin/kai` build path. The
# single-TU -O2 arm (the stage2 Makefile's own path, where scheduler races
# reproduce most readily) stays with tools/run-mn-determinism.sh, which pays
# for it on 9 fixtures instead of ~650. Everything the walk drops — build
# failures, unusable references, fixtures that vary at N=1, held flakes — is
# listed by name in the summary, and the walk checks its own arithmetic.

set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAI="$ROOT/bin/kai"

BACKENDS="${MN_CORPUS_BACKENDS:-c native}"
THREAD_COUNTS="${MN_CORPUS_THREADS:-4}"
REPEATS="${MN_CORPUS_REPEATS:-2}"
RUN_TIMEOUT="${MN_CORPUS_RUN_TIMEOUT:-30}"
# Consecutive N=1 re-runs a diverging fixture must survive before its
# divergence is believed, and consecutive re-runs at N the divergence must
# fail to reappear on before it is held as a flake. Both bars are asymmetric
# on purpose — see tools/lib/mn-observe.sh.
REF_RECHECKS="${MN_CORPUS_REF_RECHECKS:-2}"
RECHECKS="${MN_CORPUS_RECHECKS:-2}"
KAI_CORPUS_DIRS="${MN_CORPUS_DIRS:-}"
# Whether this run walks a subset. A shard cannot distinguish a baseline
# entry that left the corpus from one that belongs to a sibling shard.
SHARDED=0; [ -z "$KAI_CORPUS_DIRS" ] || SHARDED=1
ORDER_FILE="${MN_CORPUS_ORDER_FILE:-$ROOT/tools/mn-corpus-order-dependent.txt}"
BASELINE="${MN_CORPUS_BASELINE:-$ROOT/tools/mn-corpus-baseline.txt}"

. "$ROOT/tools/lib/timeout.sh"
. "$ROOT/tools/lib/corpus.sh"
. "$ROOT/tools/lib/mn-observe.sh"
. "$ROOT/tools/lib/mn-measure.sh"
. "$ROOT/tools/lib/mn-ratchet.sh"

if [ ! -x "$KAI" ]; then
  echo "run-mn-corpus-determinism FAIL — bin/kai not found or not executable"
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

if [ "$KAI_TIMEOUT_KIND" = none ]; then
  echo "::warning::run-mn-corpus-determinism runs UNBOUNDED (no timeout/gtimeout/perl); a wedge will block instead of counting as a hang"
fi

# A backend that cannot build a trivial program is dropped from the arm list
# with a loud line — never silently degraded into a pass.
usable_backends() {
  local backend usable=""
  printf 'fn main() : Int = 0\n' > "$TMP/probe.kai"
  for backend in $BACKENDS; do
    if KAI_BACKEND="$backend" "$KAI" build "$TMP/probe.kai" -o "$TMP/probe.bin" >/dev/null 2>&1; then
      usable="$usable $backend"
    else
      echo "run-mn-corpus-determinism: backend '$backend' unavailable in this checkout — arm dropped" >&2
    fi
  done
  echo "${usable# }"
}
BACKENDS="$(usable_backends)"
if [ -z "$BACKENDS" ]; then
  echo "run-mn-corpus-determinism: SKIP (no usable backend; rebuild kaic2, e.g. make KAI_LLVM=1 kaic2)"
  exit 0
fi

kai_corpus_entry_points > "$TMP/entry-points"
: > "$TMP/fixtures"
while IFS= read -r f; do
  kai_corpus_is_skipped "$f" || printf '%s\n' "$f" >> "$TMP/fixtures"
done < "$TMP/entry-points"
total="$(wc -l < "$TMP/entry-points" | tr -d ' ')"
walked="$(wc -l < "$TMP/fixtures" | tr -d ' ')"

echo "run-mn-corpus-determinism: backends=[$BACKENDS] N=1 vs N in {$THREAD_COUNTS}, $REPEATS repeats"
echo "  corpus: $walked fixtures ($((total - walked)) skipped by tools/backend-parity-skips.txt and negative-by-design goldens)"

# ---------------------------------------------------------------------------
# Build phase — parallel. Compilation is not a measurement, so it may use the
# whole machine; nothing timed happens until every binary exists.
# ---------------------------------------------------------------------------
BIN="$TMP/bin"; mkdir -p "$BIN"
buildfail="$TMP/buildfail"; : > "$buildfail"

build_one() {
  local backend="$1" fixture="$2" out
  out="$BIN/$(mn_slug_of "$fixture").$backend"
  if ! KAI_BACKEND="$backend" KAI_NATIVE_MODULAR=0 "$KAI" build "$fixture" -o "$out" >/dev/null 2>&1; then
    printf '%s [%s]\n' "$fixture" "$backend" >> "$buildfail"
    rm -f "$out"
  fi
}
export KAI BIN buildfail
export -f build_one mn_slug_of

if [ -n "${MN_CORPUS_BUILD_JOBS:-}" ]; then
  JOBS="$MN_CORPUS_BUILD_JOBS"
elif command -v nproc >/dev/null 2>&1; then
  JOBS="$(nproc)"
else
  JOBS="$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"
fi

for backend in $BACKENDS; do
  sed "s|^|$backend |" "$TMP/fixtures"
done > "$TMP/worklist"
echo "  building $(wc -l < "$TMP/worklist" | tr -d ' ') binaries with $JOBS workers..."
build_start="$SECONDS"
xargs -P "$JOBS" -n 2 bash -c 'build_one "$1" "$2"' _ < "$TMP/worklist"
build_secs=$((SECONDS - build_start))

# ---------------------------------------------------------------------------
# Measurement phase — STRICTLY SERIAL, one process at a time. Concurrency
# here would change the very scheduling being measured.
#
# The fixture list is read on FD 3, never stdin: fixtures that read stdin are
# part of the corpus, and on stdin one of them would swallow the rest of the
# list mid-walk.
# ---------------------------------------------------------------------------
findings="$TMP/findings"; : > "$findings"
reffail="$TMP/reffail";   : > "$reffail"
nondet="$TMP/nondet";     : > "$nondet"
reorders="$TMP/reorders"; : > "$reorders"
flaky="$TMP/flaky";       : > "$flaky"
pinned="$TMP/pinned";     : > "$pinned"

judged=0
measure_start="$SECONDS"
for backend in $BACKENDS; do
  while IFS= read -r f <&3; do
    bin="$BIN/$(mn_slug_of "$f").$backend"
    [ -x "$bin" ] || continue
    mn_measure_pair "$bin" "$f" "$backend"
    judged=$((judged + MN_JUDGED))
  done 3< "$TMP/fixtures"
done
measure_secs=$((SECONDS - measure_start))

# Every fixture must have been reached on every arm. A truncated walk still
# produces a tidy "no findings" report, so the arithmetic is checked rather
# than assumed — a gate that quietly measured a tenth of the corpus is worse
# than one that fails.
nbackends="$(printf '%s\n' $BACKENDS | wc -l | tr -d ' ')"
expected=$((walked * nbackends))
attempted=$((judged + $(wc -l < "$reffail") + $(wc -l < "$buildfail")))
if [ "$attempted" != "$expected" ]; then
  echo ""
  echo "run-mn-corpus-determinism FAIL — the walk reached $attempted of $expected fixture-backend pairs."
  echo "  The corpus was truncated mid-walk; no verdict from this run is trustworthy."
  exit 1
fi

# ---------------------------------------------------------------------------
# Report. Everything dropped is named; a bounded corpus is stated, not implied.
# ---------------------------------------------------------------------------
report_dropped() {
  local file="$1" label="$2" n
  n="$(wc -l < "$file" | tr -d ' ')"
  if [ "$n" != 0 ]; then
    echo ""
    echo "$label ($n):"
    sed 's/^/    /' "$file"
  fi
}

echo ""
echo "run-mn-corpus-determinism: judged $judged fixture-backend pairs at N in {$THREAD_COUNTS}"
echo "  wall-clock: ${build_secs}s building (parallel, $JOBS workers) + ${measure_secs}s measuring (serial)"
report_dropped "$buildfail" "did not build — NOT JUDGED"
report_dropped "$reffail" "N=1 reference unusable — NOT JUDGED"
report_dropped "$nondet" "nondeterministic at N=1 — NOT JUDGED"
report_dropped "$pinned" "thread count pinned by the fixture's env file — measured at the pin, not at N>1"
report_dropped "$reorders" "line order forgiven — judged on the line multiset, no finding"
report_dropped "$flaky" "diverged once, did not reproduce — HELD AS A FLAKE, still a divergence"

nfind="$(wc -l < "$findings" | tr -d ' ')"
if [ "$nfind" != 0 ]; then
  echo ""
  echo "--- findings ($nfind) ---"
  cat "$findings"
fi

if ! mn_ratchet_verdict "$findings"; then
  echo ""
  echo "run-mn-corpus-determinism: FAIL"
  exit 1
fi
echo ""
echo "run-mn-corpus-determinism: OK ($nfind finding(s), all at baseline)"
