#!/bin/sh
# L1 driver smoke: kai build --backend=llvm.
#
# For every fixture in $FIXTURES, build with both backends through
# bin/kai (no Makefile) and diff their stdout. The kaic2-side LLVM
# parity sweep already lives in stage2/Makefile (test-llvm and
# test-llvm-coverage); this script exists to gate the *driver*
# wiring — that --backend=llvm reaches kaic2's --emit=llvm path,
# finds runtime_llvm.c, finds clang, and produces a working binary.
#
# KAI_NO_STDLIB=1 is required because the LLVM emitter still has
# gaps under the full stdlib prelude (Mutable.kai, etc.); see
# docs/lane-experience-l1-llvm-driver.md for the L2 scope that
# closes those gaps. The C backend is invoked the same way for an
# apples-to-apples comparison.

set -eu

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAI="$ROOT/bin/kai"

if ! command -v "${CLANG:-clang}" >/dev/null 2>&1; then
  echo "test-llvm-driver: SKIP (clang not in PATH)"
  exit 0
fi

FIXTURES="examples/minimal/hello.kai
examples/llvm/driver_smoke.kai
examples/llvm/m3b.kai"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

pass=0
fail=0

# bin/kai build --help should advertise the flag.
if "$KAI" build --help 2>&1 | grep -q -- '--backend=<c|llvm>'; then
  echo "help OK --backend documented"
  pass=$((pass + 1))
else
  echo "help FAIL --backend not in 'kai build --help' output"
  fail=$((fail + 1))
fi

# Bad backend value must fail with a clear message.
if KAI_NO_STDLIB=1 "$KAI" build --backend=bogus examples/minimal/hello.kai -o "$tmp/x" 2>"$tmp/bogus.err"; then
  echo "validation FAIL --backend=bogus accepted"
  fail=$((fail + 1))
else
  if grep -q "must be 'c' or 'llvm'" "$tmp/bogus.err"; then
    echo "validation OK --backend=bogus rejected"
    pass=$((pass + 1))
  else
    echo "validation FAIL --backend=bogus error message did not match"
    cat "$tmp/bogus.err"
    fail=$((fail + 1))
  fi
fi

# Missing-clang detection should fire and exit non-zero.
if CLANG=__nope_clang_does_not_exist__ \
   KAI_NO_STDLIB=1 "$KAI" build --backend=llvm examples/minimal/hello.kai -o "$tmp/x" 2>"$tmp/noclang.err"; then
  echo "clang-detect FAIL missing clang accepted"
  fail=$((fail + 1))
else
  if grep -q "requires clang in PATH" "$tmp/noclang.err"; then
    echo "clang-detect OK missing clang rejected"
    pass=$((pass + 1))
  else
    echo "clang-detect FAIL missing-clang message did not match"
    cat "$tmp/noclang.err"
    fail=$((fail + 1))
  fi
fi

for src in $FIXTURES; do
  name="$(basename "$src" .kai)"
  c_bin="$tmp/$name-c"
  llvm_bin="$tmp/$name-llvm"
  c_out="$tmp/$name-c.out"
  llvm_out="$tmp/$name-llvm.out"

  if ! KAI_NO_STDLIB=1 "$KAI" build "$src" -o "$c_bin" >"$tmp/$name-c.log" 2>&1; then
    echo "parity FAIL $name (C build)"
    cat "$tmp/$name-c.log"
    fail=$((fail + 1))
    continue
  fi
  if ! KAI_NO_STDLIB=1 "$KAI" build --backend=llvm "$src" -o "$llvm_bin" >"$tmp/$name-llvm.log" 2>&1; then
    echo "parity FAIL $name (LLVM build)"
    cat "$tmp/$name-llvm.log"
    fail=$((fail + 1))
    continue
  fi
  "$c_bin"    >"$c_out"    2>&1 || true
  "$llvm_bin" >"$llvm_out" 2>&1 || true
  if diff -q "$c_out" "$llvm_out" >/dev/null; then
    echo "parity OK $name"
    pass=$((pass + 1))
  else
    echo "parity FAIL $name"
    diff "$c_out" "$llvm_out" | head
    fail=$((fail + 1))
  fi
done

echo "---"
echo "test-llvm-driver: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
