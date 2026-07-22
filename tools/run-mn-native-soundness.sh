#!/usr/bin/env bash
# M:N native soundness gate (issue #1234). The work-stealing bug this closes
# is a NATIVE-BACKEND miscompile: clang -O2 caches the thread pointer across
# swapcontext, so a fiber work-stolen onto another OS thread reads the creator
# thread's _Thread_local scheduler state and a live fiber's stack is freed
# under it. It reproduces ONLY through `--backend=native` (the P2 runtime
# bitcode); the C backend and TSAN gates are sound and never saw it — which is
# how it shipped. So this gate is native-EXPLICIT and loops enough times to
# catch an intermittent (~10%) crash reliably: a sound runtime is 0 crashes
# EVERY time, a regressed one crashes within a few dozen runs.
#
# Needs a libLLVM kaic2 (KAI_LLVM=1). On a C-only build `kai build
# --backend=native` errors, so the gate self-skips rather than false-fail.
#
#   tools/run-mn-native-soundness.sh [iterations]   # default 40

set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAI="$ROOT/bin/kai"
ITERS="${1:-40}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

. "$ROOT/tools/lib/timeout.sh"

FIXTURES=(
  "examples/effects/mn_cross_thread_copy_stress.kai"
)

# Probe native availability once; skip cleanly on a C-only compiler.
probe="$TMP/probe"
if ! "$KAI" build --backend=native "${FIXTURES[0]}" -o "$probe" >"$TMP/probe.log" 2>&1; then
  if grep -q "not built into this\|native backend is not built" "$TMP/probe.log"; then
    echo "run-mn-native-soundness: SKIP — native backend unavailable (C-only kaic2)."
    exit 0
  fi
  echo "run-mn-native-soundness: BUILD FAILED ${FIXTURES[0]}"; cat "$TMP/probe.log"; exit 1
fi

fail=0
for src in "${FIXTURES[@]}"; do
  name="$(basename "$(dirname "$src")")/$(basename "$src")"
  bin="$TMP/$(echo "$name" | tr '/' '_')"
  "$KAI" build --backend=native "$src" -o "$bin" >/dev/null 2>"$TMP/b.log" \
    || { echo "BUILD FAILED $name"; cat "$TMP/b.log"; exit 1; }

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
      echo "OK  $name  KAI_THREADS=$n  $ITERS/$ITERS clean"
    else
      echo "FAIL $name KAI_THREADS=$n: $crashes crash, $hangs hang, $bad bad-output of $ITERS (ref='$ref')"
    fi
  done
  [ "$ok" = "1" ] || fail=1
done

# Line-atomicity hammer: eight fibers race Stdout.print payload+newline
# pairs across scheduler threads. Cross-fiber ORDER is unspecified, so the
# judge is the sorted line multiset against the N=1 reference — a torn line
# (fused payload + stray empty line) breaks it. Stdout goes to a file.
HAMMER="examples/effects/mn_print_line_atomic_hammer.kai"
hbin="$TMP/hammer"
"$KAI" build --backend=native "$HAMMER" -o "$hbin" >/dev/null 2>"$TMP/hb.log" \
  || { echo "BUILD FAILED $HAMMER"; cat "$TMP/hb.log"; exit 1; }
KAI_THREADS=1 "$hbin" >"$TMP/href" 2>/dev/null \
  || { echo "FAIL $HAMMER: reference run at N=1 did not complete"; exit 1; }
sort "$TMP/href" >"$TMP/href.sorted"
hok=1
for n in 4 8; do
  crashes=0; bad=0; hangs=0
  for r in $(seq 1 "$ITERS"); do
    if kai_timeout 30 env KAI_THREADS="$n" "$hbin" >"$TMP/hrun" 2>/dev/null; then
      sort "$TMP/hrun" | cmp -s - "$TMP/href.sorted" || { bad=$((bad+1)); hok=0; }
    else
      ec=$?
      if [ "$ec" = 124 ] || [ "$ec" = 137 ]; then hangs=$((hangs+1)); else crashes=$((crashes+1)); fi
      hok=0
    fi
  done
  if [ "$crashes" = 0 ] && [ "$bad" = 0 ] && [ "$hangs" = 0 ]; then
    echo "OK  $HAMMER  KAI_THREADS=$n  $ITERS/$ITERS intact-line runs"
  else
    echo "FAIL $HAMMER KAI_THREADS=$n: $crashes crash, $hangs hang, $bad torn-output of $ITERS"
  fi
done
[ "$hok" = 1 ] || fail=1

[ "$fail" = "0" ] && echo "run-mn-native-soundness: OK" || { echo "run-mn-native-soundness: FAIL"; exit 1; }
