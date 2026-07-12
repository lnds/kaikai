#!/bin/bash
# Native self-host x Layout gate.
#
# The native self-host gate (tools/test-native-selfhost-gate.sh) self-compiles
# a TRIVIAL sample (`println`). That sample carries no `Layout` type, so its
# `layout_types` is empty and `rewrite_layout_calls` early-returns — the whole
# layout-rewrite walk is never exercised by the native-built compiler. This
# gate closes that blind spot: it feeds the native-built kaic2 a Layout-BEARING
# program (non-empty `layout_types`), forcing the full walk to run under the
# native backend, and asserts the emitted C is byte-identical to the C-direct
# oracle. A native codegen bug in that walk (crash or corruption) fails here.
#
# Native-only: SKIPs (exit 0) where libLLVM is absent. Does not touch the C
# bootstrap.

set -eu

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

LLVM_CONFIG="${LLVM_CONFIG:-llvm-config}"
if ! command -v "$LLVM_CONFIG" >/dev/null 2>&1; then
  echo "native-selfhost-layout: SKIP (llvm-config not in PATH; native backend needs libLLVM)"
  exit 0
fi

CC="${CC:-cc}"
KAIC2="$ROOT/stage2/kaic2"
RUNTIME_LLVM_BC="$ROOT/stage0/runtime_llvm.bc"
[ -f "$RUNTIME_LLVM_BC" ] || RUNTIME_LLVM_BC=""

# The Layout-bearing input. Any fixture whose `type T = { f: U32<be> }`
# populates `layout_types` drives the walk; this one also uses encode/decode.
FIXTURE="$ROOT/examples/sugars/kinds_layout_encode_decode.kai"
[ -f "$FIXTURE" ] || { echo "native-selfhost-layout FAIL — missing fixture $FIXTURE"; exit 1; }

# Relink kaic2 with KAI_LLVM=1 (make keys off timestamps, not flags, so force
# the relink; seconds).
echo "native-selfhost-layout: building kaic2 with KAI_LLVM=1 …"
rm -f "$KAIC2"
LLVM_CONFIG="$LLVM_CONFIG" make -C "$ROOT/stage2" KAI_LLVM=1 kaic2 >/dev/null 2>&1 \
  || { echo "native-selfhost-layout FAIL — kaic2 (KAI_LLVM=1) build failed"; exit 1; }
[ -x "$KAIC2" ] || { echo "native-selfhost-layout FAIL — kaic2 not found at $KAIC2"; exit 1; }

trap 'rm -f "$ROOT/stage2/main.o" "$ROOT/stage2/kaic2-native-layout" "$ROOT/stage2/native-selfhost-shim.o"; find "$ROOT/stage2/compiler" -name "*.o" -delete 2>/dev/null || true' EXIT

# Compile the compiler with itself, native backend (main.kai -> main.o). Run
# from stage2/ so `import compiler.*` resolves against compiler/*.kai.
echo "native-selfhost-layout: compiling stage2/main.kai with --emit=native …"
rc=0
( cd "$ROOT/stage2" \
  && env KAI_NATIVE_RUNTIME_BC="$RUNTIME_LLVM_BC" \
       "$KAIC2" --emit=native --path "$ROOT/stdlib" main.kai >/dev/null 2>&1 ) || rc=$?
if [ ! -f "$ROOT/stage2/main.o" ]; then
  echo "::error::native-selfhost-layout FAIL — the native self-compile produced no object (exit $rc)."
  exit 1
fi

BIN="$ROOT/stage2/kaic2-native-layout"
echo "native-selfhost-layout: linking the native compiler object …"
if ! LLVM_CONFIG="$LLVM_CONFIG" CC="$CC" "$ROOT/tools/native-selfhost-link.sh" "$ROOT/stage2/main.o" "$BIN" >/dev/null 2>&1; then
  echo "::error::native-selfhost-layout FAIL — the native compiler object did NOT link."
  exit 1
fi
[ -x "$BIN" ] || { echo "::error::native-selfhost-layout FAIL — linker produced no $BIN"; exit 1; }

# The crux: emit C for the Layout-bearing fixture with BOTH compilers. The
# native-built one must not crash (the pre-fix bug SIGSEGV'd here) and must
# match the oracle byte-for-byte.
echo "native-selfhost-layout: emitting C for the Layout fixture on both backends …"
nc="$("$BIN" --emit=c --path "$ROOT/stdlib" "$FIXTURE" 2>/dev/null)"; ncrc=$?
oc="$("$KAIC2" --emit=c --path "$ROOT/stdlib" "$FIXTURE" 2>/dev/null)"; ocrc=$?
if [ "$ncrc" -ne 0 ]; then
  echo "::error::native-selfhost-layout FAIL — the native-built compiler failed on the Layout fixture (exit $ncrc). This is the #1201 crash class."
  exit 1
fi
if [ "$ocrc" -ne 0 ]; then
  echo "::error::native-selfhost-layout FAIL — the C-direct oracle failed on the Layout fixture (exit $ocrc). Unexpected — the oracle is the trusted reference."
  exit 1
fi
if [ "$nc" != "$oc" ]; then
  echo "::error::native-selfhost-layout FAIL — the native-built compiler's emitted C DIVERGES from the oracle on a Layout-bearing input."
  diff <(printf '%s' "$oc") <(printf '%s' "$nc") | head -20 | sed 's/^/    /'
  exit 1
fi
echo "native-selfhost-layout: OK — the native-built compiler emits byte-identical C for a Layout-bearing program."
exit 0
