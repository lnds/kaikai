#!/usr/bin/env bash
# Corrective scope passes may only disappear.
#
# The name-resolution design replaces after-the-fact scope filtering with
# tables keyed at construction; each corrective pass retired is that
# design executed. This ratchet turns the direction into a gate: the
# count may never rise above the baseline, and a lane that retires one
# lowers the baseline so it cannot come back.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BASELINE_FILE="$ROOT/tools/corrective-pass-baseline.txt"

count=0
alive=""
for p in ta_scope unit_scope proto_scope const_scope; do
  if [ -f "$ROOT/stage2/compiler/$p.kai" ]; then
    count=$((count + 1)); alive="$alive $p"
  fi
done

baseline=$(cat "$BASELINE_FILE")

if [ "$count" -gt "$baseline" ]; then
  echo "corrective-pass-ratchet FAIL — $count passes alive (baseline $baseline):$alive"
  echo "  a new corrective pass is the design §9 explicitly rejects; key the table"
  echo "  at construction instead"
  exit 1
fi
if [ "$count" -lt "$baseline" ]; then
  echo "corrective-pass-ratchet OK (improved) — $count < baseline $baseline"
  echo "  suggest: echo $count > tools/corrective-pass-baseline.txt"
else
  echo "corrective-pass-ratchet OK — $count passes at baseline:$alive"
fi
