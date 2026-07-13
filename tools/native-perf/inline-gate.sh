#!/bin/sh
# native-perf/inline-gate.sh — assert the native backend inlines the runtime's
# HOT-PATH ops into the caller.
#
# The native emitter stamps every program function with the runtime bitcode's
# host target-cpu/features so `areInlineCompatible` lets the opt pipeline fold
# the `kaix_*` runtime ops into the caller. Without that stamp the inliner
# refuses EVERY runtime-op inline on a legality (not cost) basis, and the
# rb-tree hot loop pays a call per field read / re-box — the ~6x instruction /
# ~2.7x wall native-vs-C blow-up.
#
# WHAT WE COUNT, AND WHY NOT THE TOTAL. This counts only the residual HOT-PATH
# `kaix_*` calls (RC dup/drop + slot/field/variant reads) — the ops the rb-tree
# navigation loop runs per node. The raw TOTAL of `kaix_*` calls is NOT a
# backend-portable metric: under separate compilation (native-modular, the
# default) the runtime's singletons and lookup tables are `external` (they carry
# pointer identity across partitions), so the predicates that compare against
# them (`is_nil`/`is_cons`) cannot be constant-folded cross-TU and stay as
# calls — a legitimate structural floor that is NOT hot-path and does not move
# wall time. The whole-program total collapses those, the modular total does
# not; counting the total would flag a non-regression on the default backend.
# The hot-path count is ~0 on both backends when inlining works, so it measures
# the objective directly. A wall-time backstop below catches any inline success
# that nonetheless regresses (e.g. code-bloat from the merge).
#
# Native-only. SKIP where libLLVM is absent (no native backend to gate).
set -eu
_ZO_DOCTOR=0; export _ZO_DOCTOR

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="${KAI:-$ROOT/bin/kai}"
SRC="$ROOT/examples/perceus/rb_tree_bench.kai"
# The hot-path ops the navigation loop runs per node: RC dup/drop and the
# variant/field/slot reads. This whitelist is the observable of "runtime ops
# inlined into the hot loop"; it is the same class of op runtime_inline.bc is
# built to fold. A generous ceiling (a handful survive at loop edges); a broken
# stamp jumps every per-node op back to a call and blows far past it.
HOT_RE='kaix_(internal_dup|internal_drop|variant_arg|variant_arg_borrow|variant_masked|field_at|int_field|real_field|proj|projkind|tag_of)'
THRESHOLD="${INLINE_GATE_THRESHOLD:-50}"

LLVM_CONFIG="${LLVM_CONFIG:-llvm-config}"
if ! command -v "$LLVM_CONFIG" >/dev/null 2>&1; then
  echo "native-perf/inline-gate: SKIP (llvm-config not in PATH; no native backend)"
  exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
BIN="$TMP/rb_native"

# The default native path (native-modular) needs the sep-comp runtime bitcode to
# inline the hot ops; without it every partition falls back to runtime-owner
# calls and this gate fails on the un-inlined count. Fail LOUD and specific if
# the bc is missing (a build/artifact gap) rather than as a confusing high count.
if [ "${KAI_NATIVE_MODULAR:-1}" != "0" ]; then
  BC_INLINE=""
  for _c in "$ROOT/stage0/runtime_inline.bc" "$ROOT/share/kaikai/include/runtime_inline.bc"; do
    [ -f "$_c" ] && { BC_INLINE="$_c"; break; }
  done
  if [ -z "$BC_INLINE" ]; then
    echo "::error::native-perf/inline-gate FAIL — runtime_inline.bc missing;"
    echo "  the native-modular default cannot inline the runtime (run gen-runtime-bc.sh /"
    echo "  ship stage0/runtime_inline.bc in the build artifact)."
    exit 1
  fi
fi

"$KAI" build --backend=native "$SRC" -o "$BIN" >/dev/null 2>"$TMP/build.err" || {
  echo "native-perf/inline-gate FAIL — native build failed:"; cat "$TMP/build.err"; exit 1
}

# Count residual hot-path `bl <kaix_*>` call sites. macOS ships `otool`; Linux
# CI uses `objdump`. Both mangle the runtime symbols with a leading `_` on
# Mach-O and none on ELF, so match the hot-path names anywhere in the operand.
if command -v otool >/dev/null 2>&1; then
  DISASM="otool -tV"
