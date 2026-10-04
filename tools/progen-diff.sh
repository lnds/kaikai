#!/bin/bash
# Differential test over generated programs: native vs C (the oracle).
#
# For each seed, tools/progen emits one well-typed, terminating program;
# it is built with both backends, both binaries run under a timeout, and
# stdout and exit code are compared. No expected output is needed: a
# program that compiles on one backend only, or behaves differently on
# the two, is a compiler bug whatever the right answer is.
#
# Every seed lands in one class:
#   agree           same output, exit 0, checksum printed
#   output-differs  both exit alike, stdout differs
#   exit-differs    exit codes differ, neither a signal
#   crash           a binary died on a signal
#   timeout         a binary outlived the deadline
#   c-rejects       only the C backend fails to build it
#   native-rejects  only the native backend fails to build it
#   both-reject     neither builds it: the front-end, or the generator
#   bad-program     both agree on a non-zero exit or a missing checksum,
#                   or the generator itself failed: a generator bug
#
# A seed in any class but `agree` keeps its program, build logs, and
# outputs under <out>/findings/<seed>/. tools/progen-diff-skips.txt names
# the known bugs; a finding matching an entry is reported as known and
# does not fail the run. Exit status: 0 when every finding is known, 1 on
# a new one (each printed as a `NEW` line with its reproduction command),
# 2 when the run could not start.
#
# The corpus harness (tools/test-backend-parity.sh) is not reused: it
# passes a fixture whose two binaries fail alike, does not try the target
# once the oracle rejects, and keeps no outputs.
#
# Usage: tools/progen-diff.sh <first-seed> <count>
#   PROGEN_OUT      work directory        (stage2/build/progen-diff)
#   PROGEN_JOBS     parallel workers      (logical CPUs)
#   PROGEN_DEPTH    expression depth      (3)
#   PROGEN_TIMEOUT  seconds per binary    (20)
#   PROGEN_BUDGET   stop starting seeds after this many seconds (unbounded)

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
SKIPS="$ROOT/tools/progen-diff-skips.txt"
OUT="${PROGEN_OUT:-$ROOT/stage2/build/progen-diff}"
DEPTH="${PROGEN_DEPTH:-3}"
RUN_TIMEOUT="${PROGEN_TIMEOUT:-20}"
JOBS="${PROGEN_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
FIRST="${1:?usage: progen-diff.sh <first-seed> <count>}"
COUNT="${2:?usage: progen-diff.sh <first-seed> <count>}"

. "$ROOT/tools/lib/timeout.sh"

# Prints the issue of the first skip entry of class $1 whose pattern
# matches the evidence file $2.
known_issue() {
  local class issue pattern
  while IFS=: read -r class issue pattern; do
    case "$class" in "$1"|'*') ;; *) continue ;; esac
    if grep -Eq -- "$pattern" "$2"; then
      echo "$issue"
      return 0
    fi
  done < "$SKIPS"
  return 1
}

# The class of a seed whose two binaries both built and ran.
compare_runs() {
  local c_rc="$1" n_rc="$2" dir="$3"
  case "$c_rc/$n_rc" in
    124/*|*/124|137/*|*/137) echo timeout; return ;;
  esac
  if [ "$c_rc" -ge 128 ] || [ "$n_rc" -ge 128 ]; then echo crash; return; fi
  if [ "$c_rc" != "$n_rc" ]; then echo exit-differs; return; fi
  if ! cmp -s "$dir/c.out" "$dir/native.out"; then echo output-differs; return; fi
  if [ "$c_rc" != 0 ] || ! tail -1 "$dir/c.out" | grep -q '^checksum='; then echo bad-program; return; fi
  echo agree
}

classify() {
  local seed="$1" dir="$2" c_ok=1 n_ok=1 c_rc=0 n_rc=0
  "$OUT/progen" "$seed" "$DEPTH" > "$dir/prog.kai" 2> "$dir/progen.log" || { echo bad-program; return; }
  "$KAI" build --backend=c "$dir/prog.kai" -o "$dir/c.bin" > "$dir/c.build.log" 2>&1 || c_ok=0
  # Whole-program native: one-shot programs gain nothing from the partitioned path.
  KAI_NATIVE_MODULAR=0 "$KAI" build --backend=native "$dir/prog.kai" -o "$dir/native.bin" > "$dir/native.build.log" 2>&1 || n_ok=0
  case "$c_ok$n_ok" in
    00) echo both-reject; return ;;
    01) echo c-rejects; return ;;
    10) echo native-rejects; return ;;
  esac
  kai_timeout "$RUN_TIMEOUT" "$dir/c.bin" > "$dir/c.out" 2>&1 < /dev/null || c_rc=$?
  kai_timeout "$RUN_TIMEOUT" "$dir/native.bin" > "$dir/native.out" 2>&1 < /dev/null || n_rc=$?
  echo "c=$c_rc native=$n_rc" > "$dir/exit-codes"
  compare_runs "$c_rc" "$n_rc" "$dir"
}

