#!/usr/bin/env bash
# M:N corpus-determinism gate — the whole fixture corpus at KAI_THREADS>1.
#
# The existing M:N gates run ~9 hand-authored `mn_*` stress fixtures. None of
# them asks whether an ORDINARY program prints the same thing at N=4 that it
# prints at N=1, so nothing exercised the corpus above one thread while the
# default was one thread. This gate closes that: for every entry-point
# fixture, on every backend, the N=1 run is the reference and every N>1 run
# must reproduce its stdout AND its exit code.
#
# It is the intersection of two harnesses that already existed:
#   * the corpus walk of tools/test-backend-parity.sh (now tools/lib/corpus.sh)
#   * the N=1-vs-N>1 diff loop of tools/run-mn-determinism.sh
#
# Two observation conditions are load-bearing and must be preserved:
#
#   * stdout goes to a FILE, never a pipe. Command substitution is a pipe,
#     and a pipe (like a tty) changes the buffering enough to close the
#     window: a fixture that fails on a majority of runs redirected to a
#     file can be clean on every run through a pipe.
#   * every run is BOUNDED. A wedged scheduler must be counted as a hang,
#     not absorbed into the job ceiling where it reads as a cancelled run.
#
# Six buckets, never collapsed — a hang, an empty output, an exit-code
# mismatch and a stdout divergence are four different bugs:
#
#   ok       stdout and exit code reproduce the N=1 reference
#   hang     the bounded run hit its deadline (124/137)
#   crash    exit code differs from the reference (SIGSEGV, trap, abort)
#   empty    exit code matched but stdout was empty and the reference was not
#   diverge  exit code matched, stdout non-empty, contents differ
#   reorder  same lines as the reference, different order, on a fixture that
#            declares its interleaving unspecified (see below)
#
# A concurrency demo that prints from two fibers has no specified line ORDER:
# at N=1 the round-robin scheduler alternates, at N=4 the fibers really run
# at once. Comparing its byte sequence against N=1 asserts something the
# language never promised. Such a fixture declares itself in
# tools/mn-corpus-order-dependent.txt, and the gate then compares the
# MULTISET of its output lines instead of the sequence — every line still has
# to appear, exactly once, so lost work, a duplicated message and a torn line
# are all still findings. Only the order is forgiven, only where declared,
# and every tolerated reorder is counted in the summary.
#
# `diverge` and `empty` are adjudicated against N=1 self-stability before
# they are reported: a fixture whose own N=1 output varies between runs
# (addresses, hash order, wall-clock) cannot witness anything about thread
# count, so it is dropped and LOGGED rather than reported as a finding.
# `crash` and `hang` stand regardless — no amount of output nondeterminism
# explains a SIGSEGV that only appears above one thread.
#
# BOUNDED COVERAGE, stated rather than sampled silently. This gate walks the
# {c, native} backends via the shipped `bin/kai` build path. The single-TU
# -O2 arm (the stage2 Makefile's own path, where scheduler races reproduce
# most readily) stays with tools/run-mn-determinism.sh, which pays for it on
# 9 fixtures instead of ~620. Everything the walk drops — build failures,
# unusable references, nondeterministic fixtures — is listed by name in the
# summary.

set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAI="$ROOT/bin/kai"

. "$ROOT/tools/lib/timeout.sh"
. "$ROOT/tools/lib/corpus.sh"

BACKENDS="${MN_CORPUS_BACKENDS:-c native}"
THREAD_COUNTS="${MN_CORPUS_THREADS:-4}"
REPEATS="${MN_CORPUS_REPEATS:-2}"
RUN_TIMEOUT="${MN_CORPUS_RUN_TIMEOUT:-30}"
# Consecutive N=1 re-runs a diverging fixture must survive before its
# divergence is believed. The bar is asymmetric on purpose: one matching
# re-run is not evidence of stability, and a fixture that varies at N=1 is
# dropped, not reported.
REF_RECHECKS="${MN_CORPUS_REF_RECHECKS:-2}"
KAI_CORPUS_DIRS="${MN_CORPUS_DIRS:-$KAI_CORPUS_DEFAULT_DIRS}"
# Whether this run walks a subset. A shard cannot distinguish a baseline
# entry that left the corpus from one that belongs to a sibling shard.
SHARDED=0; [ -z "${MN_CORPUS_DIRS:-}" ] || SHARDED=1
ORDER_FILE="${MN_CORPUS_ORDER_FILE:-$ROOT/tools/mn-corpus-order-dependent.txt}"
BASELINE="${MN_CORPUS_BASELINE:-$ROOT/tools/mn-corpus-baseline.txt}"

