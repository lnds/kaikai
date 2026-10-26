#!/usr/bin/env bash
# Differential gate for core pruning: each program compiles and runs once
# with the core pruned as usual and once against the whole core
# (KAI_CORE_PRUNE=off); exit code, stdout and stderr must match. A pruning
# error in the pruned build fails the case even when the whole-core build
# fails too. Both variants share one HOME, so every cache on the path must
# key the knob; the pruned variant runs twice so the second compile sees a
# warm core cache and prunes.
#
#   prune-differential.sh [--backend c|native] [--jobs N] [--shard I/N] corpus|combos|combos-subset
#   prune-differential.sh --one <backend> <mode> <file>     (mode: run|test|cover)
#
# corpus        every examples/**/*.kai entry point (tools/lib/corpus.sh) with an .out.expected, run (and test
#               where the file carries test blocks)
# combos        every pair of demand routes, generated into a temp dir
# combos-subset a fixed small subset of combos
#
# Environment: PRUNE_DIFF_SKIPS (default tools/prune-differential-skips.txt),
# PRUNE_DIFF_KEEP=1 keeps the work dir.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
SELF="$ROOT/tools/prune-differential.sh"
SKIPS="${PRUNE_DIFF_SKIPS:-$ROOT/tools/prune-differential-skips.txt}"
. "$ROOT/tools/lib/corpus.sh"

norm() { sed -e "s#$ROOT#<root>#g" -e "s#${WORK:-/nonexistent}#<work>#g" -e 's#/private/var#/var#g' -e 's#/tmp\.[A-Za-z0-9]*#/tmp.X#g' \
  -e 's#[0-9]\{4\}-[0-9][0-9]-[0-9][0-9]T[0-9:]*Z#<time>#g' -e 's#elapsed: [0-9.]*s#elapsed: <t>#g' \
  -e 's#"duration_ms":[0-9]*#"duration_ms":0#g' -e '/__root__\.c:[0-9]*:[0-9]*: warning/,/^[0-9]* warnings\{0,1\} generated/d'; }

# run_variant <tag> <backend> <mode> <file>: leaves $OUT/<tag>.{out,err,rc}.
run_variant() {
  tag=$1; be=$2; mode=$3; file=$4
  dir=$(dirname "$file"); base=$(basename "$file")
  case $mode in
    run)   set -- run --backend="$be" "$base" ;;
    test)  set -- test --backend="$be" "$base" ;;
    cover) set -- test --backend="$be" --json --coverage "$base" ;;
  esac
  local env=() shims threads
  shims=$(kai_corpus_cflags "${file#"$ROOT"/}") && env+=("CFLAGS=$shims")
  threads=$(kai_corpus_meta "${file#"$ROOT"/}" KAI_THREADS) && env+=("KAI_THREADS=$threads")
  [ "$tag" = whole ] && env+=("KAI_CORE_PRUNE=off")
  ( cd "$dir" && env "${env[@]}" timeout 300 "$KAI" "$@" </dev/null ) > "$OUT/$tag.out" 2> "$OUT/$tag.err"
  echo $? > "$OUT/$tag.rc"
}

# compare <backend> <mode> <file>: runs both variants and sets $why to what differs.
compare() {
  run_variant pruned "$1" "$2" "$3"
  run_variant whole "$1" "$2" "$3"
  why=""
  grep -q 'pruning dropped\|internal error' "$OUT/pruned.err" && why="$why internal-pruning-error"
  [ "$(cat "$OUT/pruned.rc")" = "$(cat "$OUT/whole.rc")" ] || why="$why exit($(cat "$OUT/pruned.rc")/$(cat "$OUT/whole.rc"))"
  norm < "$OUT/pruned.out" > "$OUT/p.out"; norm < "$OUT/whole.out" > "$OUT/w.out"
  norm < "$OUT/pruned.err" | grep -v '^kai: ' > "$OUT/p.err"; norm < "$OUT/whole.err" | grep -v '^kai: ' > "$OUT/w.err"
  cmp -s "$OUT/p.out" "$OUT/w.out" || why="$why stdout"
  [ -z "${PRUNE_DIFF_EXPECT_OK:-}" ] || [ "$(cat "$OUT/whole.rc")" = 0 ] || why="$why whole-core-failed"
  cmp -s "$OUT/p.err" "$OUT/w.err" || why="$why stderr"
}