elif command -v objdump >/dev/null 2>&1; then
  DISASM="objdump -d"
else
  echo "native-perf/inline-gate: SKIP (no otool/objdump to disassemble)"
  exit 0
fi
CALLS="$($DISASM "$BIN" | grep -cE "(bl|call).*$HOT_RE" || true)"

echo "native-perf/inline-gate: residual hot-path kaix_ calls = $CALLS (threshold $THRESHOLD)"
if [ "$CALLS" -gt "$THRESHOLD" ]; then
  echo "::error::native-perf/inline-gate FAIL — $CALLS hot-path kaix_ calls exceeds $THRESHOLD;"
  echo "  the emitter is no longer stamping host target-features (whole-program) or"
  echo "  runtime_inline.bc is not being merged (native-modular), so the inliner"
  echo "  cannot fold the per-node runtime ops (see kai_llvm_stamp_host_features /"
  echo "  kai_llvm_link_runtime_bc_modular)."
  exit 1
fi
echo "native-perf/inline-gate OK — hot-path runtime ops inline into the loop."

# Perf backstop: the default (native-modular) binary must run within a margin of
# the whole-program native build. Catches an inline that "succeeded" by the count
# but regressed via merge code-bloat. This is a SECONDARY, best-effort check —
# the hot-path count above is the primary gate. It skips cleanly where the timer
# is unavailable, and uses a generous 1.6x + 0.1s margin so shared CI runners'
# scheduling noise never yields a false red (a lost inline is a ~2x regression,
# far past this). Best of three runs each (min = least noise).
if command -v /usr/bin/time >/dev/null 2>&1; then
  KAI_NATIVE_MODULAR=0 "$KAI" build --backend=native "$SRC" -o "$TMP/rb_wp" >/dev/null 2>&1 || true
  if [ -x "$TMP/rb_wp" ]; then
    best() {
      _b=""
      for _i in 1 2 3; do
        _t="$( { /usr/bin/time -p "$1" >/dev/null; } 2>&1 | awk '/^real/{print $2}' )"
        [ -z "$_t" ] && continue
        { [ -z "$_b" ] || awk "BEGIN{exit !($_t < $_b)}" 2>/dev/null; } && _b="$_t"
      done
      echo "$_b"
    }
    MOD="$(best "$BIN")"; WP="$(best "$TMP/rb_wp")"
    if [ -n "$MOD" ] && [ -n "$WP" ]; then
      echo "native-perf/inline-gate: wall modular=${MOD}s whole-program=${WP}s"
      if awk "BEGIN{exit !($MOD > $WP * 1.6 + 0.1)}" 2>/dev/null; then
        echo "::error::native-perf/inline-gate FAIL — modular ${MOD}s regressed past whole-program ${WP}s * 1.6;"
        echo "  the hot-path inlined but wall time regressed (merge bloat / cache-miss path)."
        exit 1
      fi
      echo "native-perf/inline-gate OK — modular wall time within margin of whole-program."
    fi
  fi
fi

# Variant fast-path gate: a primitive-slot ctor (RBNode's Int keys) must carry
# a non-zero i64 mask in the KIR, so construction routes through the typed
# masked entry instead of the cold `kai_variant_u` (per-call name/mask register
# + immortal-args hash scan). The masked entry is alwaysinline'd into the hot
# loop, so the binary shows no call to count — the KIR text is the observable:
# a mask regression (ls_ctor_mask / the registration) prints `m0` here.
KAIC2="$ROOT/stage2/kaic2"
MASKED_CON="$("$KAIC2" --path "$ROOT/stdlib" --emit=kir "$SRC" 2>/dev/null \
  | grep -cE 'con RBNode #[0-9]+ m[1-9][0-9]*' || true)"
echo "native-perf/inline-gate: masked RBNode ctors in KIR = $MASKED_CON (expect > 0)"
if [ "$MASKED_CON" -eq 0 ]; then
  echo "::error::native-perf/inline-gate FAIL — RBNode constructs with mask 0;"
  echo "  primitive-slot ctors reverted to the cold all-pointer path (see"
  echo "  nemit_con / ls_ctor_mask / nproto_register_payload_ctors)."
  exit 1
fi
echo "native-perf/inline-gate OK — primitive-slot ctors carry the i64 mask."
