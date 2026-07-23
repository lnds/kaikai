#!/usr/bin/env bash
# Measuring one (fixture, backend) pair: take the N=1 run as the reference,
# then judge every N>1 run against it. Drives tools/lib/mn-observe.sh.
#
# Expects from the caller: $TMP, $THREAD_COUNTS, $REPEATS, $RUN_TIMEOUT, and
# the report files $findings $reffail $nondet $reorders $flaky. Sets
# $MN_JUDGED to 1 when the pair produced a usable reference, 0 otherwise, so
# the caller can check its own arithmetic.
#
# Callers must invoke this SERIALLY, one process at a time. Running two
# measurements at once changes the very scheduling being measured.

# Which bucket names a mixed set of repeats, worst first. A pair that hung
# once and diverged once is reported as a hang: the wedge is the finding
# worth chasing, and the counts stay in the line either way.
mn_headline_bucket() {
  if   [ "$1" -gt 0 ]; then echo hang
  elif [ "$2" -gt 0 ]; then echo crash
  elif [ "$3" -gt 0 ]; then echo empty
  else                      echo diverge
  fi
}

# One thread count for one pair: $REPEATS runs, then a verdict.
mn_measure_at() {
  local bin="$1" fixture="$2" backend="$3" ref="$4" ref_ec="$5" n="$6"
  local ok=0 reorder=0 diverge=0 empty=0 crash=0 hang=0 witness="" r ec bucket

  for ((r = 0; r < REPEATS; r++)); do
    ec="$(mn_run_at "$bin" "$n" "$TMP/run")"
    case "$(mn_classify_run "$TMP/run" "$ec" "$ref" "$ref_ec" "$fixture")" in
      ok)      ok=$((ok + 1)) ;;
      reorder) reorder=$((reorder + 1)) ;;
      hang)    hang=$((hang + 1)) ;;
      empty)   empty=$((empty + 1)) ;;
      crash)   crash=$((crash + 1))
               [ -n "$witness" ] || witness="exit $ec (N=1 exits $ref_ec): $(mn_witness_of "$TMP/run.err")" ;;
      *)       diverge=$((diverge + 1))
               [ -n "$witness" ] || witness="got '$(mn_witness_of "$TMP/run")' want '$(mn_witness_of "$ref")'" ;;
    esac
  done

  if [ "$reorder" != 0 ]; then
    printf '%s [%s] N=%s — %s/%s runs reordered the same lines (declared order-dependent)\n' \
      "$fixture" "$backend" "$n" "$reorder" "$REPEATS" >> "$reorders"
  fi
  if [ "$((ok + reorder))" = "$REPEATS" ]; then
    return 0
  fi

  bucket="$(mn_headline_bucket "$hang" "$crash" "$empty")"

  if [ "$bucket" = diverge ] || [ "$bucket" = empty ]; then
    mn_adjudicate "$bin" "$fixture" "$backend" "$ref" "$ref_ec" "$n" "$bucket" || return 0
  fi

  printf '%-7s %s [%s] N=%s: ok=%s reorder=%s diverge=%s empty=%s crash=%s hang=%s of %s%s\n' \
    "$bucket" "$fixture" "$backend" "$n" "$ok" "$reorder" "$diverge" "$empty" "$crash" "$hang" "$REPEATS" \
    "${witness:+ — $witness}" >> "$findings"
  [ -z "${GITHUB_ACTIONS:-}" ] || \
    echo "::error title=mn-corpus $bucket::$fixture [$backend] at KAI_THREADS=$n does not reproduce its N=1 behaviour"
}

# The two checks a `diverge` / `empty` clears before it is believed. Returns
# non-zero when the verdict is withdrawn (and records why).
mn_adjudicate() {
  local bin="$1" fixture="$2" backend="$3" ref="$4" ref_ec="$5" n="$6" bucket="$7"

  if ! mn_ref_is_stable "$bin" "$ref" "$ref_ec"; then
    printf '%s [%s] — N=1 output varies between runs; %s at N=%s not judged\n' \
      "$fixture" "$backend" "$bucket" "$n" >> "$nondet"
    return 1
  fi
  if ! mn_diverges_again "$bin" "$n" "$ref" "$ref_ec" "$fixture"; then
    printf '%s [%s] N=%s — %s did not reappear on %s consecutive rechecks\n' \
      "$fixture" "$backend" "$n" "$bucket" "$RECHECKS" >> "$flaky"
    [ -z "${GITHUB_ACTIONS:-}" ] || \
      echo "::warning title=mn-corpus flaky::$fixture [$backend] diverged from its N=1 reference at KAI_THREADS=$n but converged on $RECHECKS rechecks — held as a flake, not counted. A divergence this harness deems flaky is still a divergence."
    return 1
  fi
  return 0
}

# One (fixture, backend) pair across every requested thread count. A
# fixture with a declared thread pin (kai_corpus_pinned_threads) is
# measured at the pin on every arm — still fully gated for crash / hang /
# self-reproduction, but never asked to reproduce above its pin.
mn_measure_pair() {
  local bin="$1" fixture="$2" backend="$3" ref ref_ec n pin
  MN_JUDGED=0

  pin="$(kai_corpus_pinned_threads "$fixture")" || pin=""
  [ -z "$pin" ] || printf '%s [%s] — pinned to KAI_THREADS=%s by its env file\n' \
    "$fixture" "$backend" "$pin" >> "$pinned"

  ref="$TMP/ref"
  ref_ec="$(mn_run_at "$bin" "${pin:-1}" "$ref")"
  if [ "$ref_ec" = 124 ] || [ "$ref_ec" = 137 ]; then
    printf '%s [%s] — N=1 reference did not complete within %ss\n' \
      "$fixture" "$backend" "$RUN_TIMEOUT" >> "$reffail"
    return 0
  fi
  MN_JUDGED=1

  for n in $THREAD_COUNTS; do
    mn_measure_at "$bin" "$fixture" "$backend" "$ref" "$ref_ec" "${pin:-$n}"
  done
}
