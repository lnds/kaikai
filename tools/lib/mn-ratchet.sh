#!/usr/bin/env bash
# The M:N corpus gate's anti-regression ratchet.
#
# The corpus reached the gate with a finite queue of pre-existing defects,
# each with an open issue. The queue is allowed to shrink and nothing else: a
# finding whose (fixture, backend) is not in the baseline fails the gate.
#
# The BUCKET is deliberately not part of the key. A baselined pair whose
# crash becomes a hang is the same known-broken pair, and flapping a required
# check on whichever bucket a race landed in would cost more than it buys —
# the issue named on the baseline line carries that detail.
#
# Expects from the caller: $TMP, $BASELINE, $BACKENDS, $SHARDED, $REPEATS,
# $THREAD_COUNTS, and $TMP/fixtures listing the fixtures this run walked.

# Baseline keys ($1) restricted to what THIS run could have observed.
#
# A shard walks a subset, so an entry it did not reach is out of scope — not
# closed, and not stale. Comparing a shard's findings against the FULL
# baseline would report every entry outside its subset as a fix, the trap
# tools/test-backend-parity.sh documents. Entries whose backend arm was
# dropped are out of scope for the same reason.
mn_ratchet_scope() {
  local fixture backend
  : > "$TMP/base.inscope"
  : > "$TMP/base.stale"
  while read -r fixture backend; do
    if ! grep -qxF "$fixture" "$TMP/fixtures"; then
      # Only a full-corpus run can tell a departed fixture from one that
      # simply belongs to another shard.
      if [ "$SHARDED" = 0 ]; then
        printf '%s\n' "$fixture" >> "$TMP/base.stale"
      fi
    elif printf '%s\n' $BACKENDS | grep -qxF "$backend"; then
      printf '%s %s\n' "$fixture" "$backend" >> "$TMP/base.inscope"
    fi
  done < "$1"
  sort -u -o "$TMP/base.inscope" "$TMP/base.inscope"
}

# Compare this run's findings ($1) against the baseline. Exit status is the
# gate's verdict: 0 at or below baseline, 1 on a new finding or a rotten
# baseline.
mn_ratchet_verdict() {
  local findings="$1" new closed
  if [ ! -f "$BASELINE" ]; then
    echo "run-mn-corpus-determinism FAIL — baseline missing: $BASELINE"
    return 1
  fi
  # `|| true`: an all-comment baseline is the state this gate is working
  # towards, and grep exiting 1 on no match would kill the run under
  # `set -o pipefail` — silently, right before the verdict.
  { grep -vE '^[[:space:]]*(#|$)' "$BASELINE" || true; } \
    | awk '{print $1" "$2}' | sort -u > "$TMP/base.keys"
  awk '{print $2" "$3}' "$findings" | tr -d '[]' | sort -u > "$TMP/now.keys"

  mn_ratchet_scope "$TMP/base.keys"
  if [ -s "$TMP/base.stale" ]; then
    echo ""
    echo "run-mn-corpus-determinism FAIL — the baseline names fixtures that left the corpus:"
    sort -u "$TMP/base.stale" | sed 's/^/    /'
    echo "  Remove the stale lines from $BASELINE — a baseline is not allowed to rot."
    return 1
  fi

  new="$(comm -23 "$TMP/now.keys" "$TMP/base.inscope")"
  if [ -n "$new" ]; then
    mn_ratchet_report_new "$new"
    return 1
  fi

  closed="$(comm -13 "$TMP/now.keys" "$TMP/base.inscope")"
  [ -z "$closed" ] || mn_ratchet_report_closed "$closed"
  return 0
}

mn_ratchet_report_new() {
  echo ""
  echo "run-mn-corpus-determinism FAIL — NEW findings, not in the baseline:"
  printf '%s\n' "$1" | sed 's/^/    + /'
  echo ""
  echo "  This program behaves differently above one thread than it does at"
  echo "  KAI_THREADS=1. Fix the runtime — do NOT add it to $BASELINE to"
  echo "  silence it. If its output LINE ORDER is genuinely unspecified,"
  echo "  declare it in tools/mn-corpus-order-dependent.txt with a reason;"
  echo "  the line multiset stays compared, so that forgives interleaving and"
  echo "  nothing else."
}

mn_ratchet_report_closed() {
  echo ""
  echo "run-mn-corpus-determinism OK (improved) — these baselined pairs did"
  echo "NOT reproduce their finding in THIS run:"
  printf '%s\n' "$1" | sed 's/^/    - /'
  echo "  Candidates for tightening the ratchet — but confirm first. These"
  echo "  defects are timing- and platform-dependent: a pair can pass on one"
  echo "  runner and fail on another, and this run saw $REPEATS repeats at"
  echo "  N in {$THREAD_COUNTS} on one platform. Remove a line from"
  echo "  $BASELINE when the fix is understood, not because one run was quiet."
}
