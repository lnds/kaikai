#!/bin/sh
# assert-runtime-bc.sh — fail if the native P2 bitcode is NOT active.
#
# P2 (docs/native-codegen-perf-plan.md §P2) generates the runtime bitcode at
# build time, gated to clang 18; without it the native build silently falls
# back to the legacy cc-links-runtime_llvm.c path (correct, just no O2
# inlining of the runtime). That silent opt-out is fine for a casual dev
# without clang 18 — but a RELEASE or the native CI gate must NOT ship in the
# slow path unnoticed. This script is the assert: it runs after the bitcode
# generation step and fails the job if P2 did not turn on, so a base-image
# change that drops clang 18 turns a silent perf regression into a red build.
#
# USAGE  tools/assert-runtime-bc.sh   (run after make / gen-runtime-bc.sh)
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
status="$("$ROOT/tools/gen-runtime-bc.sh" --status)"
if [ "$status" != "active" ]; then
  echo "::error::native P2 runtime bitcode is NOT active (status: $status)." >&2
  echo "  This build links runtime_llvm.c with cc (legacy, no O2 inlining) instead" >&2
  echo "  of linking the bitcode before O2. A release/CI build MUST run P2." >&2
  echo "  Cause is almost always a missing clang 18 on the build image." >&2
  echo "  Fix: ensure clang 18 is installed (brew install llvm@18 / apt-get install clang-18)" >&2
  echo "  and that 'make -C stage2 KAI_LLVM=1 kaic2' regenerated stage0/runtime_llvm.bc." >&2
  exit 1
fi
echo "assert-runtime-bc: native P2 bitcode is active."