if [ ! -x "$KAI" ]; then
  echo "run-mn-corpus-determinism FAIL — bin/kai not found or not executable"
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

if [ "$KAI_TIMEOUT_KIND" = none ]; then
  echo "::warning::run-mn-corpus-determinism runs UNBOUNDED (no timeout/gtimeout/perl); a wedge will block instead of counting as a hang"
fi

slug_of() { echo "$1" | tr '/.' '__'; }

# A backend that cannot build a trivial program is dropped from the arm list
# with a loud line — never silently degraded into a pass.
usable_backends=""
for backend in $BACKENDS; do
  printf 'fn main() : Int = 0\n' > "$TMP/probe.kai"
  if KAI_BACKEND="$backend" "$KAI" build "$TMP/probe.kai" -o "$TMP/probe.bin" >/dev/null 2>&1; then
    usable_backends="$usable_backends $backend"
  else
    echo "run-mn-corpus-determinism: backend '$backend' unavailable in this checkout — arm dropped"
  fi
done
BACKENDS="${usable_backends# }"
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
  local backend="$1" f="$2" out
  out="$BIN/$(slug_of "$f").$backend"
  if ! KAI_BACKEND="$backend" KAI_NATIVE_MODULAR=0 "$KAI" build "$f" -o "$out" >/dev/null 2>&1; then
    printf '%s [%s]\n' "$f" "$backend" >> "$buildfail"
    rm -f "$out"
  fi
}
export KAI BIN buildfail
export -f build_one slug_of

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
# ---------------------------------------------------------------------------
findings="$TMP/findings"; : > "$findings"
reffail="$TMP/reffail"; : > "$reffail"
nondet="$TMP/nondet"; : > "$nondet"
reorders="$TMP/reorders"; : > "$reorders"

# One line of witness, newlines folded so a multi-line stdout stays greppable.
witness_of() { head -c 160 "$1" | tr '\n' '|'; }

# A fixture that has declared its line order unspecified.
is_order_dependent() {
  [ -f "$ORDER_FILE" ] && grep -q "^$1:" "$ORDER_FILE" 2>/dev/null
}

# Same lines, any order. The multiset comparison that replaces the sequence
# comparison for declared fixtures.
same_lines() {
  sort "$1" > "$TMP/cmp.a"
  sort "$2" > "$TMP/cmp.b"
  cmp -s "$TMP/cmp.a" "$TMP/cmp.b"
}

# Run $1 at KAI_THREADS=$2, stdout to $3 (a FILE — see the buffering note),
# stderr to $3.err. Echoes the exit status.
#
# stdin is /dev/null, and not optionally: the corpus contains fixtures that
# read stdin, and the measurement loop is a `while read` over the fixture
# list. Without this a single stdin-reading fixture swallows the rest of the
# corpus and the gate reports a confident pass over the handful it got to.
run_at() {
  local bin="$1" n="$2" out="$3" ec=0
  kai_timeout "$RUN_TIMEOUT" env KAI_THREADS="$n" "$bin" >"$out.raw" 2>"$out.err" </dev/null || ec=$?
  kai_corpus_normalize_timestamps <"$out.raw" >"$out"
  echo "$ec"
}

# True when the N=1 reference reproduces itself on $REF_RECHECKS consecutive
# re-runs. A fixture that fails this is nondeterministic on its own and
# cannot witness a thread-count effect.
ref_is_stable() {
  local bin="$1" ref="$2" ref_ec="$3" i ec
  for ((i = 0; i < REF_RECHECKS; i++)); do
    ec="$(run_at "$bin" 1 "$TMP/recheck")"
    [ "$ec" = "$ref_ec" ] || return 1
    cmp -s "$TMP/recheck" "$ref" || return 1
  done
  return 0
}

