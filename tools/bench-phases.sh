#!/usr/bin/env bash
# Phase-by-phase wall breakdown of `kai build empty.kai`. Cited in
# `docs/cache-design.md`. Re-run after any caching lane closes to
# verify the breakdown still holds.
#
# Mirrors the `--prelude` list emitted by `bin/kai`'s
# `stdlib_prelude_flags`. Keep them in sync when a new prelude lands
# in the driver — otherwise the bench is measuring a different
# program than `kai build` does.
#
# Usage: tools/bench-phases.sh [-r] [-q]
#   -r   also report median maximum-RSS (MB) in a second column.
#   -q   skip the no-stdlib control row (faster).
# Reqs:  stage2/kaic2 built (run `make -C stage2 kaic2` first).
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
STDLIB_ROOT="$ROOT/stdlib"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

show_rss=0
skip_no_stdlib=0
for a in "$@"; do
  case "$a" in
    -r) show_rss=1 ;;
    -q) skip_no_stdlib=1 ;;
    *)  echo "usage: bench-phases.sh [-r] [-q]" >&2; exit 2 ;;
  esac
done

SRC="$TMP/empty.kai"
printf 'fn main() : Unit { () }\n' > "$SRC"

[ -x "$KAIC2" ] || { echo "kaic2 not built; run: make -C stage2 kaic2" >&2; exit 1; }

# Hanga Roa: core (core/*, protocols, effects, array) is loaded
# automatically by the compiler via the compile-time KAI_STDLIB_PATH
# constant. No --prelude flag is needed; opt-in modules require
# `import` in user code.
prelude_flags=""

# /usr/bin/time -p reports `real <sec>` on stderr. On macOS the same
# binary also accepts `-l` to print maximum-resident-set-size on a
# separate stderr line. We sample 5 runs and take the median for both.
median_real() {
  for _ in 1 2 3 4 5; do
    /usr/bin/time -p sh -c "$1" 2>&1 | awk '/^real/{print $2}'
  done | sort -n | sed -n '3p'
}

# RSS in MB (macOS reports bytes; Linux's `time -v` reports KB and has
# a different output shape — pin to macOS for now since the bench
# anchor is M2 Pro).
median_rss_mb() {
  for _ in 1 2 3 4 5; do
    /usr/bin/time -l sh -c "$1" 2>&1 | awk '
      /maximum resident set size/ { printf "%.0f\n", $1 / 1048576 }'
  done | sort -n | sed -n '3p'
}

run_row() {
  label=$1; cmd=$2
  if [ "$show_rss" = "1" ]; then
    printf "%-22s %s    %s\n" "$label" "$(median_real "$cmd")" "$(median_rss_mb "$cmd")"
  else
    printf "%-22s %s\n" "$label" "$(median_real "$cmd")"
  fi
}

if [ "$show_rss" = "1" ]; then
  echo "stage                  median(s) rss(MB)"
  echo "---------------------- --------- -------"
else
  echo "stage                  median(s)"
  echo "---------------------- --------"
fi
[ "$skip_no_stdlib" = "1" ] || \
  run_row "no-stdlib"        "$KAIC2 --path $STDLIB_ROOT $SRC > $TMP/out.c 2>/dev/null"
run_row "tokens (lex)"       "$KAIC2 --tokens --path $STDLIB_ROOT $prelude_flags $SRC > $TMP/out 2>/dev/null"
run_row "ast (parse)"        "$KAIC2 --ast --path $STDLIB_ROOT $prelude_flags $SRC > $TMP/out 2>/dev/null"
run_row "check (typecheck)"  "$KAIC2 --check --path $STDLIB_ROOT $prelude_flags $SRC > $TMP/out 2>/dev/null"
run_row "infer (typecheck)"  "$KAIC2 --infer --path $STDLIB_ROOT $prelude_flags $SRC > $TMP/out 2>/dev/null"
run_row "default (emit C)"   "$KAIC2 --path $STDLIB_ROOT $prelude_flags $SRC > $TMP/out.c 2>/dev/null"
