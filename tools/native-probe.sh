#!/bin/sh
# Succeeds when the given kaic2 emits a native object for a one-line program.
# Usage: tools/native-probe.sh <kaic2>
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAIC2="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
DIR="$(mktemp -d)"
trap 'rm -rf "$DIR"' EXIT

printf 'fn main() : Unit / Console = println("probe")\n' > "$DIR/probe.kai"
rc=0
(cd "$DIR" && "$KAIC2" --edition "$(cat "$ROOT/EDITION")" --emit=native --path "$ROOT/stdlib" probe.kai) \
  > /dev/null 2> "$DIR/probe.err" || rc=$?
[ "$rc" -eq 0 ] && [ -f "$DIR/probe.o" ] && exit 0

echo "native-probe: $KAIC2 cannot emit a native object (exit $rc)" >&2
head -10 "$DIR/probe.err" | sed 's/^/    /' >&2
exit 1
