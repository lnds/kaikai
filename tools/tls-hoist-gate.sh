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
# THE OWNER. `--owner` gates the runtime owner objects instead, compiled at the
# owner's optimisation level (OWNER_OPT in tools/kai/cli_plan.kai). The owner
# holds the scheduler, so its frames do switch, and the property becomes: a
# function that materialises a thread-local address cannot switch context
# (tools/lib/switch-reach.awk) and does not hand the address to its caller, and
# the scheduler's own thread-local, `kai_worker`, is read by kai_worker_here
# alone. Code that crossed a switch reaches its worker through the running
# fiber instead. Exceptions are the pinned functions in tools/tls-owner.allow.
#
# USAGE
#   tools/tls-hoist-gate.sh                # gate the generated hot bitcode
#   tools/tls-hoist-gate.sh --owner        # gate the runtime owner objects
#   tools/tls-hoist-gate.sh --report       # print the classification, exit 0
#   tools/tls-hoist-gate.sh --self-test    # prove both classifiers discriminate
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
OWNER_ALLOW="${TLS_OWNER_ALLOW:-$ROOT/tools/tls-owner.allow}"
REACH="$ROOT/tools/lib/switch-reach.awk"            # IR -> switching functions
OWNER_VERDICT="$ROOT/tools/lib/tls-owner-verdict.awk"
OWNER_FIXTURE="$ROOT/tools/lib/tls-owner-selftest.ll"
BITCODE=("$ROOT/stage0/runtime_llvm.bc" "$ROOT/stage0/runtime_inline.bc")

MODE="${1:-}"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

