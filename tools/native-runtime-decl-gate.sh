#!/usr/bin/env bash
# Every `kaix_*` runtime entry the native emitter declares carries the
# prototype `stage0/runtime_llvm.c` defines it with.
#
# A declaration that disagrees with its definition (a `void` entry declared
# returning `ptr`, a forwarder declared with no params) is undefined
# behaviour at the IR/C boundary that x86-64 and arm64 happen to tolerate; a
# linker that checks signatures turns every such call into a trap. clang
# compiles the whole runtime to IR for the reference signatures, and each
# corpus program's emitted IR is compared declaration by declaration:
# return type and parameter types, attributes and value names stripped.
#
# Usage: tools/native-runtime-decl-gate.sh [program.kai ...]
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/stage2/build/rt-decl}
CLANG=${CLANG:-}
if [ -z "$CLANG" ]; then
  for c in clang clang-18 clang-19 clang-20; do
    if command -v "$c" >/dev/null 2>&1; then CLANG=$c; break; fi
  done
fi
[ -n "$CLANG" ] || { echo "native-runtime-decl-gate FAIL: no clang on PATH"; exit 1; }

if [ "$#" -eq 0 ]; then
  set -- "$ROOT/examples/native/trmc_list.kai" \
         "$ROOT/examples/effects/handle_alias.kai" \
         "$ROOT/examples/effects/cancel_discard_minimal.kai" \
         "$ROOT/examples/effects/dead_perform_no_default_block.kai" \
         "$ROOT/examples/effects/default_through_call_under_other_handler.kai"
fi

rm -rf "$OUT"; mkdir -p "$OUT"

# `name ret(params)` for each kaix_ define or declare.
SIG_AWK='
/^(define|declare) / && /@kaix_[A-Za-z0-9_]*\(/ {
  line = $0
  sub(/ *#[0-9]+.*$/, "", line); sub(/ *\{ *$/, "", line)
  name = line; sub(/^.*@/, "", name); sub(/\(.*$/, "", name)
  head = line; sub(/@kaix_.*$/, "", head); sub(/^(define|declare) /, "", head)
  gsub(/(dso_local|internal|local_unnamed_addr|noundef|zeroext|signext|nonnull|noalias|hidden|range\([^)]*\)) */, "", head)
  gsub(/ +$/, "", head); n = split(head, w, " "); ret = w[n]
  args = line; sub(/^[^(]*\(/, "", args); sub(/\)[^)]*$/, "", args)
  np = split(args, ps, ","); out = ""
  for (i = 1; i <= np; i++) {
    p = ps[i]
    gsub(/(noundef|zeroext|signext|nonnull|noalias|nocapture|readonly|writeonly|captures\([^)]*\)|align [0-9]+|dereferenceable(_or_null)?\([0-9]+\)|%[A-Za-z0-9_.]+) */, "", p)
    gsub(/^ +| +$/, "", p)
    if (p != "") out = out (out == "" ? "" : ",") p
  }
  print name, ret "(" out ")"
}'

"$CLANG" -std=c99 -w -O0 -S -emit-llvm -DKAI_SEPARATE_COMPILATION=1 \
  -I "$ROOT/stage2" -I "$ROOT/stage0" "$ROOT/stage0/runtime_llvm.c" -o "$OUT/runtime.ll"
awk "$SIG_AWK" "$OUT/runtime.ll" | sort -u > "$OUT/runtime.sig"

i=0
for prog in "$@"; do
  i=$((i + 1))
  KAI_NATIVE_DUMP_IR="$OUT/p$i.ll" KAI_NATIVE_MODULAR=0 KAI_NATIVE_CORE_OBJ=0 KAI_BACKEND=native \
    "$ROOT/bin/kai" build "$prog" -o "$OUT/p$i" > "$OUT/p$i.err" 2>&1 \
    || { echo "native-runtime-decl-gate FAIL: $prog does not build"; cat "$OUT/p$i.err"; exit 1; }
  awk "$SIG_AWK" "$OUT/p$i.ll"
done | sort -u > "$OUT/emitted.sig"

[ -s "$OUT/emitted.sig" ] || { echo "native-runtime-decl-gate FAIL: no kaix_ declarations emitted"; exit 1; }

join "$OUT/emitted.sig" "$OUT/runtime.sig" | awk '$2 != $3 { print "  " $1 ": declared " $2 ", defined " $3 }' > "$OUT/mismatch.txt"
if [ -s "$OUT/mismatch.txt" ]; then
  echo "native-runtime-decl-gate FAIL: declarations disagree with stage0/runtime_llvm.c"
  cat "$OUT/mismatch.txt"
  exit 1
fi
echo "native-runtime-decl-gate OK ($(join "$OUT/emitted.sig" "$OUT/runtime.sig" | wc -l | tr -d ' ') kaix_ declarations over $# programs match their definitions)"
