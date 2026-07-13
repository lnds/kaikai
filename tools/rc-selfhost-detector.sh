#!/bin/bash
# RC self-host detector — exhaustive UAF/double-free hunt over both backends.
#
# The RC bugs that block native self-host are SILENT under the normal build
# for two reasons this detector neutralises:
#
#   1. The C runtime's cell pool recycles a freed cell into the next
#      same-size alloc, so a double-free hands back a live-looking cell
#      instead of tripping the allocator. Building with -DKAI_NO_CELL_POOL
#      turns the pool off, so a freed cell goes to the real allocator and
#      AddressSanitizer's redzone catches the second free / stale read.
#   2. Differential "byte-id against the C oracle" is blind to bugs both
#      backends inherit from the shared front end: an over-decref the
#      perceus/emit pair plants for BOTH backends passes a diff that
#      compares them to each other. So this checks an ABSOLUTE invariant
#      (no cell is freed more times than it was allocated), not a
#      differential one, and runs it on the C backend AND the native
#      backend — the C oracle is not assumed clean.
#
# Per fixture, per backend, three checks:
#   - ASAN + no-cell-pool: any AddressSanitizer / UBSan diagnostic = FAIL
#     (a double-free or use-after-free freed a still-live cell).
#   - functional diff against the .out.expected golden: a corrupted value
#     that does not crash still changes the output.
#   - strict RC ledger (-DKAI_TRACE_RC=1, pool off, poison-on-free): a
#     per-tag `frees > allocs` prints a DOUBLE flag = FAIL. Per-tag LEAK
#     (a cell still live at exit) is expected — the process exits without
#     a final free walk — and is NOT a failure.
#
# The native ASAN build must instrument the runtime, so it forces the
# legacy link path (cc compiles runtime_llvm.c) by hiding the runtime
# bitcode for the duration; the P2 self-contained object links only obj +
# libm and leaves the runtime uninstrumented.
#
# Native-only for the native half: SKIPs it where libLLVM is absent. The
# C half always runs. Exits non-zero on any detected UAF/double-free.

set -u

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

FIXDIR="$ROOT/examples/perceus"
CORPUS="$ROOT/tools/rc-detector-corpus.txt"
WORK="$ROOT/stage2/build/rc-detector"
mkdir -p "$WORK"

KAI="$ROOT/bin/kai"
CC="${CC:-cc}"
LLVM_CONFIG="${LLVM_CONFIG:-llvm-config}"

ASAN_CFLAGS="-std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -DKAI_NO_CELL_POOL -Wno-unused-function -Wno-unused-variable"
STRICT_CFLAGS="-std=c99 -O1 -g -DKAI_TRACE_RC=1 -Wno-unused-function -Wno-unused-variable"
export ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"

have_native=0
if command -v "$LLVM_CONFIG" >/dev/null 2>&1; then have_native=1; fi

# Native ASAN needs the legacy link path (runtime_llvm.c compiled by cc so
# ASAN instruments it). Hide BOTH bitcodes for the whole run — the P2
# whole-program bc AND the sep-comp inline bc (the native-modular default merges
# it per partition); hiding both forces the runtime to be the cc-compiled owner
# TU, which ASAN instruments. Restore on any exit so a failing fixture never
# leaves the tree stripped.
BC="$ROOT/stage0/runtime_llvm.bc"
BC_HIDDEN="$WORK/runtime_llvm.bc.hidden"
BC_INLINE="$ROOT/stage0/runtime_inline.bc"
BC_INLINE_HIDDEN="$WORK/runtime_inline.bc.hidden"
restore_bc() {
  [ -f "$BC_HIDDEN" ] && mv "$BC_HIDDEN" "$BC" 2>/dev/null || true
  [ -f "$BC_INLINE_HIDDEN" ] && mv "$BC_INLINE_HIDDEN" "$BC_INLINE" 2>/dev/null || true
}
trap restore_bc EXIT INT TERM

fixtures="$(grep -vE '^[[:space:]]*(#|$)' "$CORPUS")"
[ -n "$fixtures" ] || { echo "rc-detector FAIL — empty corpus $CORPUS"; exit 1; }

fail=0
sanitizer_hit() { grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' "$1"; }

# One backend × one fixture: ASAN build+run (sanitizer + golden diff) and
# strict build+run (DOUBLE flag). $1=backend(c|native) $2=fixture name.
check_one() {
  local backend="$1" name="$2"
  local src="$FIXDIR/$name.kai" exp="$FIXDIR/$name.out.expected"
  local tag="$backend/$name"
  local abin="$WORK/$name-$backend-asan" sbin="$WORK/$name-$backend-strict"

  # ASAN + no-cell-pool.
  if ! CFLAGS="$ASAN_CFLAGS" "$KAI" build --backend="$backend" "$src" -o "$abin" 2>"$WORK/$name-$backend.build"; then
    echo "  FAIL $tag — ASAN build errored:"; sed 's/^/      /' "$WORK/$name-$backend.build"; fail=1; return
  fi
  "$abin" >"$WORK/$name-$backend-asan.out" 2>"$WORK/$name-$backend-asan.err"
  if sanitizer_hit "$WORK/$name-$backend-asan.err"; then
    echo "  FAIL $tag — sanitizer diagnostic (double-free / use-after-free):"
    grep -m3 -E 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' "$WORK/$name-$backend-asan.err" | sed 's/^/      /'
    fail=1; return
  fi
  if ! diff -q "$exp" "$WORK/$name-$backend-asan.out" >/dev/null; then
    echo "  FAIL $tag — output diff under ASAN (silent corruption):"
    diff "$exp" "$WORK/$name-$backend-asan.out" | head -6 | sed 's/^/      /'
    fail=1; return
  fi

  # Strict RC ledger. Native builds a self-contained object; the strict
  # macros live in runtime.h (C link), so the strict pass runs on the C
  # backend only. Native's absolute check is the ASAN pass above.
  if [ "$backend" = "c" ]; then
    if ! CFLAGS="$STRICT_CFLAGS" "$KAI" build --backend=c "$src" -o "$sbin" 2>/dev/null; then
      echo "  FAIL $tag — strict build errored"; fail=1; return
    fi
    KAI_TRACE_RC=1 "$sbin" >/dev/null 2>"$WORK/$name-strict.err"
    if grep -q 'DOUBLE' "$WORK/$name-strict.err"; then
      echo "  FAIL $tag — strict ledger DOUBLE (a tag freed more than allocated):"
      grep 'DOUBLE' "$WORK/$name-strict.err" | sed 's/^/      /'
      fail=1; return
    fi
  fi
  echo "  OK   $tag"
}

echo "rc-detector: C backend (ASAN + no-cell-pool + strict ledger)"
for name in $fixtures; do
  [ -f "$FIXDIR/$name.kai" ] || { echo "  FAIL missing fixture $name"; fail=1; continue; }
  check_one c "$name"
done

if [ "$have_native" -eq 1 ]; then
  echo "rc-detector: native backend (ASAN + no-cell-pool, runtime instrumented)"
  [ -f "$BC" ] && mv "$BC" "$BC_HIDDEN"
  [ -f "$BC_INLINE" ] && mv "$BC_INLINE" "$BC_INLINE_HIDDEN"
  for name in $fixtures; do
    check_one native "$name"
  done
  restore_bc
else
  echo "rc-detector: native backend SKIP (llvm-config not in PATH)"
fi

if [ "$fail" -ne 0 ]; then
  echo "rc-detector: FAIL — RC invariant violated (see diagnostics above)."
  exit 1
fi
echo "rc-detector: PASS — no double-free / use-after-free across the corpus on either backend."
exit 0
