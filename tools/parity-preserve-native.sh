#!/bin/bash
# Rebuild kaic2 for the backend-parity gate WITHOUT downgrading it.
#
# `stage2/Makefile` resolves an unset KAI_LLVM from `llvm-config` on PATH.
# Homebrew's LLVM is keg-only, so the usual mac shell has no llvm-config and
# the auto-detect answers "C-only" — relinking a native kaic2 as C-only. The
# harness then probes native, finds it gone, and SKIPs with exit 0: a green
# that verified nothing, having itself destroyed the capability it needed.
#
# The invariant: a rebuild for this gate never LOWERS the tree's backend
# capability. C-only in, C-only out (the harness SKIPs, as designed for a
# genuinely C-only checkout); native in, native out or a loud stop.

set -eu

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAI="$ROOT/bin/kai"

native_capable() {
  [ -x "$ROOT/stage2/kaic2" ] || return 1
  p="$(mktemp -d)/probe.kai"
  printf 'fn main() : Int = 0\n' > "$p"
  rc=0
  "$KAI" build --backend=native "$p" -o "${p%.kai}.bin" >/dev/null 2>&1 || rc=$?
  rm -rf "$(dirname "$p")"
  return $rc
}

if native_capable; then
  was_native=1
else
  was_native=0
fi

# Resolve llvm-config the same way stage2/Makefile does, so a keg-only
# install found via `brew --prefix llvm` is offered rather than guessed at.
resolve_llvm_config() {
  if [ -n "${LLVM_CONFIG:-}" ]; then
    command -v "$LLVM_CONFIG" >/dev/null 2>&1 && { echo "$LLVM_CONFIG"; return 0; }
    [ -x "$LLVM_CONFIG" ] && { echo "$LLVM_CONFIG"; return 0; }
    return 1
  fi
  command -v llvm-config >/dev/null 2>&1 && { command -v llvm-config; return 0; }
  if command -v brew >/dev/null 2>&1; then
    for f in llvm llvm@18; do
      c="$(brew --prefix "$f" 2>/dev/null)/bin/llvm-config"
      [ -x "$c" ] && { echo "$c"; return 0; }
    done
  fi
  return 1
}

if [ "$was_native" = 1 ]; then
  llvm_config="$(resolve_llvm_config)" || {
    echo "tier1-backend-parity FAIL — this tree has a NATIVE kaic2, but no llvm-config resolves here." >&2
    echo "  Rebuilding it now would relink C-only and the gate would SKIP (exit 0) having" >&2
    echo "  verified nothing. Put llvm-config on PATH, or pass LLVM_CONFIG=/path/to/llvm-config" >&2
    echo "  (Homebrew keg-only: LLVM_CONFIG=\$(brew --prefix llvm)/bin/llvm-config)." >&2
    exit 1
  }
  echo "parity-preserve-native: native kaic2 detected — rebuilding with KAI_LLVM=1 ($llvm_config)"
  make KAI_LLVM=1 LLVM_CONFIG="$llvm_config" kaic2
  native_capable || {
    echo "tier1-backend-parity FAIL — the rebuild lost the native backend it started with." >&2
    exit 1
  }
elif [ "${KAI_LLVM:-}" = "1" ]; then
  # Native asked for explicitly on a C-only tree. stage2/Makefile stops loud
  # when llvm-config is missing; if it builds, the result must actually be
  # native — a SKIP here would be the same false green from the other side.
  echo "parity-preserve-native: KAI_LLVM=1 requested — rebuilding for native"
  if [ -n "${LLVM_CONFIG:-}" ]; then
    make KAI_LLVM=1 LLVM_CONFIG="$LLVM_CONFIG" kaic2
  else
    make KAI_LLVM=1 kaic2
  fi
  native_capable || {
    echo "tier1-backend-parity FAIL — KAI_LLVM=1 was requested but the rebuilt kaic2" >&2
    echo "  still rejects --backend=native. Not reporting SKIP over an explicit request." >&2
    exit 1
  }
else
  echo "parity-preserve-native: C-only kaic2 — rebuilding C-only (the harness will SKIP)"
  make kaic2
fi
