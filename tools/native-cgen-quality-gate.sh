#!/bin/sh
# Native codegen-quality gate.
#
# Pins the register-allocation quality of the native backend's emitted code,
# measured as spill traffic in the rb-tree descent's hot loop. A codegen
# change that spends the allocator's budget shows up here as a spill ratio
# jump, which no other gate sees: the emitted program stays correct, the
# self-host stays byte-identical, and the build-modes check still passes,
# so only a number like this one can fail.
#
# The history this exists to prevent: dropping the TargetMachine codegen
# level to `None` for unqualified builds bought ~33% off the back-half of a
# 41-module rebuild and cost +116% retired instructions in every emitted
# program (spills in this loop went 349 -> 1508, 17% -> 40%). It shipped
# green and stood for twelve releases.
#
# Ratio, not absolute count: the loop's instruction count moves with every
# front-end change, but the fraction of it that is spill traffic is a
# property of the register allocator.
#
# Usage: native-cgen-quality-gate.sh <path-to-bin/kai> [workdir]
# Skips (exit 0) when the native backend is absent — nothing to measure.
set -eu

KAI="${1:?usage: native-cgen-quality-gate.sh <bin/kai> [workdir]}"
WORK="${2:-$(mktemp -d)}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$HERE/examples/perceus/rb_tree_bench.kai"
FN="_rb_tree__insert_loop"

# Spill ratio ceiling, percent. Measured 18% at the Default codegen level;
# the None level reads 40%. Anything at or above this means the allocator
# stopped keeping the descent's live values in registers.
MAX_SPILL_PCT=25

mkdir -p "$WORK"
fail() { echo "native-cgen-quality FAIL: $1" >&2; exit 1; }

case "$(uname -s)" in
  Darwin) DISASM="otool -tV" ;;
  *)      DISASM="objdump -d" ;;
esac

# Caches are keyed by content, and the codegen level rides the backend tag,
# but a stale object from an earlier run would still make this gate report on
# code the current compiler did not emit. Build cold.
BIN="$WORK/rb_cgen_quality"
if ! KAI_BACKEND=native KAI_CORE_CACHE=0 KAI_MODULAR_NO_CACHE=1 \
     "$KAI" build "$SRC" -o "$BIN" >"$WORK/build.log" 2>&1; then
  if grep -q "native backend unavailable" "$WORK/build.log"; then
    echo "native-cgen-quality SKIP — no native backend in this kaic2"
    exit 0
  fi
  fail "native build of $SRC failed (see $WORK/build.log)"
fi

$DISASM "$BIN" > "$WORK/disasm.txt" 2>/dev/null || fail "disassembly failed"

# Slice the hot function out of the disassembly and count spill traffic.
awk -v fn="$FN" '
  $0 ~ "^" fn ":" { inside = 1; next }
  inside && /^_[A-Za-z_]/ { inside = 0 }
  inside { print }
' "$WORK/disasm.txt" > "$WORK/loop.txt"

total=$(grep -c '	' "$WORK/loop.txt" || true)
[ "${total:-0}" -gt 100 ] || fail "could not isolate $FN (found ${total:-0} instructions; symbol renamed?)"

spill=$(grep -cE '	(ldr|ldp|str|stp|ldur|stur)[ 	]' "$WORK/loop.txt" || true)
pct=$(( spill * 100 / total ))

echo "native-cgen-quality: $FN — $total instructions, $spill load/store (${pct}%)"

if [ "$pct" -ge "$MAX_SPILL_PCT" ]; then
  fail "spill ratio ${pct}% >= ${MAX_SPILL_PCT}% — the register allocator is spilling the descent's live values.
       Most likely a codegen-level or pass-pipeline change. Compare against
       KAI_NATIVE_CGLEVEL=0 (the fast-emit level, which reads ~40%) to confirm."
fi

echo "native-cgen-quality PASS — spill ratio ${pct}% under the ${MAX_SPILL_PCT}% ceiling"
