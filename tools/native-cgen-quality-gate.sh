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
FN="rb_tree__insert_loop"

# Spill ratio ceiling, percent. On arm64 the Default codegen level reads 23%
# and the None level 44%, so the ceiling sits between them with room on both
# sides — close enough to catch the regression, far enough that ordinary
# front-end churn does not trip it. x86-64 has fewer registers and folds
# memory operands into arithmetic, so its healthy ratio is higher; its
# ceiling is set alongside its spill pattern below.
mem_ceiling=33

mkdir -p "$WORK"
fail() { echo "native-cgen-quality FAIL: $1" >&2; exit 1; }

# Disassembly differs per platform in three ways this gate depends on:
# the tool, the symbol decoration (Mach-O prefixes `_`), and the mnemonics
# that count as a stack access.
case "$(uname -s)" in
  Darwin) DISASM="otool -tV"; SYM="_$FN" ;;
  *)      DISASM="objdump -d"; SYM="$FN" ;;
esac

case "$(uname -m)" in
  arm64|aarch64) SPILL='(ldr|ldp|str|stp|ldur|stur)' ;;
  # x86-64 has no load/store mnemonics: a stack access is any instruction
  # touching an rbp/rsp-relative operand. Its healthy ratio is higher than
  # arm64's — fewer registers, and memory operands fold into arithmetic.
  *)             SPILL='(%rbp\)|%rsp\))'; mem_ceiling=55 ;;
esac

MAX_SPILL_PCT="$mem_ceiling"

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

# Slice the hot function out. otool labels a function `_name:` at column 0;
# objdump labels it `<name>:` after an address. Both end at the next label.
awk -v sym="$SYM" '
  $0 ~ ("^" sym ":") || $0 ~ ("<" sym ">:") { inside = 1; next }
  inside && (/^[A-Za-z_][A-Za-z_0-9.]*:/ || /^[0-9a-f]+ </) { inside = 0 }
  inside && NF { print }
' "$WORK/disasm.txt" > "$WORK/loop.txt"

total=$(grep -c . "$WORK/loop.txt" || true)
if [ "${total:-0}" -le 100 ]; then
  echo "native-cgen-quality SKIP — could not isolate $SYM (found ${total:-0} lines)." >&2
  echo "  The symbol may be inlined away or renamed on this platform;" >&2
  echo "  skipping rather than failing, since a missing symbol is not a" >&2
  echo "  codegen-quality verdict." >&2
  exit 0
fi

spill=$(grep -cE "$SPILL" "$WORK/loop.txt" || true)
pct=$(( spill * 100 / total ))

echo "native-cgen-quality: $SYM — $total instructions, $spill stack access (${pct}%)"

if [ "$pct" -ge "$MAX_SPILL_PCT" ]; then
  fail "spill ratio ${pct}% >= ${MAX_SPILL_PCT}% — the register allocator is spilling the descent's live values.
       Most likely a codegen-level or pass-pipeline change. Compare against
       KAI_NATIVE_CGLEVEL=0 (the fast-emit level) to confirm."
fi

echo "native-cgen-quality PASS — spill ratio ${pct}% under the ${MAX_SPILL_PCT}% ceiling"
