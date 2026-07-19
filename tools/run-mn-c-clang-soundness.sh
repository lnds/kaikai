#!/usr/bin/env bash
# M:N C-backend soundness gate under clang (issue #1238, the C residual of
# #1234). The work-stealing miscompile is a clang codegen bug: clang -O1+ caches
# the thread pointer across swapcontext, so a fiber work-stolen onto another OS
# thread reads the creator thread's _Thread_local scheduler state. The NATIVE
# path (P2 bitcode) was closed by #1234; the C backend hits the SAME bug when
# its runtime is compiled by clang -O2 — which the native gate never exercises
# (it runs --backend=native only, and the default cc on CI/Docker is gcc, sound).
#
# This gate builds the cross-thread stress fixture on the C backend under clang
# (both the single-TU default and KAI_MODULAR=1) and loops it at KAI_THREADS=4/8.
# The fix compiles the scheduler as a separate -O0 owner object; a regression
# (owner inlined at -O2, or a new parking op missing its KAI_SCHED_FN gate)
# crashes within a few dozen runs.
#
# Needs a clang. Resolves one in PATH order; SKIPs cleanly if none is found (a
# gcc-only host cannot reproduce the bug and has nothing to gate).
#
#   tools/run-mn-c-clang-soundness.sh [iterations]   # default 40

set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAI="$ROOT/bin/kai"
ITERS="${1:-40}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

. "$ROOT/tools/lib/timeout.sh"

FIXTURE="examples/effects/mn_cross_thread_copy_stress.kai"

# Resolve a clang: an explicit override, then the common versioned names, then
# a plain `clang` only if it really is clang (macOS aliases gcc->clang, so a
# bare `clang` is fine; a bare `gcc` might be clang too but we do not rely on it).
resolve_clang() {
  if [ -n "${MN_C_CLANG:-}" ]; then command -v "$MN_C_CLANG" && return 0 || return 1; fi
  for c in clang-18 clang-14 /opt/homebrew/opt/llvm@18/bin/clang /usr/lib/llvm-18/bin/clang clang; do
    if command -v "$c" >/dev/null 2>&1 && "$c" --version 2>/dev/null | grep -qi clang; then
      command -v "$c"; return 0
    fi
  done
  return 1
}

CLANG="$(resolve_clang || true)"
if [ -z "$CLANG" ]; then
  echo "run-mn-c-clang-soundness: SKIP — no clang found (gcc-only host cannot reproduce #1238)."
  exit 0
fi
echo "run-mn-c-clang-soundness: using CC=$CLANG"

fail=0
run_one() {
  label="$1"; shift
  bin="$TMP/mnc_$label"
  if ! CC="$CLANG" "$@" "$KAI" build --backend=c "$FIXTURE" -o "$bin" >"$TMP/b.log" 2>&1; then
    echo "FAIL $label: build failed"; cat "$TMP/b.log"; fail=1; return
  fi
  ref="$(KAI_THREADS=1 "$bin")"
  ok=1
  for n in 4 8; do
    crashes=0; bad=0; hangs=0
    for r in $(seq 1 "$ITERS"); do
      if got="$(kai_timeout 30 env KAI_THREADS="$n" "$bin" 2>/dev/null)"; then
        [ "$got" = "$ref" ] || { bad=$((bad+1)); ok=0; }
      else
        ec=$?
        if [ "$ec" = 124 ] || [ "$ec" = 137 ]; then hangs=$((hangs+1)); else crashes=$((crashes+1)); fi
        ok=0
      fi
    done
    if [ "$crashes" = 0 ] && [ "$bad" = 0 ] && [ "$hangs" = 0 ]; then
      echo "OK  $label  KAI_THREADS=$n  $ITERS/$ITERS clean"
    else
      echo "FAIL $label KAI_THREADS=$n: $crashes crash, $hangs hang, $bad bad-output of $ITERS (ref='$ref')"
    fi
  done
  [ "$ok" = "1" ] || fail=1
}

# Single-TU default C path AND the modular C path (#748) — both compile the
# scheduler, both must route it to the -O0 owner.
run_one "single-tu" env
run_one "modular" env KAI_MODULAR=1

[ "$fail" = "0" ] && echo "run-mn-c-clang-soundness: OK" || { echo "run-mn-c-clang-soundness: FAIL"; exit 1; }
