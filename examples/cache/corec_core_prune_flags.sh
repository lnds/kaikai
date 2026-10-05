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
  # A second build under the same cache finds the core object before typing.
  WARM="$WORK/warm"; mkdir -p "$WARM"
  cp "$WORK/main.kai" "$WARM/main.kai"
  for run in cold warm; do
    (cd "$WARM" && "$KAIC2" $EDITION_FLAG --core-cache-stats --emit=native --core-cache-dir "$WARM" \
       --toolchain-id "$TID" main.kai > out 2> "$run.err") || { cat "$WARM/$run.err"; fail "native $run build failed"; }
  done
  grep -q "core-prune: off" "$WARM/cold.err" || fail "cold native build with a core cache pruned"
  grep -q "core-prune: dropped" "$WARM/warm.err" || { cat "$WARM/warm.err"; fail "warm native build did not prune on a core-object hit"; }
  expect off     --emit=native-modular
  # The reusable-tags table reads only the functions a program runs, so a
  # build's RC ledger is the same whether it lowered the whole core or not.
  FIX="$ROOT/examples/perceus/next_tier_rc_fix.kai"
  for run in cold warm; do
    KAI_BIN_MEMO=0 KAI_CORE_CACHE_DIR="$WORK/ledger" "$ROOT/bin/kai" build --backend=native "$FIX" \
      -o "$WORK/ledger-$run" 2> "$WORK/ledger-$run.err" || { cat "$WORK/ledger-$run.err"; fail "native $run build of the ledger fixture failed"; }
  done
  KAI_BIN_MEMO=0 KAI_CORE_CACHE=0 "$ROOT/bin/kai" build --backend=native "$FIX" -o "$WORK/ledger-uncached" 2> /dev/null \
    || fail "uncached native build of the ledger fixture failed"
  for run in cold warm uncached; do
    KAI_THREADS=1 KAI_TRACE_RC=1 "$WORK/ledger-$run" 2>&1 > /dev/null | grep '^\[KAI_TRACE_RC\]' > "$WORK/ledger-$run.rc" || true
  done
  [ -s "$WORK/ledger-cold.rc" ] || fail "the ledger fixture printed no RC ledger"
  for run in warm uncached; do
    cmp -s "$WORK/ledger-cold.rc" "$WORK/ledger-$run.rc" || { diff "$WORK/ledger-cold.rc" "$WORK/ledger-$run.rc"; fail "RC ledger differs between a cold and a $run native build"; }
  done
else
  echo "corec_core_prune_flags: native backend unavailable, native rows skipped"
fi
echo "corec_core_prune_flags OK"
