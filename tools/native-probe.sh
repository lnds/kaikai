#!/bin/sh
# Succeeds when the given kaic2 emits the native objects of a one-line program
# the way kai asks for them: through the emit pool, with a core object beside
# the program's. Every object it lists must exist once it has exited.
# Usage: tools/native-probe.sh <kaic2>
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAIC2="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
DIR="$(mktemp -d)"
trap 'rm -rf "$DIR"' EXIT

printf 'fn main() : Unit / Console = println("probe")\n' > "$DIR/probe.kai"
rc=0
mkdir "$DIR/core"
(cd "$DIR" && KAI_NATIVE_JOBS=2 "$KAIC2" --edition "$(cat "$ROOT/EDITION")" --emit=native --path "$ROOT/stdlib" \
  --core-cache-dir "$DIR/core" --toolchain-id probe probe.kai) \
  > "$DIR/probe.objs" 2> "$DIR/probe.err" || rc=$?
missing="$(cd "$DIR" && while IFS= read -r obj; do [ -f "$obj" ] || printf ' %s' "$obj"; done < probe.objs)"
[ "$rc" -eq 0 ] && [ -f "$DIR/probe.o" ] && [ -z "$missing" ] && exit 0

echo "native-probe: $KAIC2 cannot emit a native object (exit $rc${missing:+, never written:$missing})" >&2
head -10 "$DIR/probe.err" | sed 's/^/    /' >&2
exit 1
