#!/bin/sh
# Core pruning versus every cache the compiler can write. A build drops
# the core functions its program cannot reach only when it writes no core
# artifact keyed by the core sources and toolchain alone; under every cache
# flag and backend it must either prune or keep the whole core, and never
# trip the internal check that a pruned compile reached such a write. The
# typed cut keys each blob by the decls typed, so it prunes.

set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT INT TERM
printf 'fn main() : Int = 0\n' > "$WORK/main.kai"
TID="fixture-$(cksum < "$KAIC2" | cut -d' ' -f1)"

fail() { echo "corec_core_prune_flags: FAIL — $1"; exit 1; }

# $1: expected (dropped|off), rest: flags
expect() {
  want="$1"; shift
  dir="$WORK/run$#$(echo "$*" | cksum | cut -d' ' -f1)"
  mkdir -p "$dir/cc" "$dir/uc"
  cp "$WORK/main.kai" "$dir/main.kai"
  set -- $(echo "$*" | sed "s#@CC#$dir/cc#g; s#@UC#$dir/uc#g; s#@TID#$TID#g")
  rc=0
  (cd "$dir" && "$KAIC2" $EDITION_FLAG --core-cache-stats "$@" main.kai > out 2> err) || rc=$?
  if grep -q "internal error" "$dir/err"; then cat "$dir/err"; fail "internal error under: $*"; fi
  [ "$rc" -eq 0 ] || { cat "$dir/err"; fail "exit $rc under: $*"; }
  grep -q "core-prune: $want" "$dir/err" || { cat "$dir/err"; fail "expected core-prune: $want under: $*"; }
}

expect dropped
expect dropped --core-cache-dir @CC --toolchain-id @TID
expect dropped --user-cache --user-cache-dir @UC
expect off     --check --core-cache-dir @CC --toolchain-id @TID
expect off     --full-core
expect off     --emit=c-modular --core-cache-dir @CC --toolchain-id @TID

if "$KAIC2" --emit=native "$WORK/main.kai" > /dev/null 2> "$WORK/probe.err"; then
  expect dropped --emit=native
  expect off     --emit=native --core-cache-dir @CC --toolchain-id @TID
  expect off     --emit=native-modular
else
  echo "corec_core_prune_flags: native backend unavailable, native rows skipped"
fi
echo "corec_core_prune_flags OK"
