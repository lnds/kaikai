#!/usr/bin/env bash
# How ONE run of ONE binary is observed and judged. The vocabulary the M:N
# corpus gate is written in; tools/lib/mn-measure.sh drives it.
#
# Expects from the caller: $TMP (scratch dir), $RUN_TIMEOUT, $RECHECKS,
# $REF_RECHECKS, $ORDER_FILE, and tools/lib/timeout.sh + tools/lib/corpus.sh
# already sourced.

# Path-safe name for a fixture, used to key its built binaries.
mn_slug_of() { echo "$1" | tr '/.' '__'; }

# One line of witness, newlines folded so a multi-line stdout stays greppable
# in a CI log.
mn_witness_of() { head -c 160 "$1" | tr '\n' '|'; }

# A fixture that has declared its line order unspecified
# (tools/mn-corpus-order-dependent.txt).
mn_is_order_dependent() {
  [ -f "$ORDER_FILE" ] && grep -q "^$1:" "$ORDER_FILE" 2>/dev/null
}

# Same lines, any order — the multiset comparison that replaces the sequence
# comparison for declared fixtures.
mn_same_lines() {
  sort "$1" > "$TMP/cmp.a"
  sort "$2" > "$TMP/cmp.b"
  cmp -s "$TMP/cmp.a" "$TMP/cmp.b"
}

# Run $1 at KAI_THREADS=$2, stdout to the FILE $3, stderr to $3.err. Echoes
# the exit status.
#
# stdout must be a FILE and never a pipe: a pipe (like a tty) changes the
# buffering enough to close the window on a whole family of scheduler races.
#
# stdin is /dev/null, and not optionally: the corpus contains fixtures that
# read stdin, and the measurement loop is a `while read` over the fixture
# list. Without this a single stdin-reading fixture swallows the rest of the
# corpus and the gate reports a confident pass over the handful it got to.
mn_run_at() {
  local bin="$1" n="$2" out="$3" ec=0
  kai_timeout "$RUN_TIMEOUT" env KAI_THREADS="$n" "$bin" >"$out.raw" 2>"$out.err" </dev/null || ec=$?
  kai_corpus_normalize_timestamps <"$out.raw" >"$out"
  echo "$ec"
}

# The single place a run becomes a bucket, so the first observation and every
# recheck judge by identical rules.
#
#   ok       stdout and exit code reproduce the N=1 reference
#   hang     the bounded run hit its deadline (124/137)
#   crash    exit code differs from the reference (SIGSEGV, trap, abort)
#   empty    exit code matched, stdout empty, reference non-empty
#   reorder  same lines, different order, on a declared fixture
#   diverge  everything else
mn_classify_run() {
  local out="$1" ec="$2" ref="$3" ref_ec="$4" fixture="$5"
  if [ "$ec" = 124 ] || [ "$ec" = 137 ]; then
    echo hang
  elif [ "$ec" != "$ref_ec" ]; then
    echo crash
  elif cmp -s "$out" "$ref"; then
    echo ok
  elif [ ! -s "$out" ] && [ -s "$ref" ]; then
    echo empty
  elif mn_is_order_dependent "$fixture" && mn_same_lines "$out" "$ref"; then
    echo reorder
  else
    echo diverge
  fi
}

# True when the N=1 reference reproduces itself on $REF_RECHECKS consecutive
# re-runs. A fixture that fails this varies on its own and cannot witness
# anything about thread count.
mn_ref_is_stable() {
  local bin="$1" ref="$2" ref_ec="$3" i ec
  for ((i = 0; i < REF_RECHECKS; i++)); do
    ec="$(mn_run_at "$bin" 1 "$TMP/recheck")"
    [ "$ec" = "$ref_ec" ] || return 1
    cmp -s "$TMP/recheck" "$ref" || return 1
  done
  return 0
}

# True when a divergence reproduces on ANY of $RECHECKS further runs at the
# same thread count.
#
# The bar is asymmetric on purpose, matching tools/test-backend-parity.sh: a
# single converging re-run is not evidence of a flake, so a divergence is
# withdrawn only after failing to reappear on EVERY recheck. What this is
# for: stdout writes are not line-atomic across scheduler threads, so any
# multi-fiber program can occasionally emit a torn line — that tears a
# different fixture on every run, and a byte-exact gate believing each one
# would be red at random. A deterministic divergence reproduces well within
# two extra runs; a genuine tear does not, and is still reported loudly.
#
# crash and hang are deliberately NOT rechecked. A one-in-many SIGSEGV is a
# real defect, and re-running until it behaves is how a gate learns to lie.
mn_diverges_again() {
  local bin="$1" n="$2" ref="$3" ref_ec="$4" fixture="$5" i ec
  for ((i = 0; i < RECHECKS; i++)); do
    ec="$(mn_run_at "$bin" "$n" "$TMP/recheck")"
    case "$(mn_classify_run "$TMP/recheck" "$ec" "$ref" "$ref_ec" "$fixture")" in
      ok|reorder) : ;;
      *) return 0 ;;
    esac
  done
  return 1
}
