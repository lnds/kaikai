#!/bin/bash
# Nesting-allocation gate — a ceiling on the cells the typer allocates
# checking list literals nested as deep as the parser allows.
#
# A pass that applies the substitution to a whole type at each level of a
# walk over that type is quadratic in its depth, and cubic once the walk
# runs at each level of the literal: the occurs check did, and a nest of
# twenty 250-deep literals cost 60M cells and 2.6 GB. The output stays
# right, so only a ceiling catches the shape coming back.

set -eu
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAIC2="$ROOT/stage2/kaic2"
WORK="$ROOT/stage2/build/nest-alloc"
DEPTH=250
COPIES=20
CEILING="${KAI_NEST_ALLOC_CEILING:-8500000}"

[ -x "$KAIC2" ] || { echo "nest-alloc: SKIP — no stage2/kaic2"; exit 0; }

rm -rf "$WORK"; mkdir -p "$WORK"
open="$(printf '%*s' "$DEPTH" '' | tr ' ' '[')"
close="$(printf '%*s' "$DEPTH" '' | tr ' ' ']')"
{
  for ((i = 0; i < COPIES; i++)); do
    echo "fn f${i}() : Int = {"
    echo "  let xs = ${open}${i}${close}"
    echo "  0"
    echo "}"
  done
  echo "fn main() : Int = 0"
} > "$WORK/main.kai"

(cd "$WORK" && KAI_TRACE_RC=1 KAI_THREADS=1 "$KAIC2" --check main.kai > /dev/null 2> rc.log) \
  || { cat "$WORK/rc.log"; echo "nest-alloc: FAIL — the check did not pass"; exit 1; }
cells="$(grep -oE 'alloc_total=[0-9]+' "$WORK/rc.log" | head -1 | cut -d= -f2)"
[ -n "$cells" ] || { echo "nest-alloc: FAIL — no allocation count"; exit 1; }
if [ "$cells" -gt "$CEILING" ]; then
  echo "nest-alloc: FAIL — ${COPIES} literals nested ${DEPTH} deep: $cells cells > $CEILING"
  exit 1
fi
echo "nest-alloc OK (${COPIES} literals nested ${DEPTH} deep: $cells cells <= $CEILING)"
