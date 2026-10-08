#!/usr/bin/env bash
# M:N C-backend soundness gate under clang. clang -O1+ treats a thread-local's
# address as constant for a whole function activation and may keep it across
# swapcontext, so a function that holds one across a park reads the wrong
# thread's state once a work-stealer resumes its fiber elsewhere. The runtime
# reaches scheduler state through kai_worker_here and the running fiber, never
# through a thread-local held across a switch; this gate exercises that under
# an optimised clang build of every shape the C backend links.
#
# It builds the cross-thread stress fixture on the C backend under clang (the
# two `bin/kai` link shapes, each with the -O2 runtime owner, plus the raw
# single-TU -O2 one) and loops it at KAI_THREADS=4/8. A function that keeps a
# thread-local across a park crashes within a few dozen runs.
#
# Both observation conditions matter and neither is incidental: stdout is
# redirected to a FILE (command substitution is a pipe, and pipe buffering
# closes the race window outright), and the single-TU -O2 arm compiles the
# scheduler into the program's own frames, the path the stage2 Makefile
# recipes take.
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
. "$ROOT/tools/lib/single-tu.sh"

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

# Build arms. The first two go through `bin/kai`, which routes the scheduler
# into the runtime owner object; the third compiles the emitted C as one TU at
# -O2 with no owner, which is what the stage2 Makefile recipes do.
build_kai_single_tu() { CC="$CLANG" "$KAI" build --backend=c "$FIXTURE" -o "$1"; }
build_kai_modular()   { CC="$CLANG" KAI_MODULAR=1 "$KAI" build --backend=c "$FIXTURE" -o "$1"; }
build_raw_single_tu() { CC="$CLANG" kai_build_single_tu "$FIXTURE" "$1"; }

run_one() {
  label="$1"; builder="$2"
  bin="$TMP/mnc_$label"
  if ! "$builder" "$bin" >"$TMP/b.log" 2>&1; then
    echo "FAIL $label: build failed"; cat "$TMP/b.log"; fail=1; return
  fi
  # Every run writes stdout to a file: through a pipe the races this gate
  # exists for do not reproduce at all.
  ref="$TMP/ref.out"
  KAI_THREADS=1 "$bin" >"$ref" 2>/dev/null
  ok=1
  for n in 4 8; do
    crashes=0; bad=0; hangs=0
    for r in $(seq 1 "$ITERS"); do
      if kai_timeout 30 env KAI_THREADS="$n" "$bin" >"$TMP/run.out" 2>/dev/null; then
        cmp -s "$TMP/run.out" "$ref" || { bad=$((bad+1)); ok=0; }
      else
        ec=$?
        if [ "$ec" = 124 ] || [ "$ec" = 137 ]; then hangs=$((hangs+1)); else crashes=$((crashes+1)); fi
        ok=0
      fi
    done
    if [ "$crashes" = 0 ] && [ "$bad" = 0 ] && [ "$hangs" = 0 ]; then
      echo "OK  $label  KAI_THREADS=$n  $ITERS/$ITERS clean"
    else
      echo "FAIL $label KAI_THREADS=$n: $crashes crash, $hangs hang, $bad bad-output of $ITERS (ref='$(head -c 200 "$ref" | tr '\n' '|')')"
    fi
  done
  [ "$ok" = "1" ] || fail=1
}

# Both `bin/kai` C link shapes — single-TU program object (#1238) and the
# modular one (#748) — plus the ownerless single-TU build.
run_one "kai-single-tu" build_kai_single_tu
run_one "kai-modular" build_kai_modular
run_one "raw-single-tu-O2" build_raw_single_tu

[ "$fail" = "0" ] && echo "run-mn-c-clang-soundness: OK" || { echo "run-mn-c-clang-soundness: FAIL"; exit 1; }