# One seed: classify it, keep the evidence of a finding, and append one
# line `<class> <seed> <issue|->` to the results (atomic: well under PIPE_BUF).
one_seed() {
  local seed="$1" dir="$OUT/work/$1" class issue=-
  mkdir -p "$dir"
  class="$(classify "$seed" "$dir")"
  rm -f "$dir/c.bin" "$dir/native.bin"
  if [ "$class" = agree ]; then
    rm -rf "$dir"
  else
    cat "$dir"/* > "$dir.evidence" 2>/dev/null || true
    issue="$(known_issue "$class" "$dir.evidence")" || issue=-
    rm -f "$dir.evidence"
    echo "$class" > "$dir/class"
    mv "$dir" "$OUT/findings/$seed"
  fi
  printf '%s %s %s\n' "$class" "$seed" "$issue" >> "$OUT/results"
}

report() {
  local total agree known new
  total="$(wc -l < "$OUT/results" | tr -d ' ')"
  agree="$(grep -c '^agree ' "$OUT/results" || true)"
  known="$(grep -v '^agree ' "$OUT/results" | grep -vc ' -$' || true)"
  new="$(grep -v '^agree ' "$OUT/results" | grep -c ' -$' || true)"
  echo "progen-diff: seeds=$total agree=$agree known=$known new=$new (first=$FIRST depth=$DEPTH)"
  grep -v '^agree ' "$OUT/results" | awk '{ print $1, ($3 == "-" ? "new" : "known #" $3) }' | sort | uniq -c | sed 's/^/  /'
  grep -v '^agree ' "$OUT/results" | grep ' -$' | sort -k1,1 -k2,2n | while read -r class seed _; do
    echo "NEW $class seed $seed — ${OUT#"$ROOT"/}/findings/$seed/prog.kai — repro: PROGEN_DEPTH=$DEPTH tools/progen-diff.sh $seed 1"
  done
  [ "$new" -eq 0 ]
}

main() {
  local start seed last batch stop
  rm -rf "$OUT/work" "$OUT/findings" "$OUT/results"
  mkdir -p "$OUT/work" "$OUT/findings"
  : > "$OUT/results"
  "$KAI" build --backend=c "$ROOT/tools/progen/main.kai" -o "$OUT/progen" > "$OUT/progen.build.log" 2>&1 \
    || { echo "progen-diff FAIL — the generator does not build:"; cat "$OUT/progen.build.log"; exit 2; }
  "$ROOT/tools/kaic2-native-capable.sh" "$ROOT/stage2/kaic2" \
    || { echo "progen-diff FAIL — kaic2 has no native backend (build with KAI_LLVM=1)"; exit 2; }
  export ROOT KAI SKIPS OUT DEPTH RUN_TIMEOUT KAI_TIMEOUT_KIND _KAI_TIMEOUT_PERL
  export -f known_issue compare_runs classify one_seed kai_timeout
  start="$(date +%s)"
  seed="$FIRST"
  last=$((FIRST + COUNT))
  batch=$((JOBS * 4))
  while [ "$seed" -lt "$last" ]; do
    if [ -n "${PROGEN_BUDGET:-}" ] && [ $(($(date +%s) - start)) -ge "$PROGEN_BUDGET" ]; then
      echo "progen-diff: time budget of ${PROGEN_BUDGET}s spent before seed $seed"
      break
    fi
    stop=$((seed + batch > last ? last : seed + batch))
    # Not seq(1): BSD seq prints a date-sized seed in scientific notation.
    while [ "$seed" -lt "$stop" ]; do
      echo "$seed"
      seed=$((seed + 1))
    done | xargs -P "$JOBS" -I{} bash -c 'one_seed "$@"' _ {}
    seed="$stop"
  done
  report
}

main
