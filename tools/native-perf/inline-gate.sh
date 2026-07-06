#!/bin/sh
# native-perf/inline-gate.sh — assert the native backend inlines the runtime
# ops into the hot path.
#
# The native emitter stamps every program function with the runtime bitcode's
# host target-cpu/features so `areInlineCompatible` lets the opt pipeline fold
# the `kaix_*` runtime ops into the caller. Without that stamp the inliner
# refuses EVERY runtime-op inline on a legality (not cost) basis, and the
# rb-tree hot loop pays a call per field read / re-box — the ~6x instruction /
# ~2.7x wall native-vs-C blow-up.
#
# This gate compiles the rb-tree bench with the native backend, disassembles
# the binary, and counts residual `bl <kaix_*>` call sites. With inlining the
# count is ~168 (matches clang -O2); without it ~1168. A threshold of 400
# catches the regression (a broken stamp jumps back past 1000) with generous
# margin for LLVM-version inlining drift.
#
# Native-only. SKIP where libLLVM is absent (no native backend to gate).
set -eu
_ZO_DOCTOR=0; export _ZO_DOCTOR

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="${KAI:-$ROOT/bin/kai}"
SRC="$ROOT/examples/perceus/rb_tree_bench.kai"
THRESHOLD="${INLINE_GATE_THRESHOLD:-400}"

LLVM_CONFIG="${LLVM_CONFIG:-llvm-config}"
if ! command -v "$LLVM_CONFIG" >/dev/null 2>&1; then
  echo "native-perf/inline-gate: SKIP (llvm-config not in PATH; no native backend)"
  exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
BIN="$TMP/rb_native"

"$KAI" build --backend=native "$SRC" -o "$BIN" >/dev/null 2>"$TMP/build.err" || {
  echo "native-perf/inline-gate FAIL — native build failed:"; cat "$TMP/build.err"; exit 1
}

# Count `bl <symbol containing kaix_>` call sites in the disassembly. macOS
# ships `otool`; Linux CI uses `objdump`. Both mangle the runtime symbols with
# a leading `_` on Mach-O and none on ELF, so match `kaix_` anywhere in the
# operand.
if command -v otool >/dev/null 2>&1; then
  CALLS="$(otool -tV "$BIN" | grep -c 'bl.*kaix_' || true)"
elif command -v objdump >/dev/null 2>&1; then
  CALLS="$(objdump -d "$BIN" | grep -cE '\b(bl|call)\b.*kaix_' || true)"
else
  echo "native-perf/inline-gate: SKIP (no otool/objdump to disassemble)"
  exit 0
fi

echo "native-perf/inline-gate: residual kaix_ calls = $CALLS (threshold $THRESHOLD)"
if [ "$CALLS" -gt "$THRESHOLD" ]; then
  echo "::error::native-perf/inline-gate FAIL — $CALLS kaix_ calls exceeds $THRESHOLD;"
  echo "  the emitter is no longer stamping host target-features, so the inliner"
  echo "  cannot fold runtime ops (see kai_llvm_stamp_host_features)."
  exit 1
fi
echo "native-perf/inline-gate OK — runtime ops inline into the hot path."

# Variant fast-path gate: a primitive-slot ctor (RBNode's Int keys) must route
# through the fast entry `kai_variant_u_fast` (via `kaix_variant_masked`), not
# the cold `kai_variant_u` whose per-call name/mask register + immortal-args
# hash scan dominated the native-vs-C wall. A regression that drops the routing
# reverts every RBNode to `kai_variant_u`, so assert the fast entry fires.
if command -v otool >/dev/null 2>&1; then
  MASKED="$(otool -tV "$BIN" | grep -c 'bl.*kaix_variant_masked' || true)"
elif command -v objdump >/dev/null 2>&1; then
  MASKED="$(objdump -d "$BIN" | grep -cE '\b(bl|call)\b.*kaix_variant_masked' || true)"
else
  MASKED=1
fi
echo "native-perf/inline-gate: kaix_variant_masked calls = $MASKED (expect > 0)"
if [ "$MASKED" -eq 0 ]; then
  echo "::error::native-perf/inline-gate FAIL — no kaix_variant_masked calls;"
  echo "  primitive-slot ctors reverted to the cold kai_variant_u path (see"
  echo "  nemit_con / ls_ctor_mask / nproto_register_payload_ctors)."
  exit 1
fi
echo "native-perf/inline-gate OK — primitive-slot ctors take the fast variant entry."
