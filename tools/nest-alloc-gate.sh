#!/bin/bash
# Nesting-allocation gate — ceilings on the cells the typer allocates
# checking list literals and lambdas nested as deep as the parser allows.
#
# A pass that applies the substitution to a whole type at each level of a
# walk over that type is quadratic in its depth, and cubic once the walk
# runs at each level of the nest: the occurs check did, and twenty 250-deep
# list literals cost 60M cells and 2.6 GB. Building each lambda's type
# applied did the quadratic half for nested lambdas. The output stays
# right, so only a ceiling catches the shape coming back.

set -eu
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAIC2="$ROOT/stage2/kaic2"
WORK="$ROOT/stage2/build/nest-alloc"
DEPTH=250
COPIES=20

[ -x "$KAIC2" ] || { echo "nest-alloc: SKIP — no stage2/kaic2"; exit 0; }

rm -rf "$WORK"; mkdir -p "$WORK"

# check <name> <ceiling> <open> <close>: COPIES fns, each nesting DEPTH times
check() {
  local name="$1" ceiling="$2" open close
  open="$(for ((d = 0; d < DEPTH; d++)); do printf '%s' "$3"; done)"
  close="$(for ((d = 0; d < DEPTH; d++)); do printf '%s' "$4"; done)"
  {
    for ((i = 0; i < COPIES; i++)); do
      echo "fn f${i}() : Int = {"
      echo "  let v = ${open}${i}${close}"
      echo "  0"
      echo "}"
    done
    echo "fn main() : Int = 0"
  } > "$WORK/$name.kai"
  (cd "$WORK" && KAI_TRACE_RC=1 KAI_THREADS=1 "$KAIC2" --check "$name.kai" > /dev/null 2> "$name.log") \
    || { cat "$WORK/$name.log"; echo "nest-alloc: FAIL — $name did not check"; exit 1; }
  local cells
  cells="$(grep -oE 'alloc_total=[0-9]+' "$WORK/$name.log" | head -1 | cut -d= -f2)"
  [ -n "$cells" ] || { echo "nest-alloc: FAIL — $name: no allocation count"; exit 1; }
  if [ "$cells" -gt "$ceiling" ]; then
    echo "nest-alloc: FAIL — ${COPIES} ${name} nested ${DEPTH} deep: $cells cells > $ceiling"
    exit 1
  fi
  echo "nest-alloc OK (${COPIES} ${name} nested ${DEPTH} deep: $cells cells <= $ceiling)"
}

check lists   "${KAI_NEST_ALLOC_LISTS_CEILING:-8500000}"    '['        ']'
check lambdas "${KAI_NEST_ALLOC_LAMBDAS_CEILING:-17500000}" '(x) => '  ''