# llvm-dis must be the version of the clang that wrote the bitcode.
resolve_llvm_dis() {
  local c
  for c in ${LLVM_DIS:-} "$(CLANG18="${CLANG18:-}" "$ROOT/tools/gen-runtime-bc.sh" --tool llvm-dis || true)"; do
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

# The owner policy over one or more modules, against an allow-list.
owner_check() {
  local allow="$1" f; shift
  for f in "$@"; do awk -f "$REACH" "$f" "$f"; done > "$WORK/switching"
  for f in "$@"; do awk -v detail=1 -f "$ANALYZER" "$f" "$f"; done | sort -u > "$WORK/owner-refs"
  awk -v sched=kai_worker -v accessor=kai_worker_here -f "$OWNER_VERDICT" \
    "$allow" "$WORK/switching" "$WORK/owner-refs"
}

# The owner fixture holds one function per shape; the failing ones are named
# here, and every other function must pass.
owner_self_test() {
  local bad="st_switches st_reaches st_indirect st_program st_leaks st_sched_direct"
  local good="kai_worker_here st_leaf st_noreturn" fn
  if owner_check /dev/null "$OWNER_FIXTURE" >/dev/null 2>"$WORK/owner-err"; then
    echo "tls-hoist-gate: SELF-TEST FAILED — the owner fixture passed the owner gate" >&2
    return 1
  fi
  for fn in $bad; do
    grep -qw "$fn" "$WORK/owner-err" || {
      echo "tls-hoist-gate: SELF-TEST FAILED — owner gate missed $fn" >&2; return 1; }
  done
  for fn in $good; do
    if grep -E "(in|leaves) $fn[ ,;]" "$WORK/owner-err" >&2; then
      echo "tls-hoist-gate: SELF-TEST FAILED — owner gate rejected $fn" >&2; return 1
    fi
  done
  printf 'st_switches pinned\nst_leaf pinned\n' > "$WORK/owner-allow"
  owner_check "$WORK/owner-allow" "$OWNER_FIXTURE" >/dev/null 2>"$WORK/owner-err" || true
  if grep -q "st_switches" "$WORK/owner-err" || ! grep -q "STALE.*st_leaf" "$WORK/owner-err"; then
    echo "tls-hoist-gate: SELF-TEST FAILED — the owner allow-list is not applied or not ratcheted" >&2
    return 1
  fi
  echo "tls-hoist-gate: owner self-test OK (switching, leaking and scheduler reads are rejected)"
}

# Any clang emits the IR: the gate reads text, so no reader version applies.
resolve_any_clang() {
  local c
  for c in "${CLANG18:-}" "$("$ROOT/tools/gen-runtime-bc.sh" --clang 2>/dev/null || true)" clang; do
    [ -n "$c" ] || continue
    c="$(command -v "$c" || true)"
    [ -n "$c" ] && "$c" --version 2>/dev/null | grep -qi clang && { echo "$c"; return 0; }
  done
  return 1
}

owner_gate() {
  local clang opt
  clang="$(resolve_any_clang || true)"
  if [ -z "$clang" ]; then
    echo "tls-hoist-gate: WARNING — no clang found; the runtime owner is NOT gated for thread-locals." >&2
    return 0
  fi
  opt="$(sed -n 's/^pub const OWNER_OPT : String = "\(.*\)"$/\1/p' "$ROOT/tools/kai/cli_plan.kai")"
  [ -n "$opt" ] || { echo "tls-hoist-gate: OWNER_OPT not found in tools/kai/cli_plan.kai" >&2; return 2; }
  local common=(-std=c99 -w "$opt" -S -emit-llvm -DKAI_SEPARATE_COMPILATION=1 -DKAI_RUNTIME_OWNER=1
                -I "$ROOT/stage2" -I "$ROOT/stage0")
  "$clang" "${common[@]}" -DKAI_PROGRAM_PROVIDES_MAIN=1 "$ROOT/stage2/runtime_owner_c.c" -o "$WORK/owner-c.ll" \
    || { echo "tls-hoist-gate: compiling the C owner failed" >&2; return 2; }
  "$clang" "${common[@]}" "$ROOT/stage0/runtime_llvm.c" -o "$WORK/owner-native.ll" \
    || { echo "tls-hoist-gate: compiling the native owner failed" >&2; return 2; }
  owner_check "$OWNER_ALLOW" "$WORK/owner-c.ll" "$WORK/owner-native.ll"
  owner_exports_check
}

ext_defs() {
  awk '/^define / && !/ (internal|private|available_externally|linkonce|linkonce_odr|weak|weak_odr) / {
    if (match($0, /@[A-Za-z0-9_.$]+\(/)) print substr($0, RSTART + 1, RLENGTH - 2)
  }' "$1" | sort -u
}

# A native partition keeps the hot bitcode's external bodies only as
# available_externally, so a call left out-of-line links against the owner:
# every one of them must be an owner export.
owner_exports_check() {
  local bc="$ROOT/stage0/runtime_inline.bc" dis missing
  [ -f "$bc" ] || return 0
  dis="$(resolve_llvm_dis || true)"
  [ -n "$dis" ] || { echo "tls-hoist-gate: WARNING — llvm-dis not found; owner exports NOT checked." >&2; return 0; }
  "$dis" "$bc" -o "$WORK/inline.ll"
  missing="$(comm -23 <(ext_defs "$WORK/inline.ll") <(ext_defs "$WORK/owner-native.ll"))"
  if [ -n "$missing" ]; then
    echo "tls-hoist-gate: the hot bitcode defines external functions the native owner does not export:" >&2
    echo "$missing" | sed 's/^/  /' >&2
    return 1
  fi
}

if [ "$MODE" = "--self-test" ]; then self_test && owner_self_test; exit; fi
if [ "$MODE" = "--owner" ]; then owner_gate; exit; fi

present=$(ls "${BITCODE[@]}" 2>/dev/null || true)
if [ -z "$present" ]; then
  # P2 is optional by design: no matching clang means no hot bitcode, which means the
  # runtime never inlines into a fiber frame and there is nothing to gate.
  echo "tls-hoist-gate: no hot bitcode (P2 opted out) — nothing to gate."
  exit 0
fi

DIS="$(resolve_llvm_dis || true)"
if [ -z "$DIS" ]; then
  echo "tls-hoist-gate: WARNING — no llvm-dis matching the bitcode's clang; the hot bitcode is NOT gated for thread-local hoists." >&2
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
