#!/bin/sh
# Full-core gate. A build types and emits only the core functions the
# program reaches, so a broken function nothing in the corpus calls would
# otherwise go unnoticed. This compiles a trivial program against the
# whole core, through the typer, the C emitter and cc.

set -eu
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAIC2="$ROOT/stage2/kaic2"
WORK="$ROOT/stage2/build/core-full"

[ -x "$KAIC2" ] || { echo "core-full: SKIP — no stage2/kaic2"; exit 0; }

rm -rf "$WORK"; mkdir -p "$WORK"
printf 'fn main() : Int = 0\n' > "$WORK/main.kai"

(cd "$WORK" && "$KAIC2" --full-core main.kai > main.c) || {
  echo "core-full: FAIL — the full core does not type or emit"; exit 1; }
cc -std=c99 -w -c -I "$ROOT/stage2" -I "$ROOT/stage0" "$WORK/main.c" -o "$WORK/main.o" || {
  echo "core-full: FAIL — the full core's C does not compile"; exit 1; }

# A typecheck-only run cuts the core like a build does, and --full-core
# still types all of it.
(cd "$WORK" && "$KAIC2" --core-cache-stats --check main.kai 2>&1) | grep -q 'core-prune: dropped' || {
  echo "core-full: FAIL — a typecheck-only run types the whole core"; exit 1; }
(cd "$WORK" && "$KAIC2" --full-core --core-cache-stats --check main.kai 2>&1) | grep -q 'core-prune: off' || {
  echo "core-full: FAIL — --full-core does not reach a typecheck-only run"; exit 1; }
echo "core-full OK"
