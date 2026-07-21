#!/usr/bin/env bash
# Bounded CI step. Runs one gate under a deadline set well below the job's
# `timeout-minutes`, so a wedge fails as THIS step, named, with the elapsed
# time in the message.
#
#   tools/ci-bounded-step.sh <seconds> <label> <cmd> [args...]
#
# Without it a wedged gate burns the job ceiling and the run reports
# `cancelled` — the same conclusion a routine force-push supersede produces,
# which makes a half-hour deadlock indistinguishable from a 90-second
# supersede in the checks list.
#
# Exit status: the command's own, except a fired deadline (124) or a killed
# child (137), both reported as an ::error:: annotation and re-raised as-is.
# Only the direct child is signalled; a fixture the gate spawned may outlive
# it as an orphan, so gates whose fixtures can wedge also bound each run
# themselves (see tools/run-mn-tsan.sh).

set -uo pipefail

if [ "$#" -lt 3 ]; then
  echo "usage: ci-bounded-step.sh <seconds> <label> <cmd> [args...]" >&2
  exit 2
fi

secs="$1"; label="$2"; shift 2

. "$(dirname "$0")/lib/timeout.sh"

if [ "$KAI_TIMEOUT_KIND" = none ]; then
  echo "::warning::$label runs UNBOUNDED (no timeout/gtimeout/perl on this host)"
fi

start="$SECONDS"
ec=0
kai_timeout "$secs" "$@" || ec=$?
elapsed=$((SECONDS - start))

case "$ec" in
  124) echo "::error::$label HUNG — no completion within ${secs}s; killed by the step deadline (this is a hang, not a cancelled run)" ;;
  137) echo "::error::$label KILLED after ${elapsed}s — SIGKILL: either the step deadline escalating past its grace, or an external kill (OOM)" ;;
esac

exit "$ec"
