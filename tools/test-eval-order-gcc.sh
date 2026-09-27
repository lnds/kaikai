#!/bin/bash
# Evaluation-order gate: operand fixtures built with gcc on x86_64 must
# print their golden.
#
# C leaves the evaluation order of call arguments and initializer lists
# unspecified. clang evaluates them left to right, so the C backend's
# operand order is only exposed by a compiler that does not: gcc on
# x86_64 evaluates call arguments right to left. Under clang these
# fixtures pass whether or not the emitter sequences its operands, which
# is why this gate refuses to run anywhere else.

set -eu

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
GCC="${EVAL_ORDER_CC:-gcc}"
FIXTURES="examples/effects/eval_order_left_to_right.kai examples/effects/row_in_ordinary_type.kai"

arch="$(uname -m)"
cc_banner="$("$GCC" --version 2>/dev/null | head -1 || true)"
echo "eval-order-gcc: arch=$arch cc=$GCC ($cc_banner)"

if [ "$arch" != "x86_64" ]; then
  echo "::error::eval-order-gcc: runner is $arch, not x86_64 — gcc evaluates call arguments left to right here, so this gate cannot detect an unsequenced operand"
  exit 1
fi
if ! "$GCC" --version 2>/dev/null | grep -q 'Free Software Foundation'; then
  echo "::error::eval-order-gcc: '$GCC' is not GNU gcc — this gate needs gcc's right-to-left argument order to detect an unsequenced operand"
  exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

fail=0
for f in $FIXTURES; do
  golden="${f%.kai}.out.expected"
  name="$(basename "${f%.kai}")"
  if ! CC="$GCC" KAI_BACKEND=c "$ROOT/bin/kai" run "$f" > "$tmp/$name.out" 2> "$tmp/$name.err"; then
    echo "::error::eval-order-gcc: $f failed to build or run under $GCC"
    cat "$tmp/$name.err"
    fail=1
    continue
  fi
  if diff -u "$golden" "$tmp/$name.out" > "$tmp/$name.diff"; then
    echo "eval-order-gcc: PASS $f"
  else
    echo "::error::eval-order-gcc: $f prints its operands out of order under $GCC — the C emitter left a multi-operand form unsequenced (golden is left to right, as the native backend evaluates)"
    cat "$tmp/$name.diff"
    fail=1
  fi
done

exit "$fail"
