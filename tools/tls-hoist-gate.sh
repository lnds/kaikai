#!/usr/bin/env bash
# tls-hoist-gate.sh — thread-local hoist gate for the hot runtime bitcode.
#
# THE DEFECT CLASS. A fiber parks on thread A and resumes on thread B. Any
# thread-local slot ADDRESS resolved before the park is stale after it. This is
# not a compiler bug: C and LLVM both guarantee thread identity is constant for
# the duration of a function activation, and `llvm.threadlocal.address` is
# `speculatable memory(none)`, so hoisting, sinking and CSE-ing the address are
# all legal. The runtime is what has to hold up its end.
#
# THE PROPERTY. The address must be materialised inside a `noinline` function
# that keeps it — then it is resolved and consumed within one activation, on
# whatever thread is running, and no caller frame can cache it across a switch.
#
# WHY BITCODE AND NOT TEXT. A textual ban on bare `kai_active_fiber` is both too
# strict and too loose: a dozen bare uses in runtime.h are provably fine (pre-swap
# writes on a thread that has not switched; fresh-entry reads at the top of a
# trampoline), while a use that looks identical inside an inlinable helper is not.
# The invariant is about where the address materialises after optimisation, which
# is a question only the bitcode can answer.
#
# THE COMPANION GATE. gen-runtime-bc.sh already proves no function in the hot
# bitcode reaches swapcontext — that the CALLEE does not switch. It cannot prove
# the CALLER does not, and the caller is an emitted kaikai frame that by
# construction spans parks. This gate covers the other side of the call.
#
# CLASSES (second column of tools/tls-hoist.allow):
#   accessor  every function materialising the address is `noinline` and keeps
#             it. Verified here, so the entry cannot rot.
#   exposed   at least one materialising function is inlinable. Tracked debt:
#             listed so a NEW one fails the build, not because it is safe.
#
# The gate fails on: an unlisted symbol (new debt), an `accessor` entry that is
# no longer accessor-only (regression), an `exposed` entry that has become
# accessor-only (ratchet — promote it), and an entry no longer referenced (stale).
#
# USAGE
#   tools/tls-hoist-gate.sh                # gate the generated hot bitcode
#   tools/tls-hoist-gate.sh --report       # print the classification, exit 0
#   tools/tls-hoist-gate.sh --self-test    # prove the classifier discriminates
#   LLVM_DIS=... CLANG18=... tools/tls-hoist-gate.sh   # tool-resolution hints
#
# EXIT: 0 pass or clean skip (no bitcode / no llvm-dis), 1 gate failure,
# 2 configuration error.

set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
ALLOW="${TLS_HOIST_ALLOW:-$ROOT/tools/tls-hoist.allow}"
ANALYZER="$ROOT/tools/lib/tls-refs.awk"     # IR        -> reference triples
VERDICT="$ROOT/tools/lib/tls-verdict.awk"   # triples   -> allow-list verdict
FIXTURE="$ROOT/tools/lib/tls-refs-selftest.ll"
BITCODE=("$ROOT/stage0/runtime_llvm.bc" "$ROOT/stage0/runtime_inline.bc")

MODE="${1:-}"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

# llvm-dis must match the clang that wrote the bitcode (writer <= reader), so
# look next to the resolved clang 18 first. Both distributions gen-runtime-bc.sh
# names — brew llvm@18, apt llvm-18 — ship it in the same bin directory.
resolve_llvm_dis() {
  local clang bindir="" c
  clang="$(CLANG18="${CLANG18:-}" "$ROOT/tools/gen-runtime-bc.sh" --clang || true)"
  # Resolve through PATH first: gen-runtime-bc.sh may hand back a bare name.
  [ -n "$clang" ] && bindir="$(dirname "$(command -v "$clang" || echo "$clang")")"
  for c in ${LLVM_DIS:-} ${bindir:+"$bindir/llvm-dis"} llvm-dis llvm-dis-18; do
    c="$(command -v "$c" || true)"
    [ -n "$c" ] && { echo "$c"; return 0; }
  done
  return 1
}

# Reference triples for every module, merged. Each is analysed on its own —
# attribute-group numbering is per-module — and the analyser reads its input
# twice, because a body cites an attribute group the module declares at the end.
triples() {
  local f
  for f in "$@"; do awk -f "$ANALYZER" "$f" "$f"; done | sort -u
}

# --self-test proves the classifier tells the three shapes apart, on hand-written
# IR that needs no compiler, and that the policy then rejects what it flagged.
# A gate that cannot fail is not a gate.
self_test() {
  local want="st_hoistable exposed st_read_hoistable
st_leaked exposed st_addr_of_leaked
st_safe accessor st_read_safe"
  triples "$FIXTURE" > "$WORK/probe"
  awk -v report=1 -f "$VERDICT" /dev/null "$WORK/probe" > "$WORK/report"
  if ! diff <(echo "$want") "$WORK/report" >&2; then
    echo "tls-hoist-gate: SELF-TEST FAILED — classifier disagrees with the fixture (want <, got >)" >&2
    return 1
  fi
  if awk -f "$VERDICT" /dev/null "$WORK/probe" >/dev/null 2>&1; then
    echo "tls-hoist-gate: SELF-TEST FAILED — an unlisted thread-local passed the gate" >&2
    return 1
  fi
  echo "tls-hoist-gate: self-test OK (inlinable and address-returning accessors are both rejected)"
}

if [ "$MODE" = "--self-test" ]; then self_test; exit; fi

present=$(ls "${BITCODE[@]}" 2>/dev/null || true)
if [ -z "$present" ]; then
  # P2 is optional by design: no clang 18 means no hot bitcode, which means the
  # runtime never inlines into a fiber frame and there is nothing to gate.
  echo "tls-hoist-gate: no hot bitcode (P2 opted out) — nothing to gate."
  exit 0
fi

DIS="$(resolve_llvm_dis || true)"
if [ -z "$DIS" ]; then
  echo "tls-hoist-gate: WARNING — llvm-dis not found next to clang 18 nor on PATH; the hot bitcode is NOT gated for thread-local hoists." >&2
  echo "tls-hoist-gate:   set LLVM_DIS=/path/to/llvm-dis to restore the gate." >&2
  exit 0
fi

lls=()
for bc in $present; do
  ll="$WORK/$(basename "$bc").ll"
  "$DIS" "$bc" -o "$ll" || { echo "tls-hoist-gate: llvm-dis failed on $bc" >&2; exit 2; }
  lls+=("$ll")
done
triples "${lls[@]}" > "$WORK/triples"

if [ "$MODE" = "--report" ]; then
  awk -v report=1 -f "$VERDICT" /dev/null "$WORK/triples"
  exit 0
fi
awk -f "$VERDICT" "$ALLOW" "$WORK/triples"