one() {
  be=$1; mode=$2; file=$3
  WORK=$(mktemp -d "${TMPDIR:-/tmp}/prunediff.XXXXXX"); OUT=$WORK
  trap '[ -n "${PRUNE_DIFF_KEEP:-}" ] || rm -rf "$WORK"' EXIT
  run_variant warm "$be" "$mode" "$file"
  compare "$be" "$mode" "$file"
  [ -z "$why" ] || compare "$be" "$mode" "$file"
  if [ -n "$why" ]; then
    kind=FAIL; [ "$why" = " whole-core-failed" ] && kind=BUG
    echo "$kind $be $mode ${file#"$ROOT"/}:$why"
    diff "$OUT/w.out" "$OUT/p.out" | head -5 | sed 's/^/    out /'
    diff "$OUT/w.err" "$OUT/p.err" | head -5 | sed 's/^/    err /'
    exit 1
  fi
  exit 0
}

if [ "${1:-}" = "--one" ]; then shift; one "$@"; fi

SHARD=""; BACKENDS="c native"; JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc)
while [ $# -gt 1 ]; do
  case $1 in
    --backend) BACKENDS=$2; shift 2 ;;
    --jobs) JOBS=$2; shift 2 ;;
    --shard) SHARD=$2; shift 2 ;;
    *) break ;;
  esac
done
SET=${1:?usage: prune-differential.sh [--backend c|native] [--jobs N] corpus|combos|combos-subset}

LIST=$(mktemp "${TMPDIR:-/tmp}/prunediff-list.XXXXXX"); GEN=""
trap 'rm -f "$LIST"; [ -z "$GEN" ] || rm -rf "$GEN"' EXIT

corpus_cases() {
  local f
  (cd "$ROOT" && KAI_CORPUS_DIRS=$(find examples -type d | sort) kai_corpus_entry_points) | while read -r f; do
    [ -f "$ROOT/${f%.kai}.out.expected" ] || continue
    grep -qxF "$f" "$SKIPS" 2>/dev/null && continue
    echo "run $ROOT/$f"
    if grep -q '^[[:space:]]*test "' "$ROOT/$f"; then echo "test $ROOT/$f"; echo "cover $ROOT/$f"; fi
  done
}

case $SET in
  corpus) corpus_cases > "$LIST" ;;
  combos|combos-subset)
    GEN=$(mktemp -d "${TMPDIR:-/tmp}/prunecombos.XXXXXX")
    export PRUNE_DIFF_EXPECT_OK=1
    "$ROOT/tools/prune-combos.sh" "$GEN" "$SET" > "$LIST" ;;
  *) echo "unknown set $SET" >&2; exit 2 ;;
esac

if [ -n "$SHARD" ]; then
  awk -v i="${SHARD%/*}" -v n="${SHARD#*/}" '(NR - i) % n == 0' "$LIST" > "$LIST.shard" && mv "$LIST.shard" "$LIST"
fi
TOTAL=$(wc -l < "$LIST"); FAILS=$(mktemp "${TMPDIR:-/tmp}/prunediff-fails.XXXXXX")
for be in $BACKENDS; do
  while read -r mode file; do printf '%s\0%s\0%s\0' "$be" "$mode" "$file"; done < "$LIST" \
    | xargs -0 -n 3 -P "$JOBS" "$SELF" --one >> "$FAILS" 2>&1
done
n=$(grep -c '^FAIL\|^BUG' "$FAILS"); b=$(grep -c '^BUG' "$FAILS")
cat "$FAILS"; rm -f "$FAILS"
echo "prune-differential: $SET: $TOTAL cases x [$BACKENDS], $n failed ($b not pruning: the whole core fails too)"
[ "$n" -eq 0 ]
