#!/usr/bin/env bash
# Contract gate for tools/lib/timeout.sh. A deadline must report 124 and a
# child that survives SIGTERM must report 137: the M:N gates classify a hang
# by that exit code, and a shim that reports 137 for every deadline files
# every hang as an output mismatch instead.

set -uo pipefail
cd "$(dirname "$0")/.."
. tools/lib/timeout.sh

echo "== test-timeout-shim: kind=$KAI_TIMEOUT_KIND =="
if [ "$KAI_TIMEOUT_KIND" = none ]; then
  echo "SKIP — no timeout(1), gtimeout or perl on this host"
  exit 0
fi

fail=0
check() {
  want="$1"; label="$2"; shift 2
  ec=0
  "$@" >/dev/null 2>&1 || ec=$?
  if [ "$ec" = "$want" ]; then
    echo "OK   $label (exit $ec)"
  else
    echo "FAIL $label: want $want, got $ec"
    fail=1
  fi
}

check 0   "success passes through"   kai_timeout 5 true
check 3   "exit code passes through" kai_timeout 5 sh -c 'exit 3'
check 124 "deadline reports 124"     kai_timeout 1 sleep 10

# Only the coreutils paths have an escalation phase; perl kills outright and
# reports the deadline either way.
want=137
[ "$KAI_TIMEOUT_KIND" = perl ] && want=124
check "$want" "SIGTERM-ignoring child reports $want" \
  kai_timeout 1 sh -c 'trap "" TERM; sleep 10'

[ "$fail" = 0 ] && echo "test-timeout-shim: OK" || { echo "test-timeout-shim: FAIL"; exit 1; }
