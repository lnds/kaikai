#!/bin/sh
# Native core-object split vs. a missing inline runtime bitcode, kaic2
# invoked directly. With the whole-program bitcode available but its
# separate-compilation twin absent, the split would emit every kaix_* op
# as an out-of-line call (an order of magnitude on hot loops) with no
# signal. kaic2 must instead disengage the split — one whole-program
# object, runtime ops inlined — and print one kai:-prefixed stderr note.
# The control run (both bitcodes) proves the split still engages, so the
# degraded-run assertions cannot pass vacuously.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
BC="$ROOT/stage0/runtime_llvm.bc"
INL="$ROOT/stage0/runtime_inline.bc"

if [ ! -x "$KAIC2" ]; then
  echo "ncoreobj_inline_bc_fallback SKIP — no stage2/kaic2 binary"
  exit 0
fi

PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

printf 'fn main() : Unit = ()\n' > "$PROJ/probe.kai"
"$KAIC2" --emit=native --path "$ROOT/stdlib" --path "$PROJ" "$PROJ/probe.kai" \
  >/dev/null 2>"$PROJ/probe.err" || true
if grep -q "not built into this compiler" "$PROJ/probe.err" 2>/dev/null; then
  echo "ncoreobj_inline_bc_fallback SKIP — kaic2 has no libLLVM native backend"
  exit 0
fi
if [ ! -f "$BC" ] || [ ! -f "$INL" ]; then
  echo "ncoreobj_inline_bc_fallback SKIP — runtime bitcodes not generated"
  exit 0
fi

printf 'fn main() : Unit / Console = println("inline-bc hello")\n' > "$PROJ/main.kai"
ccdir="$PROJ/core-cache"
NOTE="native runtime inline bitcode unavailable"

emit() {
  KAI_NATIVE_CORE_OBJ=1 KAI_NATIVE_OPT=2 \
  KAI_NATIVE_RUNTIME_BC="$BC" KAI_NATIVE_RUNTIME_INLINE_BC="$1" \
    "$KAIC2" --emit=native --core-cache-dir "$ccdir" --toolchain-id nib-test \
    --core-cache-stats --path "$ROOT/stdlib" --path "$PROJ" "$PROJ/main.kai"
}

# Control: both bitcodes present — the split engages (two objects, a
# cached core object minted, no fallback note).
rm -rf "$ccdir"; mkdir -p "$ccdir"
emit "$INL" > "$PROJ/ctrl.objs" 2> "$PROJ/ctrl.err" \
  || { echo "ncoreobj_inline_bc_fallback FAIL — control emit failed"; cat "$PROJ/ctrl.err"; exit 1; }
[ "$(wc -l < "$PROJ/ctrl.objs" | tr -d ' ')" = "2" ] \
  || { echo "ncoreobj_inline_bc_fallback FAIL — control did not split (want 2 object paths)"; cat "$PROJ/ctrl.objs" "$PROJ/ctrl.err"; exit 1; }
[ "$(ls "$ccdir"/ncore-*.o 2>/dev/null | wc -l | tr -d ' ')" = "1" ] \
  || { echo "ncoreobj_inline_bc_fallback FAIL — control minted no cached core object"; exit 1; }
if grep -q "$NOTE" "$PROJ/ctrl.err"; then
  echo "ncoreobj_inline_bc_fallback FAIL — control printed the fallback note"
  cat "$PROJ/ctrl.err"; exit 1
fi

# Degraded: inline bitcode absent — the split must disengage: one
# whole-program object, no cached core object, the note on stderr, and
# no core-obj stats line (the split was never consulted).
rm -rf "$ccdir"; mkdir -p "$ccdir"; rm -f "$PROJ/main.o"
emit "" > "$PROJ/deg.objs" 2> "$PROJ/deg.err" \
  || { echo "ncoreobj_inline_bc_fallback FAIL — degraded emit failed"; cat "$PROJ/deg.err"; exit 1; }
grep -q "$NOTE" "$PROJ/deg.err" \
  || { echo "ncoreobj_inline_bc_fallback FAIL — no kai: fallback note on stderr"; cat "$PROJ/deg.err"; exit 1; }
[ "$(wc -l < "$PROJ/deg.objs" | tr -d ' ')" = "1" ] \
  || { echo "ncoreobj_inline_bc_fallback FAIL — degraded build did not take the whole-program path (want 1 object path)"; cat "$PROJ/deg.objs"; exit 1; }
[ "$(ls "$ccdir"/ncore-*.o 2>/dev/null | wc -l | tr -d ' ')" = "0" ] \
  || { echo "ncoreobj_inline_bc_fallback FAIL — degraded build minted a core object"; ls "$ccdir"; exit 1; }
if grep -q "native-core-obj:" "$PROJ/deg.err"; then
  echo "ncoreobj_inline_bc_fallback FAIL — degraded build reported core-obj stats (split engaged?)"
  cat "$PROJ/deg.err"; exit 1
fi

echo "ncoreobj_inline_bc_fallback OK — split engages with both bitcodes; missing inline bc falls back to the whole-program merge with a note"
exit 0
