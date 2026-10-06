#!/bin/bash
# Tail-call matrix gate — every call in tail position inside a recursive
# group runs in constant stack, on both backends.
#
# tools/tail-call-matrix/gen.py writes one program holding a loop per shape:
# a self or a mutual tail call, the callee taking as many, fewer or more
# parameters than the caller, with or without an effect row, with or
# without a parameter it never reads, on the main fiber or a spawned one.
# The program is built once per backend and run once per shape: five
# million turns on a 64 KB fiber stack, where a call that is not a jump
# overflows. A (backend, shape) listed in tools/tail-call-matrix/
# expected-fail.txt must still fail; a listed one that passes is an error
# too, so the list only ever shrinks.

set -u
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAI="$ROOT/bin/kai"
WORK="$ROOT/stage2/build/tail-call-matrix"
EXPECT="$ROOT/tools/tail-call-matrix/expected-fail.txt"
BACKENDS="${TAIL_MATRIX_BACKENDS:-c native}"

[ -x "$ROOT/stage2/kaic2" ] || { echo "tail-call-matrix: SKIP — no stage2/kaic2"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "tail-call-matrix: SKIP — no python3"; exit 0; }

rm -rf "$WORK"; mkdir -p "$WORK"
shapes="$(python3 "$ROOT/tools/tail-call-matrix/gen.py" "$WORK/matrix.kai")" || exit 1
fail=0; pass=0; xfail=0
for b in $BACKENDS; do
  exe="$WORK/matrix-$b"
  if ! KAI_BIN_MEMO=0 KAI_BACKEND="$b" "$KAI" build "$WORK/matrix.kai" -o "$exe" > "$exe.build" 2>&1; then
    echo "tail-call-matrix: FAIL — the $b build failed"; sed -n 1,12p "$exe.build"; fail=1; continue
  fi
  for s in $shapes; do
    key="$b $s"
    if KAI_THREADS=1 KAI_FIBER_STACK_SIZE=65536 "$exe" "$s" > "$exe.out" 2>&1; then ok=1; else ok=0; fi
    listed=0; grep -v '^#' "$EXPECT" | grep -qxF "$key" && listed=1
    if [ "$ok" = 1 ] && [ "$listed" = 1 ]; then
      echo "tail-call-matrix: FAIL — $key now passes; remove it from expected-fail.txt"; fail=1
    elif [ "$ok" = 1 ]; then
      pass=$((pass + 1))
    elif [ "$listed" = 1 ]; then
      xfail=$((xfail + 1))
    else
      echo "tail-call-matrix: FAIL — $key: $(head -c 160 "$exe.out")"; fail=1
    fi
  done
done 2>/dev/null
echo "tail-call-matrix: $pass pass, $xfail expected-fail"
[ "$fail" = 0 ] && echo "tail-call-matrix OK" || exit 1
