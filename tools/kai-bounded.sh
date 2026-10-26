#!/bin/sh
# kai-bounded.sh <seconds> <cmd> [args...]: bounded run that names the deadline on stderr.
. "$(dirname "$0")/lib/timeout.sh"
secs=$1; shift
rc=0
kai_timeout "$secs" "$@" || rc=$?
[ "$rc" -ne 124 ] || echo "TIMEOUT after ${secs}s: $*" >&2
exit "$rc"
