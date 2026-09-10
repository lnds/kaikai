#!/bin/bash
# Compile-allocation gate — a ceiling on the cells the compiler allocates
# compiling a fixed generated program.
#
# Several compiler passes used to accumulate into a list inside a walk
# over the program: an append per module, a membership scan per lambda.
# Each is O(n^2) in program size and invisible to output-diffing tests —
# the compiler emits the right C and merely allocates more doing it.
# Pinning a ceiling on a fixed input catches a reintroduction: the sites
# this guards were together worth about half the cells this program costs.
#
# The ceiling has headroom for ordinary drift and is not a baseline to
# re-pin on every improvement; lower it when a lane wins real ground.

set -eu
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAIC2="$ROOT/stage2/kaic2"
WORK="$ROOT/stage2/build/compile-alloc"
MODS=40
CEILING="${KAI_COMPILE_ALLOC_CEILING:-9000000}"

[ -x "$KAIC2" ] || { echo "compile-alloc: SKIP — no stage2/kaic2"; exit 0; }

rm -rf "$WORK"; mkdir -p "$WORK/mods"

for ((i = 0; i < MODS; i++)); do
  {
    for ((j = 0; j < 8; j++)); do
      echo "pub fn m${i}_f${j}(x: Int) : Int = apply_it((v) => v + ${j}, x)"
    done
    echo "pub fn apply_it(f: (Int) -> Int, x: Int) : Int = f(x)"
  } > "$WORK/mods/m${i}.kai"
done
{
  for ((i = 0; i < MODS; i++)); do echo "import mods.m${i}"; done
  echo
  echo "fn main() : Unit / Stdout {"
  echo "  Stdout.print(\"#{m0_f0(1)}\")"
  echo "}"
} > "$WORK/main.kai"

( cd "$WORK" && KAI_TRACE_RC=1 KAI_THREADS=1 "$KAIC2" main.kai >/dev/null 2>rc.log )
cells=$(grep -oE 'alloc_total=[0-9]+' "$WORK/rc.log" | head -1 | cut -d= -f2)

[ -n "$cells" ] || { echo "compile-alloc FAIL — no alloc_total from the compiler"; exit 1; }

if [ "$cells" -le "$CEILING" ]; then
  echo "compile-alloc OK ($MODS modules: $cells cells <= $CEILING)"
else
  echo "compile-alloc FAIL ($MODS modules: $cells cells > $CEILING)"
  echo "  A pass is likely accumulating into a list inside a walk over the program."
  echo "  tools/superlinear-survey.py flags the candidates; the bound is the analyst's call."
  exit 1
fi