# The fixture list is read on FD 3, never stdin: fixtures that read stdin
# are part of the corpus, and on stdin one of them would swallow the rest of
# the list mid-walk.
judged=0
measure_start="$SECONDS"
for backend in $BACKENDS; do
  while IFS= read -r f <&3; do
    bin="$BIN/$(slug_of "$f").$backend"
    [ -x "$bin" ] || continue

    ref="$TMP/ref"
    ref_ec="$(run_at "$bin" 1 "$ref")"
    if [ "$ref_ec" = 124 ] || [ "$ref_ec" = 137 ]; then
      printf '%s [%s] — N=1 reference did not complete within %ss\n' "$f" "$backend" "$RUN_TIMEOUT" >> "$reffail"
      continue
    fi
    judged=$((judged + 1))

    for n in $THREAD_COUNTS; do
      ok=0; reorder=0; diverge=0; empty=0; crash=0; hang=0; witness=""
      for ((r = 0; r < REPEATS; r++)); do
        ec="$(run_at "$bin" "$n" "$TMP/run")"
        if [ "$ec" = 124 ] || [ "$ec" = 137 ]; then
          hang=$((hang + 1))
        elif [ "$ec" != "$ref_ec" ]; then
          crash=$((crash + 1))
          [ -n "$witness" ] || witness="exit $ec (N=1 exits $ref_ec): $(witness_of "$TMP/run.err")"
        elif [ ! -s "$TMP/run" ] && [ -s "$ref" ]; then
          empty=$((empty + 1))
        elif cmp -s "$TMP/run" "$ref"; then
          ok=$((ok + 1))
        elif is_order_dependent "$f" && same_lines "$TMP/run" "$ref"; then
          reorder=$((reorder + 1))
        else
          diverge=$((diverge + 1))
          [ -n "$witness" ] || witness="got '$(witness_of "$TMP/run")' want '$(witness_of "$ref")'"
        fi
      done
      if [ "$reorder" != 0 ]; then
        printf '%s [%s] N=%s — %s/%s runs reordered the same lines (declared order-dependent)\n' \
          "$f" "$backend" "$n" "$reorder" "$REPEATS" >> "$reorders"
      fi
      if [ "$((ok + reorder))" = "$REPEATS" ]; then
        continue
      fi

      # Headline bucket, worst first. crash/hang are believed immediately;
      # diverge/empty must first clear the N=1 self-stability check.
      if [ "$hang" -gt 0 ]; then bucket=hang
      elif [ "$crash" -gt 0 ]; then bucket=crash
      elif [ "$empty" -gt 0 ]; then bucket=empty
      else bucket=diverge
      fi
      if [ "$bucket" = diverge ] || [ "$bucket" = empty ]; then
        if ! ref_is_stable "$bin" "$ref" "$ref_ec"; then
          printf '%s [%s] — N=1 output varies between runs; %s at N=%s not judged\n' \
            "$f" "$backend" "$bucket" "$n" >> "$nondet"
          continue
        fi
      fi
      printf '%-7s %s [%s] N=%s: ok=%s reorder=%s diverge=%s empty=%s crash=%s hang=%s of %s%s\n' \
        "$bucket" "$f" "$backend" "$n" "$ok" "$reorder" "$diverge" "$empty" "$crash" "$hang" "$REPEATS" \
        "${witness:+ — $witness}" >> "$findings"
      [ -z "${GITHUB_ACTIONS:-}" ] || \
        echo "::error title=mn-corpus $bucket::$f [$backend] at KAI_THREADS=$n does not reproduce its N=1 behaviour"
    done
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
  if [ "$n" = 0 ]; then
    return 0
  fi
  echo ""
  echo "$label ($n):"
  sed 's/^/    /' "$file"
}

echo ""
echo "run-mn-corpus-determinism: judged $judged fixture-backend pairs at N in {$THREAD_COUNTS}"
echo "  wall-clock: ${build_secs}s building (parallel, $JOBS workers) + ${measure_secs}s measuring (serial)"
report_dropped "$buildfail" "did not build — NOT JUDGED"
report_dropped "$reffail" "N=1 reference unusable — NOT JUDGED"
report_dropped "$nondet" "nondeterministic at N=1 — NOT JUDGED"
report_dropped "$reorders" "line order forgiven — judged on the line multiset, no finding"

