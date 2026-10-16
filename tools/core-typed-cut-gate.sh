#!/bin/bash
# Typed core cut gate. After monomorphisation the cut keeps a core impl
# only when something reached selects it. A queue and a stack operate on
# `Int` fields of a generic record, so a build of them must emit no
# `Complex` arithmetic impl and no structural `Eq` impl.

set -u
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAIC2="$ROOT/stage2/kaic2"
WORK="$ROOT/stage2/build/core-typed-cut"
EDITION="$(cat "$ROOT/EDITION")"
[ -x "$KAIC2" ] || { echo "core-typed-cut: SKIP — no stage2/kaic2"; exit 0; }
rm -rf "$WORK"; mkdir -p "$WORK"

SRC=examples/stdlib/core_cut_generic_ops.kai
"$KAIC2" --edition "$EDITION" --path "$ROOT/stdlib" "$SRC" > "$WORK/pruned.c" 2> "$WORK/pruned.err" \
  || { echo "core-typed-cut: FAIL — the pruned build does not compile"; cat "$WORK/pruned.err"; exit 1; }

fail=0
for impl in Sub_Complex Add_Complex Eq_List Eq_Option Eq_Pair Eq_Result; do
  if grep -q "__pimpl_${impl}_" "$WORK/pruned.c"; then
    echo "  FAIL — the build keeps __pimpl_${impl}_*"; fail=1
  fi
done
grep -q "__pimpl_Eq_Int_" "$WORK/pruned.c" && { echo "  FAIL — the build keeps __pimpl_Eq_Int_*"; fail=1; }

[ "$fail" -eq 0 ] || { echo "core-typed-cut: FAIL"; exit 1; }
echo "core-typed-cut OK"
