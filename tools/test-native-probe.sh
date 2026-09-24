#!/usr/bin/env bash
# The native probe accepts a kaic2 that emits an object, rejects one that
# crashes or emits nothing, and the KAI_LLVM=1 link removes a kaic2 it
# rejects. Hermetic, no kaic2 dependency, milliseconds.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROBE="$ROOT/tools/native-probe.sh"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

fake() { printf '#!/bin/sh\n%s\n' "$2" > "$T/$1"; chmod +x "$T/$1"; }
fake emits 'touch probe.o'
fake crashes 'kill -SEGV $$'
fake silent 'exit 0'

fail=0
"$PROBE" "$T/emits" 2> /dev/null \
  || { echo "native-probe FAIL — rejected a kaic2 that emits an object"; fail=1; }
for k in crashes silent; do
  if "$PROBE" "$T/$k" 2> /dev/null; then
    echo "native-probe FAIL — accepted a kaic2 that $k"; fail=1
  fi
done
"$PROBE" "$T/crashes" 2>&1 | grep -q 'exit 139' \
  || { echo "native-probe FAIL — a crash is not reported with its exit status"; fail=1; }
grep -qF 'native-probe.sh $@ || { rm -f $@ build/$@.id;' "$ROOT/stage2/Makefile" \
  || { echo "native-probe FAIL — the KAI_LLVM=1 link no longer removes a kaic2 the probe rejects"; fail=1; }

[ "$fail" -eq 0 ] || exit 1
echo "native-probe OK — a kaic2 that cannot emit native is rejected and not left installed"