nfind="$(wc -l < "$findings" | tr -d ' ')"
if [ "$nfind" != 0 ]; then
  echo ""
  echo "--- findings ($nfind) ---"
  cat "$findings"
fi

# ---------------------------------------------------------------------------
# Ratchet. The corpus reached this gate with a finite queue of pre-existing
# defects, each with an open issue; the queue is allowed to shrink and
# nothing else. A finding whose (fixture, backend) is not in the baseline
# fails the gate — that is a regression, and it is the whole point.
#
# The BUCKET is not part of the key. A baselined pair whose crash becomes a
# hang is the same known-broken pair, and flapping the required check on the
# bucket a race happened to land in this run would cost more than it buys.
# The issue named on the baseline line carries that detail.
# ---------------------------------------------------------------------------
[ -f "$BASELINE" ] || { echo "run-mn-corpus-determinism FAIL — baseline missing: $BASELINE"; exit 1; }
# `|| true`: an all-comment baseline is the state this gate is working
# towards, and grep exiting 1 on no match would otherwise kill the run under
# `set -o pipefail` — silently, right before the verdict.
{ grep -vE '^[[:space:]]*(#|$)' "$BASELINE" || true; } | awk '{print $1" "$2}' | sort -u > "$TMP/base.keys"
awk '{print $2" "$3}' "$findings" | tr -d '[]' | sort -u > "$TMP/now.keys"

# A shard walks a subset, so a baseline entry it did not reach is out of
# scope for this run — not closed, and not stale. Comparing the shard's
# findings against the FULL baseline would report every entry outside its
# subset as a fix, which is the trap tools/test-backend-parity.sh documents.
# Entries whose backend arm was dropped are out of scope for the same reason.
: > "$TMP/base.inscope"
stale=""
while read -r fixture backend; do
  if ! grep -qxF "$fixture" "$TMP/fixtures"; then
    # Only a full-corpus run can tell a departed fixture from one that
    # simply belongs to another shard.
    if [ "$SHARDED" = 0 ]; then
      stale="$stale$fixture
"
    fi
  elif printf '%s\n' $BACKENDS | grep -qxF "$backend"; then
    printf '%s %s\n' "$fixture" "$backend" >> "$TMP/base.inscope"
  fi
done < "$TMP/base.keys"
sort -u -o "$TMP/base.inscope" "$TMP/base.inscope"

if [ -n "$stale" ]; then
  echo ""
  echo "run-mn-corpus-determinism FAIL — the baseline names fixtures that left the corpus:"
  printf '%s' "$stale" | sort -u | sed 's/^/    /'
  echo "  Remove the stale lines from $BASELINE — a baseline is not allowed to rot."
  exit 1
fi

new_findings="$(comm -23 "$TMP/now.keys" "$TMP/base.inscope")"
closed="$(comm -13 "$TMP/now.keys" "$TMP/base.inscope")"

if [ -n "$new_findings" ]; then
  echo ""
  echo "run-mn-corpus-determinism FAIL — NEW findings, not in the baseline:"
  printf '%s\n' "$new_findings" | sed 's/^/    + /'
  echo ""
  echo "  This program behaves differently above one thread than it does at"
  echo "  KAI_THREADS=1. Fix the runtime — do NOT add it to $BASELINE to"
  echo "  silence it. If its output LINE ORDER is genuinely unspecified,"
  echo "  declare it in tools/mn-corpus-order-dependent.txt with a reason;"
  echo "  the line multiset stays compared, so that forgives interleaving and"
  echo "  nothing else."
  exit 1
fi

if [ -n "$closed" ]; then
  echo ""
  echo "run-mn-corpus-determinism OK (improved) — these baselined pairs now"
  echo "reproduce their N=1 behaviour; tighten the ratchet:"
  printf '%s\n' "$closed" | sed 's/^/    - /'
  echo "  Remove the lines above from $BASELINE."
fi
echo ""
echo "run-mn-corpus-determinism: OK ($nfind finding(s), all at baseline)"
