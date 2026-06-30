#!/bin/bash
# Native self-host gate (issue #1021).
#
# The in-process libLLVM native backend (`kaic2 --emit=native`) is the
# DEFAULT since the Lane 1.5 flip, and the native-vs-C parity ratchet
# reached zero — but that ratchet covers ~620 USER-PROGRAM fixtures. No
# test compiles the COMPILER ITSELF with the native backend (the compiler
# is built C-only: bootstrap + selfhost oracle). So any KIR construct the
# native backend cannot yet lower, but the compiler's own source uses, is
# NEVER exercised in CI — the subset gap is silent. native looks
# "complete" because nothing that uses the missing pieces is tested.
#
# This gate makes the gap VISIBLE: it compiles `stage2/main.kai` (the
# modular compiler entry point) with `--emit=native` and counts the
# `unbound register` subset-gap aborts. It does NOT block: in the current
# expected-fail state the count equals the baseline and the gate PASSES.
# It fails ONLY on REGRESSION (the count rises). See
# tools/native-selfhost-baseline.txt for the ratchet semantics.
#
# This gate does NOT close the gap (driving the count to 0 is separate,
# potentially large work in emit_native_fn.kai + kir_lower_*.kai, tracked
# by #1021). It installs the meter + baseline + regression guard only.
#
# Additive and native-only: it does NOT touch the C bootstrap or the
# selfhost C-byte-id check. It exits 0 (SKIP) where libLLVM is absent.

set -eu

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

BASELINE_FILE="$ROOT/tools/native-selfhost-baseline.txt"

LLVM_CONFIG="${LLVM_CONFIG:-llvm-config}"
if ! command -v "$LLVM_CONFIG" >/dev/null 2>&1; then
  echo "native-selfhost-gate: SKIP (llvm-config not in PATH; native backend needs libLLVM)"
  exit 0
fi

CC="${CC:-cc}"
KAIC2="$ROOT/stage2/kaic2"
RUNTIME_LLVM_BC="$ROOT/stage0/runtime_llvm.bc"
[ -f "$RUNTIME_LLVM_BC" ] || RUNTIME_LLVM_BC=""

# Read the baseline: the single non-comment, non-blank integer line.
if [ ! -f "$BASELINE_FILE" ]; then
  echo "native-selfhost-gate FAIL — baseline file missing: $BASELINE_FILE"
  exit 1
fi
baseline="$(grep -vE '^[[:space:]]*(#|$)' "$BASELINE_FILE" | head -1 | tr -d '[:space:]')"
case "$baseline" in
  ''|*[!0-9]*)
    echo "native-selfhost-gate FAIL — baseline is not an integer: '$baseline'"
    exit 1 ;;
esac

# The native backend needs a kaic2 linked against libLLVM. `make` keys
# off timestamps, not flags, so a kaic2 built WITHOUT KAI_LLVM is "up to
# date" w.r.t. build/stage2.c and `make KAI_LLVM=1 kaic2` would not
# relink it. Force the relink by removing the binary first; this only
# redoes the final link (seconds).
echo "native-selfhost-gate: building kaic2 with KAI_LLVM=1 …"
rm -f "$KAIC2"
LLVM_CONFIG="$LLVM_CONFIG" make -C "$ROOT/stage2" KAI_LLVM=1 kaic2 >/dev/null 2>&1 \
  || { echo "native-selfhost-gate FAIL — kaic2 (KAI_LLVM=1) build failed"; exit 1; }
[ -x "$KAIC2" ] || { echo "native-selfhost-gate FAIL — kaic2 not found at $KAIC2"; exit 1; }

# Compile the compiler with itself, native backend. Run from stage2/ so
# `import compiler.driver` in main.kai resolves against compiler/*.kai
# (the modular path kaic2 takes — kaic1's concatenated bundle is the
# bootstrap path and is irrelevant here). The native spine derives the
# object path from the source (main.kai -> main.o) and ignores -o, so we
# pass no -o and clean main.o (+ any partial compiler/*.o) afterwards.
ERR="$(mktemp)"
trap 'rm -f "$ERR" "$ROOT/stage2/main.o"; find "$ROOT/stage2/compiler" -name "*.o" -delete 2>/dev/null || true' EXIT

echo "native-selfhost-gate: compiling stage2/main.kai with --emit=native …"
rc=0
( cd "$ROOT/stage2" \
  && env KAI_NATIVE_RUNTIME_BC="$RUNTIME_LLVM_BC" \
       "$KAIC2" --emit=native --path "$ROOT/stdlib" main.kai >/dev/null 2>"$ERR" ) || rc=$?
obj_produced=0
[ -f "$ROOT/stage2/main.o" ] && obj_produced=1

count="$(grep -c 'unbound register' "$ERR" || true)"
count="${count:-0}"

# A zero count with a FAILED compile that produced no object is NOT the
# gap-closed milestone — it is an unrelated failure (typer/parser/link or
# a NEW subset-gap class that is not an `unbound register`). Surface it
# loudly instead of mis-reporting it as ACHIEVED or as a silent pass.
if [ "$count" -eq 0 ] && { [ "$rc" -ne 0 ] || [ "$obj_produced" -ne 1 ]; }; then
  echo "::error::native-selfhost-gate FAIL — the native self-compile failed (exit $rc, object produced=$obj_produced) with ZERO 'unbound register' aborts."
  echo "  This is NOT the known subset gap (which is all 'unbound register'). Likely an unrelated break or a new gap class. First 40 stderr lines:"
  head -40 "$ERR" | sed 's/^/    /'
  exit 1
fi

echo "native-selfhost-gate: unbound-register subset-gap count = $count (baseline $baseline)"
if [ "$count" -gt 0 ]; then
  echo "  breakdown by register name:"
  grep 'unbound register' "$ERR" \
    | sed -E 's/.*unbound register ([a-zA-Z0-9_]+).*/\1/' \
    | sort | uniq -c | sort -rn \
    | sed 's/^/    /'
fi

if [ "$count" -gt "$baseline" ]; then
  echo "::error::native-selfhost-gate FAIL — REGRESSION: $count subset-gap aborts > baseline $baseline."
  echo "  The native backend lost ground: it now rejects more of the compiler's KIR than before."
  echo "  Either fix the regression, or (if intentional and justified) raise the baseline in $BASELINE_FILE."
  exit 1
elif [ "$count" -eq 0 ]; then
  echo "native-selfhost-gate: native self-host ACHIEVED — the compiler lowers under the native backend with zero subset-gap aborts."
  echo "  Next: upgrade this gate to require the native object to LINK + RUN, and close #1021."
  exit 0
elif [ "$count" -lt "$baseline" ]; then
  echo "native-selfhost-gate: PASS — baseline improvable; part of the gap closed."
  echo "  Lower the baseline in $BASELINE_FILE from $baseline to $count to tighten the ratchet."
  exit 0
else
  echo "native-selfhost-gate: PASS — known gap held at baseline $baseline (documented expected-fail; #1021 still open)."
  exit 0
fi
